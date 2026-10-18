// SPDX-License-Identifier: GPL-3.0-or-later
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "game_port_config.h"
#include "thunk_manager.h"
#include "thunks_opensles.h"
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#endif
#include <cstring>
#include <algorithm>
#include <elf.h>
#include <android/log.h>
#include <unordered_map>
#include <vector>

void ClearGuestJumpStates(const DynarmicHost* host, uint64_t thread_generation);
void ClearGuestFiles(const DynarmicHost* host);
void ClearGuestCxa(const DynarmicHost* host);
void ClearGuestContracts(const DynarmicHost* host);

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define LOG_TAG "Arm32Bridge_Host"
#if ARM32_BRIDGE_ENABLE_LOGGING
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

#define EXIT_TRAP_ARM   0x00000004
#define EXIT_TRAP_THUMB 0x00000008

namespace {

#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
constexpr uint32_t kFmodProbeTagBase = 0x574F4600u; // Above all ordinary thunk IDs.
#endif
std::atomic<uint64_t> g_next_thread_generation{1};
thread_local const uint64_t g_thread_generation = g_next_thread_generation.fetch_add(1, std::memory_order_relaxed);
#if ARM32_BRIDGE_ENABLE_HEAP_DIAGNOSTICS
struct HeapTrace {
    uint64_t total_bytes = 0;
    uint64_t total_capacity = 0;
    uint64_t allocs = 0;
    uint64_t live_bytes = 0;
    uint64_t live_blocks = 0;
    bool first_large_logged = false;
};
std::unordered_map<uint32_t, HeapTrace> g_heap_trace;
#endif

std::string BaseSymbolName(const std::string& name) {
    const size_t version = name.find('@');
    return version == std::string::npos ? name : name.substr(0, version);
}

bool ExportMatchesImport(uint8_t import_type, uint8_t export_type) {
    if (import_type == STT_OBJECT) return export_type == STT_OBJECT;
    if (import_type == STT_FUNC) return export_type == STT_FUNC || export_type == STT_NOTYPE;
    return import_type == STT_NOTYPE &&
           (export_type == STT_FUNC || export_type == STT_OBJECT || export_type == STT_NOTYPE);
}
}

thread_local DynarmicHost::JitContext* DynarmicHost::s_active_context = nullptr;

DynarmicHost::DynarmicHost() = default;

DynarmicHost::~DynarmicHost() {
    ShutdownOpenSLES(this);
    ClearArm32TmZoneCache(this);
    ClearGuestJumpStates(this, 0);
    ClearGuestFiles(this);
    ClearGuestCxa(this);
    ClearGuestContracts(this);
    {
        std::lock_guard<std::mutex> lock(m_jit_mutex);
        m_jit_contexts.clear();
    }
    if (m_memory) {
#if defined(_WIN32)
        VirtualFree(m_memory, 0, MEM_RELEASE);
#else
        munmap(m_memory, m_memory_size);
#endif
        m_memory = nullptr;
    }
}

// Reserve guest memory. MAP_NORESERVE keeps the 4 GiB reservation virtual: untouched pages
// cost nothing and only written pages are ever committed.
static uint8_t* MapGuestMemory(size_t size) {
#if defined(_WIN32)
    return reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : reinterpret_cast<uint8_t*>(p);
#endif
}

bool DynarmicHost::Initialize() {
    if (m_memory || HasFailed() || HasExited()) return false;
    // Mapping the whole 32-bit space lets the JIT emit a single `ldr Wt, [Xfastmem, Waddr, uxtw]`
    // per guest access instead of a call into MemoryRead*/MemoryWrite*.
    m_memory_size = GUEST_ADDRESS_SPACE;
    m_memory = MapGuestMemory(m_memory_size);

    bool fastmem = m_memory != nullptr;
    if (m_memory == nullptr) {
        LOGE("Could not reserve %zu bytes for the guest address space; "
             "falling back to %zu bytes without fastmem.", GUEST_ADDRESS_SPACE, GUEST_FALLBACK_SIZE);
        m_memory_size = GUEST_FALLBACK_SIZE;
        m_memory = MapGuestMemory(m_memory_size);
        if (!m_memory) {
            LOGE("Failed to allocate guest memory (%zu bytes)", m_memory_size);
            m_memory_size = 0;
            return false;
        }
    }

    if (!HeapSelfCheck()) {
        Fail("HeapSelfCheck FAILED");
        return false;
    }

    // The null code address is permanently undefined in both instruction sets:
    // ARM UDF e7f0def0, low Thumb halfword UDF def0. MemoryReadCode preserves it
    // even if a bad guest data write changes the anonymous low-page backing.
    *reinterpret_cast<uint32_t*>(m_memory) = 0xE7F0DEF0u;

    // ARM exit trap at 0x00000004: SVC 0 (0xEF000000)
    *reinterpret_cast<uint32_t*>(m_memory + EXIT_TRAP_ARM) = 0xEF000000;

    // Thumb exit trap at 0x00000008: SVC 0 (0xDF00, 0xDF00)
    *reinterpret_cast<uint32_t*>(m_memory + EXIT_TRAP_THUMB) = 0xDF00DF00;

    m_thunk_manager = std::make_unique<ThunkManager>(*this);
    m_thunk_manager->RegisterAllThunks();
    if (HasFailed() || HasExited()) return false;

    // SDL, FMOD and the game execute on distinct host threads. Each host thread gets its
    // own Dynarmic CPU context while sharing guest memory and the exclusive monitor.
    m_fastmem = fastmem;
    m_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxJitContexts);
    if (!PrepareThreadContext()) {
        Fail("Initial guest CPU context unavailable");
        return false;
    }
    LOGI("Dynarmic Host initialized: %zu bytes guest space, fastmem %s",
         m_memory_size, fastmem ? "ON" : "OFF");
    return true;
}

DynarmicHost::JitContext& DynarmicHost::GetThreadContext() {
    const std::thread::id id = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    auto it = m_jit_contexts.find(id);
    if (it != m_jit_contexts.end()) {
        JitContext& context = *it->second;
        // OS thread identifiers may be reused after pthread/std::thread exit.
        // Reuse the bounded slot, but give the new thread fresh CPU/TLS state.
        if (context.thread_generation != g_thread_generation) {
            ClearGuestJumpStates(this, context.thread_generation);
            if (context.jit) context.jit->Reset();
            MemoryWrite32(context.errno_addr, 0);
            context.result_hi = 0;
            context.last_boundary.store(0, std::memory_order_relaxed);
            context.running = false;
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
            context.fmod_pending_valid = {};
#endif
            context.thread_generation = g_thread_generation;
        }
        return context;
    }

    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    config.global_monitor = m_monitor.get();
    config.processor_id = m_jit_contexts.size(); // ExclusiveMonitor requires a unique slot per JIT CPU.
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.enable_cycle_counting = false;
    config.always_little_endian = true;
    config.define_unpredictable_behaviour = true;
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    // An injected, non-terminal supervisor observer must see materialized state.
    // Both enabled and uninstrumented ON controls use this exact configuration.
    config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
#endif
    if (m_fastmem) config.fastmem_pointer = reinterpret_cast<uintptr_t>(m_memory);

    auto context = std::make_unique<JitContext>();
    context->owner = this;
    context->thread_generation = g_thread_generation;
    if (!m_monitor || config.processor_id >= kMaxJitContexts ||
        config.processor_id >= m_monitor->GetProcessorCount()) {
        LOGE("Guest CPU context capacity exceeded at slot %zu; execution refused", config.processor_id);
        return *m_jit_contexts.emplace(id, std::move(context)).first->second;
    }
    context->errno_addr = kGuestTlsBase +
                          static_cast<uint32_t>(config.processor_id) * kGuestTlsStride;
    MemoryWrite32(context->errno_addr, 0);
    context->jit = std::make_unique<Dynarmic::A32::Jit>(config);
    constexpr uint32_t kStackStride = 0x00200000;
    const uint32_t old_top = m_next_stack_top.fetch_sub(kStackStride);
    context->stack_top = old_top > 0x32200000 ? old_top : 0;
    if (!context->stack_top) LOGE("Out of guest thread stack space.");
    JitContext* raw = context.get();
    m_jit_contexts.emplace(id, std::move(context));
    LOGI("Created guest CPU context with stack top 0x%08X processor=%zu", raw->stack_top, config.processor_id);
    return *raw;
}

bool DynarmicHost::PrepareThreadContext() {
    if (HasFailed() || HasExited()) return false;
    JitContext& context = GetThreadContext();
    return context.jit && context.stack_top;
}

