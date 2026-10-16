#include <Windows.h>

#include "mod.h"

// 規則：DllMain 只負責開執行緒，不做任何初始化。
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE thread = CreateThread(nullptr, 0, Mod::InitThread, hModule, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        break;
    }
    case DLL_PROCESS_DETACH:
        // reserved != nullptr 表示行程正在結束，此時不做任何清理（其他執行緒已被終止）。
        if (reserved == nullptr)
            Mod::Shutdown();
        break;
    }
    return TRUE;
}
