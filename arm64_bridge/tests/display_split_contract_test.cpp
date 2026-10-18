// SPDX-License-Identifier: GPL-3.0-or-later
#include "wolf_display_overrides.h"
#include <jni.h>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int IndexApks(const char* const*, size_t);
extern "C" int TelltaleWidescreenOverrideEnabled();
JavaVM* GetHostJavaVM() { return nullptr; }

static unsigned checks = 0, failures = 0;
static void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}
static unsigned Fds() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return UINT32_MAX;
    unsigned count = 0;
    while (const auto* entry = readdir(dir)) if (entry->d_name[0] != '.') ++count;
    closedir(dir);
    return count;
}

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    DynarmicHost host;
    if (!host.Initialize()) return 3;
    constexpr const char* names[]{"libfmodex.so", "libSDL2.so", "libGameEngine.so", "libmain.so"};
    std::array<std::vector<uint8_t>, 4> images;
    std::vector<GuestModuleImage> modules;
    for (size_t i = 0; i < images.size(); ++i) {
        std::ifstream input(std::string(argv[1]) + "/" + names[i], std::ios::binary);
        images[i] = std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
        if (!input || images[i].empty()) return 4;
        modules.push_back({names[i], images[i].data(), images[i].size()});
    }
    if (!host.LoadModulesFromMemory(modules)) return 5;
    const auto* engine = host.FindModule("libGameEngine.so");
    if (!engine) return 6;
    const uint32_t base = engine->GetBaseAddress();
    const uint32_t wrapper = base + 0x90E38Cu, force = base + 0xC75981u;
    const uint32_t quality = base + 0xC754B8u, low_tier = base + 0x438980u;
    host.MemoryWrite32(wrapper, host.MemoryRead32(wrapper) ^ 1u);
    Check(!InstallWolfDisplayOverrides(host, true), "changed wrapper rejected");
    Check(host.MemoryRead32(wrapper + 48) == 0xE3A00000u && host.MemoryRead8(force) == 1 &&
          host.MemoryRead32(low_tier) == 0xE3A0243Fu, "failed guard performs no partial writes");
    host.MemoryWrite32(wrapper, host.MemoryRead32(wrapper) ^ 1u);
    host.MemoryWrite32(quality, 0x3F000000u);
    Check(!InstallWolfDisplayOverrides(host, false), "changed initial scalar rejected");
    host.MemoryWrite32(quality, 0x3F800000u);
    Check(InstallWolfDisplayOverrides(host, false), "base display selector accepted");
    Check(host.MemoryRead32(wrapper + 48) == 0xE3A00001u && host.MemoryRead8(force) == 1 &&
          host.MemoryRead32(low_tier) == 0xE3A025FEu, "base forces16:9 and removes0.5 downscale");
    host.MemoryWrite32(wrapper + 48, 0xE3A00000u);
    host.MemoryWrite32(low_tier, 0xE3A0243Fu);
    Check(InstallWolfDisplayOverrides(host, true), "wide display selector accepted");
    Check(host.MemoryRead32(wrapper + 48) == 0xE3A00000u && host.MemoryRead8(force) == 0 &&
          host.MemoryRead32(low_tier) == 0xE3A025FEu, "wide uses physical aspect and removes0.5 downscale");
    const char* plain[]{argv[2]};
    const char* wide[]{argv[2], argv[3]};
    const char* invalid[]{argv[2], argv[4]};
    Check(IndexApks(plain, 1) == 0 && !TelltaleWidescreenOverrideEnabled(), "base index has no marker");
    Check(IndexApks(wide, 2) == 0 && TelltaleWidescreenOverrideEnabled(), "empty stored marker activates wide");
    const unsigned before = Fds();
    for (unsigned i = 0; i < 32; ++i) Check(IndexApks(wide, 2) == 0, "marker reindex succeeds");
    Check(before != UINT32_MAX && Fds() == before, "marker APK reindex does not leak descriptors");
    Check(IndexApks(invalid, 2) != 0 && !TelltaleWidescreenOverrideEnabled(), "nonempty marker rejected and selector reset");
    Check(IndexApks(plain, 1) == 0 && !TelltaleWidescreenOverrideEnabled(), "removing split restores default");
    std::printf("DISPLAY_SPLIT_CONTRACTS checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