bool DynarmicHost::IsJitRunning() const {
    return s_active_context && s_active_context->running;
}

uint32_t DynarmicHost::CurrentGuestErrnoAddress() const {
    return s_active_context && s_active_context->running && s_active_context->owner == this
        ? s_active_context->errno_addr : 0;
}

void DynarmicHost::LogGuestExecution() const {
    // Temporary startup-stall trace: sample atomics, never a running JIT's registers.
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    for (const auto& [id, context] : m_jit_contexts) {
        const uint64_t boundary = context->last_boundary.load(std::memory_order_relaxed);
        if (!boundary) continue;
        const uint32_t svc = static_cast<uint32_t>(boundary >> 32) & 0x7FFFFFFFu;
        LOGI("Guest execution: stack=0x%08X svc=%u thunk=0x%08X LR=0x%08X inHost=%u",
             context->stack_top, svc, svc ? 0x10000 + (svc - 1) * 8 : 0,
             static_cast<uint32_t>(boundary), static_cast<unsigned>(boundary >> 63));
    }
}

uint32_t DynarmicHost::GetResultHigh() const {
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    auto it = m_jit_contexts.find(std::this_thread::get_id());
    return it != m_jit_contexts.end() ? it->second->result_hi : 0;
}

namespace {
constexpr uint32_t kModuleBase = 0x40000000;
constexpr uint32_t kModuleLimit = 0x50000000;
constexpr uint32_t kModuleAlignment = 0x10000;

bool InspectModuleSpan(const uint8_t* data, size_t size, uint32_t& min_vaddr, uint32_t& max_vaddr) {
    if (!data || size < sizeof(Elf32_Ehdr)) return false;
    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(data);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr->e_machine != EM_ARM || ehdr->e_phentsize != sizeof(Elf32_Phdr) ||
        static_cast<uint64_t>(ehdr->e_phoff) + static_cast<uint64_t>(ehdr->e_phnum) * sizeof(Elf32_Phdr) > size) {
        return false;
    }
    const auto* phdrs = reinterpret_cast<const Elf32_Phdr*>(data + ehdr->e_phoff);
    min_vaddr = UINT32_MAX;
    max_vaddr = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        const auto& ph = phdrs[i];
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_filesz > ph.p_memsz || static_cast<uint64_t>(ph.p_offset) + ph.p_filesz > size ||
            static_cast<uint64_t>(ph.p_vaddr) + ph.p_memsz > UINT32_MAX) return false;
        min_vaddr = std::min(min_vaddr, ph.p_vaddr);
        max_vaddr = std::max(max_vaddr, ph.p_vaddr + ph.p_memsz);
    }
    return min_vaddr != UINT32_MAX && max_vaddr > min_vaddr;
}
}

bool DynarmicHost::LoadModulesFromMemory(const std::vector<GuestModuleImage>& modules) {
    if (HasFailed() || HasExited() || !m_memory || modules.empty() || !m_modules.empty()) {
        LOGE("LoadModulesFromMemory requires one non-empty initial module set.");
        return false;
    }

    uint32_t cursor = kModuleBase;
    for (const GuestModuleImage& image : modules) {
        uint32_t min_vaddr = 0, max_vaddr = 0;
        if (image.name.empty() || FindModule(image.name) || !InspectModuleSpan(image.data, image.size, min_vaddr, max_vaddr)) {
            LOGE("Invalid guest module: %s", image.name.c_str());
            return false;
        }
        const uint32_t base = (cursor + kModuleAlignment - 1) & ~(kModuleAlignment - 1);
        const uint64_t end = static_cast<uint64_t>(base) + max_vaddr;
        if (end > std::min(kModuleLimit, kHeapBase)) {
            LOGE("Guest modules exceed reserved module space at %s", image.name.c_str());
            return false;
        }

        auto loader = std::make_unique<ElfLoader>(m_memory, base, m_memory_size);
        if (!loader->LoadFromMemory(image.data, image.size)) {
            LOGE("Failed to load guest module: %s", image.name.c_str());
            return false;
        }
        for (const GuestModule& existing : m_modules) {
            if (loader->GetImageStart() < existing.loader->GetImageEnd() &&
                existing.loader->GetImageStart() < loader->GetImageEnd()) {
                LOGE("Guest module overlap: %s with %s", image.name.c_str(), existing.name.c_str());
                return false;
            }
        }
        LOGI("Guest module %s at 0x%08X..0x%08X", image.name.c_str(),
             loader->GetImageStart(), loader->GetImageEnd());
        cursor = static_cast<uint32_t>((end + kModuleAlignment - 1) & ~(static_cast<uint64_t>(kModuleAlignment) - 1));
        m_modules.push_back({image.name, std::move(loader)});
    }

    bool dependencies_ok = true;
    for (size_t i = 0; i < m_modules.size(); ++i) {
        const auto& module = m_modules[i];
        for (const auto& needed : module.loader->GetNeededLibraries()) {
            const ElfLoader* guest = FindModule(needed);
            LOGI("Dependency module='%s' needed='%s' provider=%s", module.name.c_str(), needed.c_str(),
                 guest ? "guest" : ThunkManager::SupportsLibrary(needed) ? "bridge" : "MISSING");
            if (!guest && !ThunkManager::SupportsLibrary(needed)) dependencies_ok = false;
            for (size_t j = i + 1; j < m_modules.size(); ++j) {
                if (m_modules[j].name == needed) {
                    LOGE("Guest initializer dependency order invalid: %s before %s", module.name.c_str(), needed.c_str());
                    dependencies_ok = false;
                }
            }
        }
    }

    uint32_t guest_ctype_object = 0;
    uint32_t guest_toupper_object = 0;
    uint32_t guest_tolower_object = 0;
    auto resolve_ctype_data = [this, &guest_ctype_object, &guest_toupper_object, &guest_tolower_object](const ElfImport& import) -> uint32_t {
        if (import.type != STT_OBJECT) return 0;
        const std::string base_name = BaseSymbolName(import.name);
        if (base_name == "_ctype_") {
            if (!guest_ctype_object) {
                const uint32_t table = GuestAlloc(384, 16);
                const uint32_t object = GuestAlloc(4, 4);
                if (!table || !object) return 0;
                for (uint32_t i = 0; i < 384; ++i) MemoryWrite8(table + i, 0);
                for (uint32_t c = 0; c < 256; ++c) {
                    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
                    const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z';
                    const bool digit = c >= '0' && c <= '9';
                    const bool hex = digit || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
                    const bool control = c < 32 || c == 127;
                    const bool punct = c >= 33 && c <= 126 && !upper && !lower && !digit;
                    const uint8_t flags = (upper ? 1 : 0) | (lower ? 2 : 0) | (digit ? 4 : 0) |
                        (space ? 8 : 0) | (punct ? 16 : 0) | (control ? 32 : 0) | (hex ? 64 : 0) |
                        (c == ' ' ? 128 : 0);
                    MemoryWrite8(table + 128 + c, flags);
                }
                MemoryWrite32(object, table + 127);
                guest_ctype_object = object;
                m_thunk_manager->RegisterDataSymbol("_ctype_", guest_ctype_object, {"libc.so"});
                LOGI("Provided guest _ctype_ object=0x%08X table=0x%08X", object, table + 127);
            }
            return guest_ctype_object;
        }
        if (base_name == "_toupper_tab_") {
            if (!guest_toupper_object) {
                const uint32_t table = GuestAlloc(384 * sizeof(uint16_t), 16);
                const uint32_t object = GuestAlloc(4, 4);
                if (!table || !object) return 0;
                for (uint32_t c = 0; c < 384; ++c) MemoryWrite16(table + c * 2, 0);
                for (uint32_t c = 0; c < 256; ++c) {
                    uint16_t upper = static_cast<uint16_t>(c);
                    if (c >= 'a' && c <= 'z') upper = static_cast<uint16_t>(c - ('a' - 'A'));
                    MemoryWrite16(table + (128 + c) * 2, upper);
                }
                MemoryWrite16(table + 127 * 2, 0xFFFF); // EOF
                MemoryWrite32(object, table + 127 * 2);
                guest_toupper_object = object;
                m_thunk_manager->RegisterDataSymbol("_toupper_tab_", guest_toupper_object, {"libc.so"});
                LOGI("Provided guest _toupper_tab_ object=0x%08X table=0x%08X", object, table + 127 * 2);
            }
            return guest_toupper_object;
        }
        if (base_name == "_tolower_tab_") {
            if (!guest_tolower_object) {
                const uint32_t table = GuestAlloc(384 * sizeof(uint16_t), 16);
                const uint32_t object = GuestAlloc(4, 4);
                if (!table || !object) return 0;
                for (uint32_t c = 0; c < 384; ++c) MemoryWrite16(table + c * 2, 0);
                for (uint32_t c = 0; c < 256; ++c) {
                    uint16_t lower = static_cast<uint16_t>(c);
                    if (c >= 'A' && c <= 'Z') lower = static_cast<uint16_t>(c + ('a' - 'A'));
                    MemoryWrite16(table + (128 + c) * 2, lower);
                }
                MemoryWrite16(table + 127 * 2, 0xFFFF); // EOF
                MemoryWrite32(object, table + 127 * 2);
                guest_tolower_object = object;
                m_thunk_manager->RegisterDataSymbol("_tolower_tab_", guest_tolower_object, {"libc.so"});
                LOGI("Provided guest _tolower_tab_ object=0x%08X table=0x%08X", object, table + 127 * 2);
            }
            return guest_tolower_object;
        }
        return 0;
    };
    LOGI("Guest resolver scope=global-load-group (supplied module order), exact versions, typed bridge fallback");
    auto resolver = [this, &resolve_ctype_data](const ElfImport& import) -> uint32_t {
        const std::string& name = import.name;
        if ((import.bind != STB_GLOBAL && import.bind != STB_WEAK) ||
            (import.visibility != STV_DEFAULT && import.visibility != STV_PROTECTED) ||
            (import.type != STT_OBJECT && import.type != STT_FUNC && import.type != STT_NOTYPE)) return 0;
        const uint8_t type = import.type == STT_NOTYPE && import.relocation_type == R_ARM_JUMP_SLOT
            ? STT_FUNC : import.type;
        for (const GuestModule& module : m_modules) {
            if (!import.version_library.empty() && module.name != import.version_library) continue;
            if (const ElfSymbol* symbol = module.loader->GetExportedSymbol(name, import.version)) {
                if (ExportMatchesImport(type, symbol->type)) return symbol->value;
                LOGI("Rejected incompatible guest candidate module='%s' name='%s' import-type=%u export-type=%u",
                     module.name.c_str(), name.c_str(), type, symbol->type);
            }
        }
        if (ThunkManager::SupportsVersion(import.version_library, import.version) &&
            (import.version_library.empty() || import.version_library == "libc.so")) resolve_ctype_data(import);
        if (type == STT_FUNC || type == STT_NOTYPE) {
            if (uint32_t provider = m_thunk_manager->FindProvider(name, ThunkManager::ProviderKind::Function,
                                                               import.version, import.version_library)) return provider;
        }
        if (type == STT_OBJECT || type == STT_NOTYPE) {
            if (uint32_t provider = m_thunk_manager->FindProvider(name, ThunkManager::ProviderKind::Object,
                                                               import.version, import.version_library)) return provider;
        }
        return 0; // Unresolved weak imports remain zero; the loader reports every strong failure.
    };
    bool relocated = dependencies_ok;
    for (GuestModule& module : m_modules) {
        if (!module.loader->Relocate(resolver)) {
            LOGE("Failed to relocate guest module: %s", module.name.c_str());
            relocated = false;
        }
    }
    if (!relocated) {
        LOGE("Guest load-group audit FAILED; no initializers may run.");
        return false;
    }
    for (const GuestModule& module : m_modules) {
        for (const ElfSegment& seg : module.loader->GetSegments()) {
            if ((seg.flags & PF_X) != 0) AddExecutableRange(module.loader->GetBaseAddress() + seg.vaddr, seg.memsz);
        }
    }
    m_load_group_ready = true;
    LOGI("Loaded and relocated %zu guest modules.", m_modules.size());
    return true;
}

