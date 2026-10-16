#pragma once

#include <string>

// 逆向用除錯功能：類似 Cheat Engine 的「Find out what writes/accesses this address」。
// 監看 DLL 同目錄的 Nioh2DropFilter.watch.txt，內容格式：
//   w  <hex 位址> [大小 1|2|4|8]    監看寫入
//   rw <hex 位址> [大小 1|2|4|8]    監看讀寫
//   off                              關閉
// 命中的指令（含暫存器與前後位元組）會寫進 log。只在 ini [Debug] EnableWatch=1 時啟用。
// 只存在於 Debug 版（_DEBUG），Release 版不編譯也不呼叫。
#ifdef _DEBUG
namespace DebugWatch
{
    void Start(const std::wstring& dllDir);
    void Stop();
}
#endif
