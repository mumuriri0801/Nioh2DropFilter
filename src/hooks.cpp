#include "hooks.h"

#include <MinHook.h>

#include "log.h"

namespace
{
    bool g_initialized = false;
}

namespace Hooks
{
    bool Init()
    {
        if (g_initialized)
            return true;
        const MH_STATUS status = MH_Initialize();
        if (status != MH_OK)
        {
            Log::Error("MH_Initialize 失敗：%s", MH_StatusToString(status));
            return false;
        }
        g_initialized = true;
        return true;
    }

    void Shutdown()
    {
        if (!g_initialized)
            return;
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_initialized = false;
    }

    bool Create(uintptr_t target, void* detour, void** original, const char* name)
    {
        if (!g_initialized || target == 0)
        {
            Log::Warn("略過 hook [%s]：MinHook 未初始化或目標位址為 0", name);
            return false;
        }

        auto* targetPtr = reinterpret_cast<void*>(target);
        MH_STATUS status = MH_CreateHook(targetPtr, detour, original);
        if (status != MH_OK)
        {
            Log::Error("MH_CreateHook [%s] 失敗：%s", name, MH_StatusToString(status));
            return false;
        }
        status = MH_EnableHook(targetPtr);
        if (status != MH_OK)
        {
            Log::Error("MH_EnableHook [%s] 失敗：%s", name, MH_StatusToString(status));
            MH_RemoveHook(targetPtr);
            return false;
        }
        Log::Info("hook [%s] 已啟用", name);
        return true;
    }
}