bool DynarmicHost::RunModuleInitializers() {
    if (HasFailed() || HasExited() || !m_load_group_ready) {
        LOGE("Initializers refused: guest load-group audit has not passed.");
        return false;
    }
    for (GuestModule& module : m_modules) {
        if (uint32_t init = module.loader->GetInitFunction()) {
            LOGI("Running DT_INIT for %s at 0x%08X", module.name.c_str(), init);
            RunFunction(init);
            if (HasFailed() || HasExited()) return false;
        }
        LOGI("Running %zu init-array entries for %s", module.loader->GetInitArray().size(), module.name.c_str());
        for (uint32_t init : module.loader->GetInitArray()) {
            RunFunction(init);
            if (HasFailed() || HasExited()) return false;
        }
    }
    return true;
}

ElfLoader* DynarmicHost::FindModule(const std::string& name) {
    for (GuestModule& module : m_modules) if (module.name == name) return module.loader.get();
    return nullptr;
}

const ElfLoader* DynarmicHost::FindModule(const std::string& name) const {
    for (const GuestModule& module : m_modules) if (module.name == name) return module.loader.get();
    return nullptr;
}

ElfLoader& DynarmicHost::GetElfLoader() {
    return *m_modules.back().loader;
}

uint32_t DynarmicHost::FindSymbol(const std::string& name) {
    for (const GuestModule& module : m_modules) {
        if (uint32_t addr = module.loader->GetSymbolAddress(name)) return addr;
    }
    return 0;
}

uint32_t DynarmicHost::RunFunction(uint32_t target_pc, const std::vector<uint32_t>& args) {
    if (HasFailed() || HasExited()) return 0;
    JitContext& context = GetThreadContext();
    if (!context.jit || !context.stack_top || !target_pc) {
        Fail("Invalid guest function/context");
        return 0;
    }
    // Each CPU context owns a 2 MiB stack; leave room for guest frames as well
    // as the AAPCS spill area rather than truncating a size_t argument count.
    if (args.size() > 4 + (0x00200000u - 16u) / sizeof(uint32_t)) {
        Fail("Guest call arguments exceed context stack");
        return 0;
    }
    if (context.running) {
        std::lock_guard<std::mutex> lock(m_deferred_mutex);
        m_deferred.emplace_back(target_pc, args);
        return 0;
    }

#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    {
        std::lock_guard<std::mutex> lock(m_fmod_probe_mutex);
        for (bool pending : context.fmod_pending_valid)
            if (pending) ++m_fmod_probe_summary.rejected_pairs;
        context.fmod_pending_valid = {};
    }
#endif
    context.running = true;
    JitContext* previous = s_active_context;
    s_active_context = &context;
    struct JitRunningGuard {
        JitContext& context;
        JitContext*& active;
        JitContext* previous;
        ~JitRunningGuard() {
#if ARM32_BRIDGE_ENABLE_RUNTIME_WATCHDOG
            context.last_boundary.store(0, std::memory_order_relaxed);
#endif
            context.running = false;
            active = previous;
        }
    } running_guard{context, s_active_context, previous};

    Dynarmic::A32::Jit& jit = *context.jit;
    for (size_t i = 0; i < 13; ++i) jit.Regs()[i] = 0;
    for (size_t i = 0; i < args.size() && i < 4; ++i) jit.Regs()[i] = args[i];
    jit.Regs()[13] = context.stack_top;

    if (args.size() > 4) {
        const size_t extra_args = args.size() - 4;
        const uint32_t sp = context.stack_top - static_cast<uint32_t>((extra_args * 4 + 7) & ~size_t{7});
        jit.Regs()[13] = sp;
        for (size_t i = 0; i < extra_args; ++i) MemoryWrite32(sp + static_cast<uint32_t>(i * 4), args[4 + i]);
    }

    const bool is_thumb = (target_pc & 1) != 0;
    // AAPCS stack arguments belong to the caller. A valid callee restores this
    // exact SP, which need not equal stack_top when there are more than 4 args.
    context.call_target = target_pc;
    context.call_sp = jit.Regs()[13];
    context.call_thumb = is_thumb;
    context.return_seen = false;
    jit.Regs()[14] = is_thumb ? (EXIT_TRAP_THUMB | 1) : EXIT_TRAP_ARM;
    jit.Regs()[15] = target_pc & ~1u;
    jit.SetCpsr((jit.Cpsr() & ~0x20u) | (is_thumb ? 0x20u : 0u));
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);

#if ARM32_BRIDGE_ENABLE_RUNTIME_WATCHDOG
    context.last_boundary.store(target_pc, std::memory_order_relaxed);
