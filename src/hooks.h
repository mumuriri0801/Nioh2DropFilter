#pragma once

#include <cstdint>

namespace Hooks
{
    bool Init();
    void Shutdown();

    // 建立並啟用 hook。target 為 0 時直接回傳 false（特徵碼掃描失敗的情況）。
    bool Create(uintptr_t target, void* detour, void** original, const char* name);

    template <typename T>
    bool Create(uintptr_t target, T* detour, T** original, const char* name)
    {
        return Create(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
    }
}
