#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Scanner
{
    struct Module
    {
        uintptr_t base = 0;
        size_t size = 0;
    };

    // 取得已載入模組的範圍（例如 L"nioh2.exe"）。
    bool GetModule(const wchar_t* name, Module& out);

    // 將 "48 8B 05 ?? ?? ?? ??" 轉成位元組陣列，-1 代表萬用字元。格式錯誤回傳空陣列。
    std::vector<int> ParsePattern(const std::string& pattern);

    // 只掃描模組中可執行的區段。回傳所有符合的位址。
    std::vector<uintptr_t> FindAll(const Module& module, const std::string& pattern);

    // 只接受「唯一」的結果；找不到或有多個符合都回傳 0 並寫 log。
    uintptr_t FindUnique(const Module& module, const std::string& pattern, const char* name);

    // 解析 RIP 相對位址：instruction + length + *(int32*)(instruction + dispOffset)
    // 例：48 8B 05 xx xx xx xx → dispOffset=3, length=7
    //     E8 xx xx xx xx       → dispOffset=1, length=5
    uintptr_t ResolveRip(uintptr_t instruction, size_t dispOffset, size_t length);
}

namespace Mem
{
    // 以 SEH 保護的記憶體讀取，位址無效時回傳 false 而不是當掉。
    bool SafeRead(const void* address, void* out, size_t size);

    template <typename T>
    bool Read(uintptr_t address, T& out)
    {
        if (address == 0)
            return false;
        return SafeRead(reinterpret_cast<const void*>(address), &out, sizeof(T));
    }

    // 沿指標鏈讀取：[[base + o0] + o1] + ... ，任何一層為 null 都回傳 0。
    uintptr_t ReadChain(uintptr_t base, const std::vector<ptrdiff_t>& offsets);
}