#endif
    Dynarmic::HaltReason hr;
    for (;;) {
        hr = jit.Run();
        if (!Dynarmic::Has(hr, Dynarmic::HaltReason::CacheInvalidation)) break;
        hr &= ~Dynarmic::HaltReason::CacheInvalidation;
        if (!!hr) break;
    }
    const uint32_t final_pc = jit.Regs()[15];
    context.result_hi = jit.Regs()[1];
    // Dynarmic's pinned ARM/Thumb SVC translators advance PC before CallSVC.
    // A low PC alone cannot prove return: an invalid virtual jump may land there.
    const uint32_t return_pc = context.call_thumb ? EXIT_TRAP_THUMB + 2 : EXIT_TRAP_ARM + 4;
    const bool normal_return = context.return_seen && final_pc == return_pc &&
                               bool(jit.Cpsr() & 0x20u) == context.call_thumb &&
                               jit.Regs()[13] == context.call_sp &&
                               hr == Dynarmic::HaltReason::UserDefined1;
    if (!normal_return && !HasFailed() && !HasExited()) {
        LogFailureSnapshot("unexpected JIT halt/return", final_pc);
        Fail("Unexpected guest JIT halt/return");
    }
    return jit.Regs()[0];
}

uint64_t DynarmicHost::CurrentThreadGeneration() const {
    return g_thread_generation;
}

void DynarmicHost::LogFailureSnapshot(const char* reason, uint32_t fault_pc) {
    // Failure-only path: one bounded snapshot per Host, including exception
    // callbacks, before Fail emits the harness's stop marker. No hot tracing.
    if (m_failure_snapshot_logged.exchange(true, std::memory_order_acq_rel)) return;
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    LogLastFmodPairOnFailure();
#endif
    JitContext* context = s_active_context;
    if (!context || context->owner != this || !context->running || !context->jit) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "[GUEST_SNAPSHOT] reason=%s fault=0x%08X no_active_context",
                            reason, fault_pc);
        return;
    }
    Dynarmic::A32::Jit& jit = *context->jit;
    const auto& regs = jit.Regs();
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "[GUEST_SNAPSHOT] reason=%s fault=0x%08X target=0x%08X entrySP=0x%08X top=0x%08X sentinelNext=0x%08X expectedThumb=%u returnSeen=%u CPSR=0x%08X FPSCR=0x%08X",
                        reason, fault_pc, context->call_target, context->call_sp, context->stack_top,
                        context->call_thumb ? EXIT_TRAP_THUMB + 2 : EXIT_TRAP_ARM + 4,
                        unsigned(context->call_thumb), unsigned(context->return_seen), jit.Cpsr(), jit.Fpscr());
    for (unsigned first = 0; first < 16; first += 4) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "[GUEST_SNAPSHOT] r%u..r%u=%08X,%08X,%08X,%08X",
                            first, first + 3, regs[first], regs[first + 1], regs[first + 2], regs[first + 3]);
    }
    // Every read uses uint64 arithmetic. Readability is not object validity.
    // Object words may change on other guest threads; this is not an atomic dump.
    auto read_word = [&](uint64_t address, uint64_t low, uint64_t high, uint32_t& value) {
        if (!m_memory || address < low || address + 4 > high ||
            address + 4 > GUEST_ADDRESS_SPACE || address + 4 > m_memory_size) return false;
        std::memcpy(&value, m_memory + static_cast<size_t>(address), sizeof(value));
        return true;
    };
    auto words = [&](const char* label, uint64_t address, unsigned count, uint64_t low, uint64_t high) {
        uint32_t values[4]{};
        unsigned valid = 0, executable = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!read_word(address + uint64_t(i) * 4, low, high, values[i])) continue;
            valid |= 1u << i;
            if (IsExecutableAddress(values[i] & ~1u)) executable |= 1u << i;
        }
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "[GUEST_SNAPSHOT] %s at=0x%llX count=%u valid=0x%X execValues=0x%X words=%08X,%08X,%08X,%08X",
                            label, static_cast<unsigned long long>(address), count, valid, executable,
                            values[0], values[1], values[2], values[3]);
    };
    const uint64_t stack_low = uint64_t(context->stack_top) - 0x00200000u;
    for (unsigned first = 0; first < 32; first += 4)
        words("stack", uint64_t(regs[13]) + first * 4, 4, stack_low, context->stack_top);
    words("R0_header", regs[0], 4, 0, GUEST_ADDRESS_SPACE);
    uint32_t r0_vptr = 0;
    const bool r0_vptr_read = read_word(regs[0], 0, GUEST_ADDRESS_SPACE, r0_vptr);
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                        "[GUEST_SNAPSHOT] R0_exec=%u R12=0x%08X R0_vptr_read=%u R0_vptr=0x%08X",
                        unsigned(IsExecutableAddress(regs[0] & ~1u)), regs[12], unsigned(r0_vptr_read), r0_vptr);
    words("R12_slots60_6c", uint64_t(regs[12]) + 0x60, 4, 0, GUEST_ADDRESS_SPACE);
    words("R12_slots70_74", uint64_t(regs[12]) + 0x70, 2, 0, GUEST_ADDRESS_SPACE);
    if (r0_vptr_read && r0_vptr != regs[12]) {
        words("R0_vptr_slots60_6c", uint64_t(r0_vptr) + 0x60, 4, 0, GUEST_ADDRESS_SPACE);
        words("R0_vptr_slots70_74", uint64_t(r0_vptr) + 0x70, 2, 0, GUEST_ADDRESS_SPACE);
    }
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[GUEST_SNAPSHOT] end");
}

