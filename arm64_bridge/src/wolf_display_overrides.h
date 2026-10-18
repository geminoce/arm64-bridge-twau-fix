// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dynarmic_host.h"
#include "elf_loader.h"
#include <array>

inline bool InstallWolfDisplayOverrides(DynarmicHost& host, bool wide) {
    const auto* engine = host.FindModule("libGameEngine.so");
    const auto* wrapper = engine ? engine->GetExportedSymbol(
        "_Z34luaRenderForce_16_by_9_AspectRatioP9lua_State") : nullptr;
    const auto* force = engine ? engine->GetExportedSymbol(
        "_ZN12RenderDevice18sbForceAspectRatioE") : nullptr;
    const auto* screen = engine ? engine->GetExportedSymbol(
        "_ZN15Application_SDL11ScaleScreenEv") : nullptr;
    const auto* quality = engine ? engine->GetExportedSymbol(
        "_ZN13RenderTexture15smQualityScalarE") : nullptr;
    auto exact = [&](const ElfSymbol* symbol, uint32_t rva, uint32_t size,
                     uint8_t type, uint32_t flags) {
        if (!engine || !symbol || symbol->type != type || symbol->size != size ||
            symbol->value != static_cast<uint64_t>(engine->GetBaseAddress()) + rva)
            return false;
        const uint64_t end = static_cast<uint64_t>(symbol->value) + size;
        if (end > host.GetGuestMemorySize()) return false;
        for (const auto& segment : engine->GetSegments()) {
            const uint64_t start = static_cast<uint64_t>(engine->GetBaseAddress()) + segment.vaddr;
            if (segment.flags == flags && symbol->value >= start &&
                end <= start + segment.filesz) return true;
        }
        return false;
    };
    constexpr std::array<uint32_t, 20> accepted = {
        0xE92D4010u, 0xE1A04000u, 0xEB022E49u, 0xE1A00004u,
        0xE3A01002u, 0xEB022DFAu, 0xE1A00004u, 0xE3A01001u,
        0xEB023350u, 0xE1A00004u, 0xE3A01000u, 0xEB022E45u,
        0xE3A00000u, 0xEBFB548Fu, 0xE3080E39u, 0xE3430FE3u,
        0xEBFB547Du, 0xE1A00004u, 0xE8BD4010u, 0xEA022E38u,
    };
    constexpr std::array<uint32_t, 9> low_tier = {
        0xE59F3054u, 0xE3A00001u, 0xE3A0243Fu,
        0xE79F3003u, 0xE5832000u, 0xEB0EAA40u,
        0xE3A02E1Eu, 0xE3001356u, 0xEAFFFFACu,
    };
    if (!exact(wrapper, 0x90E38Cu, sizeof(accepted), STT_FUNC, PF_R | PF_X) ||
        !exact(force, 0xC75981u, 1, STT_OBJECT, PF_R | PF_W) ||
        !exact(screen, 0x438770u, 616, STT_FUNC, PF_R | PF_X) ||
        !exact(quality, 0xC754B8u, 4, STT_OBJECT, PF_R | PF_W)) return false;
    for (size_t i = 0; i < accepted.size(); ++i)
        if (host.MemoryRead32(wrapper->value + static_cast<uint32_t>(i * 4)) != accepted[i])
            return false;
    for (size_t i = 0; i < low_tier.size(); ++i)
        if (host.MemoryRead32(screen->value + 0x208u + static_cast<uint32_t>(i * 4)) != low_tier[i])
            return false;
    if (host.MemoryRead8(force->value) != 1 ||
        host.MemoryRead32(quality->value) != 0x3F800000u ||
        host.MemoryRead32(screen->value) != 0xE92D43F0u ||
        host.MemoryRead32(screen->value + 4u) != 0xE24DD00Cu ||
        host.MemoryRead32(screen->value + 0xFCu) != 0xEA000015u) return false;

    // The stored guest is already wide; select the original force argument on each cold bootstrap.
    host.MemoryWrite32(wrapper->value + 48u, wide ? 0xE3A00000u : 0xE3A00001u);
    host.MemoryWrite8(force->value, wide ? 0 : 1);
    host.MemoryWrite32(screen->value + 0x210u, 0xE3A025FEu);
    host.InvalidateGuestCode(wrapper->value + 48u, 4);
    host.InvalidateGuestCode(screen->value + 0x210u, 4);
    return true;
}
