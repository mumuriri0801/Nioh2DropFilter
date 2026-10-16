#pragma once

#include <Windows.h>
#include <cstdint>

namespace Mod
{
    // 由 DllMain 以 CreateThread 啟動，lpParam 為本 DLL 的 HMODULE。
    DWORD WINAPI InitThread(LPVOID param);

    // DLL 被卸載時呼叫（非行程結束）。
    void Shutdown();

    // 目前擋掉落是否啟用（ini 的 Enabled 與熱鍵切換）。
    bool IsEnabled();

    // 查道具類型（道具參數 +0x4）。查不到或尚未初始化回傳 -1。
    int GetItemType(uint16_t itemId);
}