void DynarmicHost::Fail(const char* reason) {
    if (!m_failed.exchange(true, std::memory_order_acq_rel)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Guest failure: %s", reason ? reason : "unknown");
    }
    if (s_active_context && s_active_context->owner == this && s_active_context->jit) {
        s_active_context->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
}

void DynarmicHost::ExitGuest(int32_t status) {
    if (!m_exit_requested.exchange(true, std::memory_order_acq_rel)) {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Guest exit requested: status=%d", status);
    }
    if (status != 0) Fail("Guest exited with nonzero status");
    if (s_active_context && s_active_context->owner == this && s_active_context->jit) {
        s_active_context->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
}

void DynarmicHost::InvalidateGuestCode(uint32_t addr, size_t size) {
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    for (auto& [id, context] : m_jit_contexts) {
        if (context->jit) context->jit->InvalidateCacheRange(addr, size);
    }
}

void DynarmicHost::DrainDeferred() {
    if (HasFailed() || HasExited() || IsJitRunning()) return;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> batch;
    {
        std::lock_guard<std::mutex> lock(m_deferred_mutex);
        batch.swap(m_deferred);
    }
    for (auto& call : batch) RunFunction(call.first, call.second);
}

// -------------------------------------------------------------
// Guest heap
//
// Every block carries a 16-byte header immediately before its payload:
//   [-16] magic  [-12] block base  [-8] capacity  [-4] payload size
// The magic is what makes GuestFree safe: a pointer that does not carry it is
// not ours (module data, an interior pointer, a stack top, something the guest
// invented) and is ignored -- exactly as a no-op free ignored everything.
// Freeing clears the magic, so a double free is ignored too.
//
// A plain bump allocator with an empty GuestFree is the usual first version and
// it is a guaranteed leak: the guest does call free(), nobody listens. Measured
// on a Scaleform title that was ~1 MB/s during gameplay.
// -------------------------------------------------------------
namespace {
constexpr uint32_t kBlockMagic = 0x41524D31; // 'ARM1' - live block
constexpr uint32_t kFreedMagic = 0x41524D46; // 'ARMF' - already on a free list
constexpr uint32_t kHeaderSize = 16;
}

uint32_t DynarmicHost::GuestAlloc(size_t size, size_t alignment) {
    if (!m_memory || !alignment || (alignment & (alignment - 1))) return 0;
    if (size == 0) size = 1;
    if (alignment < 8) alignment = 8;
    // Check before either size-class rounding or the uint32_t capacity conversion.
    if (alignment > UINT32_MAX - kHeaderSize - 15 ||
        size > UINT32_MAX - alignment - kHeaderSize - 15) return 0;
    // Worst case layout: header, then up to a full alignment step of padding.
    uint64_t need = static_cast<uint64_t>(size) + alignment + kHeaderSize;

    uint32_t base = 0, capacity = 0;
    bool recycled = false;
    {
        std::lock_guard<std::mutex> lk(m_heap_mutex);
        if (need <= kMaxClassSize) {
            int cls = 0;
            while ((1u << (cls + kMinClassShift)) < need) ++cls;
            capacity = 1u << (cls + kMinClassShift);
            auto& fl = m_free_small[cls];
            if (!fl.empty()) { base = fl.back(); fl.pop_back(); recycled = true; }
        } else {
            capacity = static_cast<uint32_t>((need + 15) & ~15ull);
            auto it = m_free_huge.find(capacity);
            if (it != m_free_huge.end() && !it->second.empty()) {
                base = it->second.back();
                it->second.pop_back();
                recycled = true;
            }
        }
        if (!recycled) {
            // Stop before the heap walks into the guest stacks instead of silently
            // handing out addresses that overlap them.
            if (static_cast<uint64_t>(m_heap_current) + capacity > std::min<uint64_t>(kHeapLimit, m_memory_size)) {
                const uint32_t pc = s_active_context && s_active_context->jit
                    ? s_active_context->jit->Regs()[15] : 0;
                const uint32_t sp = s_active_context && s_active_context->jit
                    ? s_active_context->jit->Regs()[13] : 0;
                const uint32_t lr = s_active_context && s_active_context->jit
                    ? s_active_context->jit->Regs()[14] : 0;
                const uint32_t thunk_pc = pc & ~7u;
                const char* thunk_name = "";
                if (m_thunk_manager) {
                    for (const auto& [name, address] : m_thunk_manager->GetThunkSymbolTable()) {
                        if (address == thunk_pc) { thunk_name = name.c_str(); break; }
                    }
                }
                LOGE("GuestAlloc out of guest heap: at 0x%08X, want %u, limit 0x%08X pc=0x%08X lr=0x%08X sp=0x%08X thunk='%s' live=%llu blocks=%llu allocs=%llu frees=%llu",
                     m_heap_current, capacity, kHeapLimit, pc, lr, sp, thunk_name,
                     static_cast<unsigned long long>(m_live_bytes),
                     static_cast<unsigned long long>(m_live_blocks),
                     static_cast<unsigned long long>(m_total_allocs),
                     static_cast<unsigned long long>(m_total_frees));
                return 0;
            }
            base = m_heap_current;
            m_heap_current = base + capacity;
        }
        m_live_bytes += size;
        ++m_live_blocks;
        ++m_total_allocs;
#if ARM32_BRIDGE_ENABLE_HEAP_DIAGNOSTICS
        const uint32_t trace_size = static_cast<uint32_t>(std::min<uint64_t>(size, UINT32_MAX));
        auto& trace = g_heap_trace[trace_size];
        trace.total_bytes += size;
        trace.total_capacity += capacity;
        ++trace.allocs;
        trace.live_bytes += size;
        ++trace.live_blocks;
        if (size >= 65536 && !trace.first_large_logged) {
            const uint32_t pc = s_active_context && s_active_context->jit ? s_active_context->jit->Regs()[15] : 0;
            const uint32_t lr = s_active_context && s_active_context->jit ? s_active_context->jit->Regs()[14] : 0;
            const uint32_t sp = s_active_context && s_active_context->jit ? s_active_context->jit->Regs()[13] : 0;
            const uint32_t stack_lr = sp <= UINT32_MAX - 32 && static_cast<uint64_t>(sp) + 32 <= m_memory_size
                ? MemoryRead32(sp + 28) : 0;
            const uint32_t stack_caller = sp <= UINT32_MAX - 48 && static_cast<uint64_t>(sp) + 48 <= m_memory_size
                ? MemoryRead32(sp + 44) : 0;
            const uint32_t stack_caller2 = sp <= UINT32_MAX - 64 && static_cast<uint64_t>(sp) + 64 <= m_memory_size
                ? MemoryRead32(sp + 60) : 0;
            LOGI("GuestHeap first-large requested=%u pc=0x%08X lr=0x%08X sp=0x%08X stackLR=0x%08X stackCaller=0x%08X stackCaller2=0x%08X",
                 trace_size, pc, lr, sp, stack_lr, stack_caller, stack_caller2);
            trace.first_large_logged = true;
        }
        if ((m_total_allocs & 4095u) == 0) {
            std::vector<std::pair<uint64_t, uint32_t>> top;
            top.reserve(g_heap_trace.size());
            for (const auto& [requested, stats] : g_heap_trace) {
                if (stats.live_bytes) top.emplace_back(stats.live_bytes, requested);
            }
            std::sort(top.rbegin(), top.rend());
            LOGI("GuestHeap milestone allocs=%llu frees=%llu live=%llu blocks=%llu cursor=0x%08X classes=%zu",
                 static_cast<unsigned long long>(m_total_allocs),
                 static_cast<unsigned long long>(m_total_frees),
                 static_cast<unsigned long long>(m_live_bytes),
                 static_cast<unsigned long long>(m_live_blocks), m_heap_current, top.size());
            for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
                const auto& stats = g_heap_trace[top[i].second];
                LOGI("GuestHeap live-size #%zu requested=%u bytes=%llu blocks=%llu total=%llu capacity=%llu",
                     i + 1, top[i].second,
                     static_cast<unsigned long long>(stats.live_bytes),
                     static_cast<unsigned long long>(stats.live_blocks),
                     static_cast<unsigned long long>(stats.total_bytes),
                     static_cast<unsigned long long>(stats.total_capacity));
            }
        }
#endif
    }

    uint32_t payload = static_cast<uint32_t>(
        (static_cast<uint64_t>(base) + kHeaderSize + alignment - 1) & ~(static_cast<uint64_t>(alignment) - 1));
    if (recycled && static_cast<uint64_t>(payload) + size <= m_memory_size) {
        // Fresh arena memory has always been zero, and guest code that leaned on
        // that would break in confusing ways the first time it saw a recycled
        // block. Cheap insurance at the observed churn.
        memset(m_memory + payload, 0, size);
    }
    MemoryWrite32(payload - 16, kBlockMagic);
    MemoryWrite32(payload - 12, base);
    MemoryWrite32(payload - 8, capacity);
    MemoryWrite32(payload - 4, static_cast<uint32_t>(size));
    return payload;
}

void DynarmicHost::GuestFree(uint32_t addr) {
    std::lock_guard<std::mutex> lk(m_heap_mutex);
    if (!IsLiveHeapBlock(addr)) {
#if ARM32_BRIDGE_ENABLE_HEAP_DIAGNOSTICS
        static std::atomic<uint32_t> ignored{0};
        const uint32_t count = ++ignored;
        if (count <= 64) {
            LOGI("GuestFree ignored addr=0x%08X magic=0x%08X prev=0x%08X size=0x%08X",
                  addr, 0u, 0u, 0u);
        }
#endif
        return; // foreign pointer, or already freed
    }
    uint32_t base = MemoryRead32(addr - 12);
    uint32_t capacity = MemoryRead32(addr - 8);
    const uint32_t payload_size = MemoryRead32(addr - 4);
    MemoryWrite32(addr - 16, kFreedMagic);
#if ARM32_BRIDGE_ENABLE_HEAP_DIAGNOSTICS
    static std::atomic<uint32_t> freed{0};
    const uint32_t count = ++freed;
    if (count <= 64) LOGI("GuestFree released addr=0x%08X base=0x%08X capacity=%u", addr, base, capacity);
#endif

    m_live_bytes -= std::min<uint64_t>(m_live_bytes, payload_size);
    if (m_live_blocks) --m_live_blocks;
    ++m_total_frees;
#if ARM32_BRIDGE_ENABLE_HEAP_DIAGNOSTICS
    auto trace_it = g_heap_trace.find(payload_size);
    if (trace_it != g_heap_trace.end()) {
        trace_it->second.live_bytes -= std::min<uint64_t>(trace_it->second.live_bytes, payload_size);
        if (trace_it->second.live_blocks) --trace_it->second.live_blocks;
    }
#endif
    if (capacity <= kMaxClassSize) {
        int cls = 0;
        while ((1u << (cls + kMinClassShift)) < capacity) ++cls;
        m_free_small[cls].push_back(base);
    } else {
        m_free_huge[capacity].push_back(base);
    }
}

uint32_t DynarmicHost::GuestAllocSize(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_heap_mutex);
    if (!IsLiveHeapBlock(addr)) return 0;
    return MemoryRead32(addr - 4);
}

bool DynarmicHost::IsLiveHeapBlock(uint32_t addr) const {
    const uint64_t limit = std::min<uint64_t>(m_heap_current, std::min<uint64_t>(kHeapLimit, m_memory_size));
    if (!m_memory || addr < kHeapBase + kHeaderSize || (addr & 7u) || addr >= limit) return false;
    uint32_t header[4];
    memcpy(header, m_memory + addr - kHeaderSize, sizeof(header));
    const uint32_t base = header[1], capacity = header[2], size = header[3];
    if (header[0] != kBlockMagic || base < kHeapBase || (base & 15u) ||
        capacity < 32 || (capacity & 15u) || !size ||
        (capacity <= kMaxClassSize && (capacity & (capacity - 1)))) return false;
    return uint64_t(base) + kHeaderSize <= addr && uint64_t(base) + capacity <= limit &&
           uint64_t(addr) + size <= uint64_t(base) + capacity;
}

