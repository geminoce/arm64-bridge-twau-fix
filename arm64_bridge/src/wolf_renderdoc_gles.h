// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <utility>

namespace wolf_renderdoc_gles {

void Initialize();
bool IsReady() noexcept;
void* ResolveNext(const char* name) noexcept;
void* ResolveLayer(const char* name, void* next) noexcept;

template <auto Function, typename... Args>
decltype(auto) Invoke(const char* name, Args&&... args) {
    using FunctionPointer = decltype(Function);
    static const FunctionPointer resolved = [name] {
        if (!IsReady()) return Function;
        void* next = ResolveNext(name);
        if (!next) return Function;
        void* hooked = ResolveLayer(name, next);
        return hooked ? reinterpret_cast<FunctionPointer>(hooked) : Function;
    }();
    return resolved(std::forward<Args>(args)...);
}

} // namespace wolf_renderdoc_gles
