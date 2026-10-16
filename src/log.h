#pragma once

#include <string>

namespace Log
{
    // 在 dllDir 建立 Nioh2DropFilter.log（舊檔改名為 .previous.log）。Debug 版另開 console。
    void Init(const std::wstring& dllDir);
    void Shutdown();

    // printf 風格，字串為 UTF-8。
    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);
}