// Address space for the mmap thunk. Separate from the malloc heap on purpose -- see the
// declaration. No free list: a file mapping is handed to the engine for the life of the
// process, and the mmap thunk reuses an identical (file, offset, length) mapping instead of
// making a second one, which is what keeps a re-opened expansion file from costing the arena
// twice.
uint32_t DynarmicHost::GuestAllocMapping(size_t size, size_t alignment) {
    if (!m_memory || !size || !alignment || (alignment & (alignment - 1)) || alignment > UINT32_MAX) return 0;
    std::lock_guard<std::mutex> lock(m_heap_mutex);
    const uint64_t aligned = (uint64_t(m_map_current) + alignment - 1) & ~(uint64_t(alignment) - 1);
    const uint64_t limit = std::min<uint64_t>(kMapLimit, m_memory_size);
    if (aligned > limit || size > limit - aligned) {
        LOGE("GuestAllocMapping out of mapping space: at 0x%08llX, want %zu",
             static_cast<unsigned long long>(aligned), size);
        return 0;
    }
    m_map_current = static_cast<uint32_t>(aligned + size);
    return static_cast<uint32_t>(aligned);
}

// Runs once at startup. A silently wrong allocator corrupts the guest far away
// from the cause, so pay a few microseconds to fail loudly at the source.
bool DynarmicHost::HeapSelfCheck() {
#define HEAP_CHECK(cond) do { if (!(cond)) { LOGE("HeapSelfCheck FAILED: %s", #cond); ok = false; } } while (0)
    bool ok = true;

    uint32_t a = GuestAlloc(100, 8);
    uint32_t b = GuestAlloc(100, 8);
    HEAP_CHECK(a != 0 && b != 0 && a != b);
    if (!a || !b) return false;
    HEAP_CHECK((a & 7) == 0 && (b & 7) == 0);
    HEAP_CHECK(a + 100 <= b - kHeaderSize || b + 100 <= a - kHeaderSize); // no overlap
    HEAP_CHECK(GuestAllocSize(a) == 100);

    uint32_t aligned = GuestAlloc(64, 0x1000);
    HEAP_CHECK(aligned != 0 && (aligned & 0xFFF) == 0);

    // Freed blocks come back, and the payload is handed over zeroed.
    MemoryWrite32(a, 0xDEADBEEF);
    GuestFree(a);
    HEAP_CHECK(GuestAllocSize(a) == 0);        // magic cleared
    GuestFree(a);                              // double free: must be a no-op
    uint32_t reused = GuestAlloc(100, 8);
    HEAP_CHECK(reused == a);
    HEAP_CHECK(MemoryRead32(reused) == 0);

    // Pointers we did not hand out must be ignored, not recycled.
    GuestFree(0x40000000);
    GuestFree(kHeapBase + 4);
    GuestFree(b + 8);                          // interior pointer
    HEAP_CHECK(GuestAllocSize(b) == 100);      // b survived all of that
    GuestFree(UINT32_MAX);
    HEAP_CHECK(GuestAllocSize(UINT32_MAX) == 0);
    HEAP_CHECK(GuestAlloc(UINT32_MAX) == 0 && GuestAlloc(SIZE_MAX) == 0);
    HEAP_CHECK(GuestAlloc(8, 3) == 0 && GuestAlloc(8, 0) == 0);
    HEAP_CHECK(GuestAllocMapping(SIZE_MAX, 0x1000) == 0 && GuestAllocMapping(8, 3) == 0);

    // Large blocks recycle by exact capacity instead of rounding to a power of two.
    uint32_t big = GuestAlloc(8 * 1024 * 1024, 0x10000);
    HEAP_CHECK(big != 0 && (big & 0xFFFF) == 0);
    GuestFree(big);
    uint32_t big2 = GuestAlloc(8 * 1024 * 1024, 0x10000);
    HEAP_CHECK(big2 == big);
    GuestFree(big2);

    GuestFree(b);
    GuestFree(reused);
    GuestFree(aligned);
    LOGI("HeapSelfCheck %s", ok ? "OK" : "FAILED");
#undef HEAP_CHECK
    return ok;
}

void DynarmicHost::AddExecutableRange(uint32_t base, uint32_t size) {
    if (!size) return;
    if (uint64_t(base) + size > UINT32_MAX) { Fail("Executable range exceeds ARM32 space"); return; }
    std::lock_guard<std::mutex> lock(m_code_mutex);
    m_code_ranges.emplace_back(base, base + size);
    LOGI("Executable range: 0x%08X..0x%08X", base, base + size);
}

bool DynarmicHost::IsExecutableAddress(uint32_t address) const {
    if (address >= 0x00010000 && address < 0x00020000) return true;
    std::lock_guard<std::mutex> lock(m_code_mutex);
    for (const auto& range : m_code_ranges) {
        if (address >= range.first && address < range.second) return true;
    }
    return false;
}

std::optional<std::uint32_t> DynarmicHost::MemoryReadCode(std::uint32_t vaddr) {
    if (vaddr == 0) return 0xE7F0DEF0u;
    // Runaway PC containment. The arena is zero-filled and zeros decode as a valid
    // `andeq r0,r0,r0`, so a jump through a garbage pointer makes the JIT translate
    // a fresh block every 0x8000 bytes forever: RSS runs away and the LMK kills the
    // process, a symptom that reads exactly like a memory leak. Feed the JIT a
    // permanently-undefined instruction instead -- ExceptionRaised halts it and
    // prints where the jump came from.
    if (!m_modules.empty() && vaddr >= 0x00010000 && !IsExecutableAddress(vaddr)) {
        LOGE("[RUNAWAY] code fetch 0x%08X", vaddr);
        return 0xE7FEDEFFu;
    }
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) {
        return *reinterpret_cast<uint32_t*>(m_memory + vaddr);
    }
    return std::nullopt;
}

std::uint8_t DynarmicHost::MemoryRead8(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) < m_memory_size) return m_memory[vaddr];
    return 0;
}

std::uint16_t DynarmicHost::MemoryRead16(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 2 <= m_memory_size) return *reinterpret_cast<uint16_t*>(m_memory + vaddr);
    return 0;
}

std::uint32_t DynarmicHost::MemoryRead32(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) return *reinterpret_cast<uint32_t*>(m_memory + vaddr);
    return 0;
}

std::uint64_t DynarmicHost::MemoryRead64(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 8 <= m_memory_size) return *reinterpret_cast<uint64_t*>(m_memory + vaddr);
    return 0;
}

void DynarmicHost::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (static_cast<uint64_t>(vaddr) < m_memory_size) m_memory[vaddr] = value;
}

void DynarmicHost::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (static_cast<uint64_t>(vaddr) + 2 <= m_memory_size) *reinterpret_cast<uint16_t*>(m_memory + vaddr) = value;
}

void DynarmicHost::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) *reinterpret_cast<uint32_t*>(m_memory + vaddr) = value;
}

void DynarmicHost::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (static_cast<uint64_t>(vaddr) + 8 <= m_memory_size) *reinterpret_cast<uint64_t*>(m_memory + vaddr) = value;
}

bool DynarmicHost::ConfigureFmodProbe() {
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    auto* engine = FindModule("libGameEngine.so");
    auto* fmod = FindModule("libfmodex.so");
    if (!engine || !fmod) return false;
    const uint64_t engine_base = engine->GetBaseAddress();
    const uint64_t fmod_base = fmod->GetBaseAddress();
    if (engine_base + 0xC70D54 > GetGuestMemorySize() ||
        fmod_base + 0xA1A20 > GetGuestMemorySize() ||
        engine_base + 0xC70D54 > UINT32_MAX || fmod_base + 0xA1A20 > UINT32_MAX) return false;
    const auto* initialize = engine->GetExportedSymbol("_ZN11SoundSystem10InitializeEv");
    const auto* open = engine->GetExportedSymbol("_ZN9SoundData4OpenEv");
    const auto* create_sound = fmod->GetExportedSymbol("_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE");
    const auto* init = fmod->GetExportedSymbol("_ZN4FMOD6System4initEijPv");
    // Defined exports already contain the module base and retain their ARM/Thumb bit.
    if (!initialize || initialize->type != STT_FUNC || initialize->value != engine_base + 0x6C5B7C || initialize->size != 1460 ||
        !open || open->type != STT_FUNC || open->value != engine_base + 0x6C3158 || open->size != 276 ||
        !create_sound || create_sound->type != STT_FUNC || create_sound->value != fmod_base + 0xA1430 || create_sound->size != 72 ||
        !init || init->type != STT_FUNC || init->value != fmod_base + 0xA19E0 || init->size != 64) return false;
    if (MemoryRead32(static_cast<uint32_t>(engine_base + 0xC70D18)) != create_sound->value ||
        MemoryRead32(static_cast<uint32_t>(engine_base + 0xC70D50)) != init->value) return false;
    const FmodProbeSites sites{
        {initialize->value + 0x208, 0xEBF4BAA0, 4, false},
        {initialize->value + 0x20C, 0xE59F3358, 4, false},
        {open->value + 0xCC, 0xEBF4C54E, 4, false},
        {open->value + 0xD0, 0xE1A00004, 4, false},
    };
    return EnableFmodProbe(sites);
#else
    return false;
#endif
}

bool DynarmicHost::EnableFmodProbe(const FmodProbeSites& sites) {
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    std::lock_guard<std::mutex> lock(m_fmod_probe_mutex);
    if (!m_memory || HasFailed() || HasExited() ||
        m_guest_translation_started.load(std::memory_order_acquire) ||
        m_fmod_probe_enabled.load(std::memory_order_acquire)) return false;
    const FmodProbeSite all[]{sites.init_before, sites.init_after,
                              sites.sound_before, sites.sound_after};
    for (unsigned i = 0; i < 4; ++i) {
        const auto& site = all[i];
        const uint64_t end = uint64_t(site.address) + site.instruction_bytes;
        if (site.address < 0x10000u || (site.address & (site.thumb ? 1u : 3u)) ||
            (site.instruction_bytes != 4 && (!site.thumb || site.instruction_bytes != 2)) ||
            end > m_memory_size || end > GUEST_ADDRESS_SPACE ||
            !IsExecutableAddress(site.address) ||
            !IsExecutableAddress(static_cast<uint32_t>(end - 1))) return false;
        for (unsigned j = 0; j < i; ++j) {
            const uint64_t previous_end = uint64_t(all[j].address) + all[j].instruction_bytes;
            if (uint64_t(site.address) < previous_end && uint64_t(all[j].address) < end) return false;
        }
        uint32_t opcode = 0;
        std::memcpy(&opcode, m_memory + site.address, site.instruction_bytes);
        if (opcode != site.opcode) return false;
        if (!site.thumb) {
            if ((opcode >> 28) != 0xEu) return false; // ARM AL only.
        } else {
            const uint16_t first = static_cast<uint16_t>(opcode);
            if (site.instruction_bytes == 2) {
                // No conditional B/SVC/UDF, CBZ/CBNZ, IT or wide prefix.
                if ((first & 0xF000u) == 0xD000u ||
                    (first & 0xF500u) == 0xB100u ||
                    ((first & 0xFF00u) == 0xBF00u && (first & 0xFu)) ||
                    (first & 0xF800u) >= 0xE800u) return false;
            } else {
                // The diagnostic test supports only unconditional Thumb BL32.
                const uint16_t second = static_cast<uint16_t>(opcode >> 16);
                if ((first & 0xF800u) != 0xF000u || (second & 0xD000u) != 0xD000u) return false;
            }
            // Conservative: an IT in the maximum preceding four-instruction
            // window refuses setup, even if control flow might bypass it.
            for (unsigned delta = 2; delta <= 18; delta += 2) {
                if (site.address < delta) return false;
                uint16_t previous = 0;
                std::memcpy(&previous, m_memory + site.address - delta, 2);
                if ((previous & 0xFF00u) == 0xBF00u && (previous & 0xFu)) return false;
            }
        }
    }
    m_fmod_probe_sites = sites;
    m_fmod_probe_summary.enabled = true;
    m_fmod_probe_enabled.store(true, std::memory_order_release);
    return true;
#else
    (void)sites;
    return false;
#endif
}

DynarmicHost::FmodProbeSummary DynarmicHost::GetFmodProbeSummary() const {
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    std::lock_guard<std::mutex> lock(m_fmod_probe_mutex);
    FmodProbeSummary copy = m_fmod_probe_summary;
    copy.translation_started = m_guest_translation_started.load(std::memory_order_acquire);
    return copy;
#else
    return {};
#endif
}

void DynarmicHost::PreCodeTranslationHook(bool is_thumb, std::uint32_t pc,
                                         Dynarmic::A32::IREmitter& ir) {
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    if (!m_guest_translation_started.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(m_fmod_probe_mutex);
        m_guest_translation_started.store(true, std::memory_order_release);
    }
    if (!m_fmod_probe_enabled.load(std::memory_order_acquire)) return;
    const FmodProbeSite all[]{m_fmod_probe_sites.init_before, m_fmod_probe_sites.init_after,
                              m_fmod_probe_sites.sound_before, m_fmod_probe_sites.sound_after};
    for (unsigned i = 0; i < 4; ++i) {
        if (all[i].address != pc) continue;
        uint32_t opcode = 0;
        std::memcpy(&opcode, m_memory + pc, all[i].instruction_bytes);
        if (all[i].thumb != is_thumb || opcode != all[i].opcode ||
            (is_thumb && ir.current_location.IT().IsInITBlock())) {
            std::lock_guard<std::mutex> lock(m_fmod_probe_mutex);
            ++m_fmod_probe_summary.rejected_translations;
            return;
        }
        // Site identity is carried only by the IR tag. Do not materialize PC or
        // alter any guest register/control state to satisfy this observer.
        ir.CallSupervisor(ir.Imm32(kFmodProbeTagBase + i));
        return;
    }
#else
    (void)is_thumb; (void)pc; (void)ir;
#endif
}

#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
DynarmicHost::FmodProbeBoundary DynarmicHost::CaptureFmodBoundary(
    unsigned index, const FmodProbeBoundary* before) const {
    const JitContext& context = *s_active_context;
    const auto& jit = *context.jit;
    FmodProbeBoundary result{};
    const FmodProbeSite all[]{m_fmod_probe_sites.init_before, m_fmod_probe_sites.init_after,
                              m_fmod_probe_sites.sound_before, m_fmod_probe_sites.sound_after};
    result.site = all[index].address;
    result.thread_generation = context.thread_generation;
    result.regs = jit.Regs();
    result.ext_regs = jit.ExtRegs();
    result.cpsr = jit.Cpsr();
    result.fpscr = jit.Fpscr();
    const auto& args = before ? before->regs : result.regs;
    auto read = [&](uint64_t address, void* destination, size_t bytes, uint32_t bit) {
        if (!address || address + bytes > GUEST_ADDRESS_SPACE || address + bytes > m_memory_size)
            return false;
        std::memcpy(destination, m_memory + static_cast<size_t>(address), bytes);
        result.read_mask |= bit;
        return true;
    };
    uint8_t live = 0;
    if (args[0]) {
        read(uint64_t(args[0]) + 0x11, &live, 1, ProbeSystemLive);
        result.system_live = live;
        read(uint64_t(args[0]) + 0x550, &result.system_mixer, 4, ProbeSystemMixer);
    }
    if (index < 2) return result;
    result.sound_data = args[4];
    if (before) {
        result.out_address = before->out_address;
        result.read_mask |= before->read_mask & ProbeOutAddress;
    } else {
        // A fifth AAPCS argument must belong to this CPU's current stack.
        const uint64_t sp = result.regs[13];
        const uint64_t low = uint64_t(context.stack_top) - 0x00200000u;
        if (sp >= low && sp + 4 <= context.stack_top)
            read(sp, &result.out_address, 4, ProbeOutAddress);
    }
    if (result.read_mask & ProbeOutAddress)
        read(result.out_address, &result.out_value, 4, ProbeOutValue);
    if (args[3]) {
        for (unsigned i = 0; i < result.exinfo.size(); ++i)
            read(uint64_t(args[3]) + i * 4, &result.exinfo[i], 4, ProbeExInfo0 << i);
    }
    if (result.sound_data) {
        read(uint64_t(result.sound_data) + 0x38, &result.sound38, 4, ProbeSound38);
        read(uint64_t(result.sound_data) + 0x3c, &result.sound3c, 4, ProbeSound3c);
    }
    return result;
}

void DynarmicHost::ObserveFmodBoundary(unsigned index) {
    JitContext* context = s_active_context;
    if (!context || context->owner != this || !context->running || !context->jit ||
        !m_fmod_probe_enabled.load(std::memory_order_acquire)) return;
    const unsigned family = index / 2;
    std::lock_guard<std::mutex> lock(m_fmod_probe_mutex);
    ++m_fmod_probe_summary.event_counts[index];
    if ((index & 1u) == 0) {
        if (context->fmod_pending_valid[family]) ++m_fmod_probe_summary.rejected_pairs;
        context->fmod_pending[family] = CaptureFmodBoundary(index, nullptr);
        context->fmod_pending_valid[family] = true;
        return;
    }
    const FmodProbeBoundary& before = context->fmod_pending[family];
    const auto& regs = context->jit->Regs();
    if (!context->fmod_pending_valid[family] || before.thread_generation != context->thread_generation ||
        before.regs[13] != regs[13] || bool(before.cpsr & 0x20u) != bool(context->jit->Cpsr() & 0x20u)) {
        context->fmod_pending_valid[family] = false;
        ++m_fmod_probe_summary.rejected_pairs;
        return;
    }
    context->fmod_pending_valid[family] = false;
    FmodProbePair pair{};
    pair.captured = true;
    pair.before = before;
    pair.after = CaptureFmodBoundary(index, &before);
    if (pair.after.regs[0] != 0) pair.suspect_mask |= ProbeNonzeroStatus;
    if (family == 0) {
        ++m_fmod_probe_summary.init_pairs;
        if (!m_fmod_probe_summary.init.captured) {
            m_fmod_probe_summary.init = pair;
            LogFmodPair("init_first", pair);
        }
        return;
    }
    ++m_fmod_probe_summary.sound_pairs;
    const uint32_t output_bits = ProbeOutAddress | ProbeOutValue;
    if ((pair.after.read_mask & output_bits) != output_bits) pair.suspect_mask |= ProbeUnreadableOutput;
    else if (!pair.after.out_value) pair.suspect_mask |= ProbeNullOutput;
    else if (IsExecutableAddress(pair.after.out_value & ~1u)) pair.suspect_mask |= ProbeExecutableOutput;
    if (pair.after.regs[4] != pair.before.regs[4]) pair.suspect_mask |= ProbeChangedThis;
    if (((pair.after.read_mask & ProbeSound38) && pair.after.sound38 &&
         IsExecutableAddress(pair.after.sound38 & ~1u)) ||
        ((pair.after.read_mask & ProbeSound3c) && pair.after.sound3c &&
         IsExecutableAddress(pair.after.sound3c & ~1u))) pair.suspect_mask |= ProbeExecutableSoundField;
    m_fmod_probe_summary.last_sound = pair;
    if (pair.suspect_mask && !m_fmod_probe_summary.suspect_sound.captured) {
        m_fmod_probe_summary.suspect_sound = pair;
        m_fmod_sound_logged = true;
        LogFmodPair("createSound_first_suspect", pair);
    }
}

void DynarmicHost::LogFmodPair(const char* label, const FmodProbePair& pair) const {
    const auto& b = pair.before;
    const auto& a = pair.after;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "[FMOD_PROBE] %s thread=%llu sites=%08X/%08X suspect=%X status=%d/0x%08X SP=%08X/%08X LR=%08X/%08X CPSR=%08X/%08X FPSCR=%08X/%08X",
        label, static_cast<unsigned long long>(b.thread_generation), b.site, a.site, pair.suspect_mask,
        static_cast<int32_t>(a.regs[0]), a.regs[0], b.regs[13], a.regs[13], b.regs[14], a.regs[14],
        b.cpsr, a.cpsr, b.fpscr, a.fpscr);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "[FMOD_PROBE] args=%08X,%08X,%08X,%08X this=%08X/%08X readMask=%X/%X System+11=%X/%X System+550=%08X/%08X",
        b.regs[0], b.regs[1], b.regs[2], b.regs[3], b.regs[4], a.regs[4],
        b.read_mask, a.read_mask, b.system_live, a.system_live, b.system_mixer, a.system_mixer);
    if (label[0] == 'i') return;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "[FMOD_PROBE] outAddress=%08X/%08X outValue=%08X/%08X exinfo=%08X,%08X,%08X,%08X SoundData=%08X fields38/3c=%08X,%08X/%08X,%08X",
        b.out_address, a.out_address, b.out_value, a.out_value,
        b.exinfo[0], b.exinfo[1], b.exinfo[2], b.exinfo[3], b.sound_data,
        b.sound38, b.sound3c, a.sound38, a.sound3c);
}

void DynarmicHost::LogLastFmodPairOnFailure() {
    std::lock_guard<std::mutex> lock(m_fmod_probe_mutex);
    if (!m_fmod_probe_summary.enabled || m_fmod_sound_logged) return;
    m_fmod_sound_logged = true;
    if (m_fmod_probe_summary.last_sound.captured)
        LogFmodPair("createSound_last_completed_no_suspect", m_fmod_probe_summary.last_sound);
    else
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
            "[FMOD_PROBE] no_completed_createSound events=%llu,%llu,%llu,%llu rejectedPairs=%llu rejectedTranslations=%llu",
            static_cast<unsigned long long>(m_fmod_probe_summary.event_counts[0]),
            static_cast<unsigned long long>(m_fmod_probe_summary.event_counts[1]),
            static_cast<unsigned long long>(m_fmod_probe_summary.event_counts[2]),
            static_cast<unsigned long long>(m_fmod_probe_summary.event_counts[3]),
            static_cast<unsigned long long>(m_fmod_probe_summary.rejected_pairs),
            static_cast<unsigned long long>(m_fmod_probe_summary.rejected_translations));
}
#endif

void DynarmicHost::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    LOGE("Interpreter Fallback at 0x%08X", pc);
    Fail("Guest interpreter fallback");
}

void DynarmicHost::CallSVC(std::uint32_t swi) {
    if (!s_active_context) return;
    Dynarmic::A32::Jit& jit = *s_active_context->jit;
    if (HasFailed() || HasExited()) { jit.HaltExecution(Dynarmic::HaltReason::UserDefined1); return; }
#if defined(WOLF_FMOD_DIAGNOSTIC) && WOLF_FMOD_DIAGNOSTIC
    if (swi >= kFmodProbeTagBase && swi < kFmodProbeTagBase + 4) {
        ObserveFmodBoundary(swi - kFmodProbeTagBase);
        return;
    }
#endif
    if (swi == 0) {
        const bool is_thumb = (jit.Cpsr() & 0x20u) != 0;
        const uint32_t return_pc = s_active_context->call_thumb ? EXIT_TRAP_THUMB + 2 : EXIT_TRAP_ARM + 4;
        if (s_active_context->owner != this || !s_active_context->running ||
            is_thumb != s_active_context->call_thumb || jit.Regs()[15] != return_pc ||
            jit.Regs()[13] != s_active_context->call_sp) {
            LogFailureSnapshot("invalid SVC0 return sentinel/SP/ISA", jit.Regs()[15]);
            Fail("Invalid guest return sentinel/SP/ISA");
            return;
        }
        s_active_context->return_seen = true;
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    if (jit.Regs()[15] <= 0x40) {
        LogFailureSnapshot("invalid low-page SVC", jit.Regs()[15]);
        Fail("Invalid guest low-page SVC");
        return;
    }
#if ARM32_BRIDGE_ENABLE_RUNTIME_WATCHDOG
    const uint64_t boundary = (static_cast<uint64_t>(swi) << 32) | jit.Regs()[14];
    s_active_context->last_boundary.store(boundary | (1ull << 63), std::memory_order_relaxed);
#endif
    m_thunk_manager->DispatchThunk(swi, jit);
    if (HasFailed() || HasExited()) jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
#if ARM32_BRIDGE_ENABLE_RUNTIME_WATCHDOG
    s_active_context->last_boundary.store(boundary, std::memory_order_relaxed);
#endif
}

void DynarmicHost::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    LogFailureSnapshot("guest JIT exception", pc);
    LOGE("Exception at PC: 0x%08X (type %d)", pc, static_cast<int>(exception));
    Fail("Guest JIT exception");
}

void DynarmicHost::AddTicks(std::uint64_t ticks) {
    if (m_ticks_remaining > ticks) m_ticks_remaining -= ticks;
    else m_ticks_remaining = 0;
}

std::uint64_t DynarmicHost::GetTicksRemaining() {
    return m_ticks_remaining;
}

// STREX: compare-and-set against guest memory. The ExclusiveMonitor already holds its lock
// around this and has verified the reservation, so a plain compare/store is sufficient.
template<typename T>
static bool GuestCompareAndSwap(uint8_t* mem, size_t mem_size, uint32_t vaddr, T value, T expected) {
    if (static_cast<uint64_t>(vaddr) + sizeof(T) > mem_size) return false;
    T* p = reinterpret_cast<T*>(mem + vaddr);
    if (*p != expected) return false;
    *p = value;
    return true;
}

bool DynarmicHost::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}
