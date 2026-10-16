#include "mod.h"

#include <intrin.h>
#include <atomic>
#include <cstring>
#include <string>

#include "config.h"
#include "debugwatch.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "scanner.h"

#ifndef NIOH2DROPFILTER_VERSION
#define NIOH2DROPFILTER_VERSION "0.5.0"
#endif

namespace
{
    // 等遊戲完成啟動再開始掃描；mod loader 本身約在啟動後 5 秒才載入 mod。
    constexpr DWORD kStartupDelayMs = 5000;
    constexpr DWORD kModuleWaitTimeoutMs = 60000;
    constexpr DWORD kHotkeyPollMs = 50;
    // 實測一次按鍵會在約 200ms 內觸發兩次（原因未確認），切換後一段時間內忽略重複觸發。
    constexpr ULONGLONG kHotkeyCooldownMs = 400;

    constexpr DWORD kIniPollMs = 1000;

    Config g_config;
    std::wstring g_iniPath;
    std::atomic<bool> g_enabled = false;
    std::atomic<bool> g_featureReady = false; // 所有特徵碼與 hook 都成功才為 true
    std::atomic<bool> g_stop = false;

    // 擋掉落規則。ini 重新載入時整份換新（遊戲執行緒只讀取指標），舊的刻意不釋放以免競爭。
    struct DropRules
    {
        std::vector<uint16_t> keepIds;
        std::vector<uint16_t> blockIds;
        std::vector<uint16_t> keepTypes;
        std::vector<uint16_t> blockTypes;
        bool logDrops = true;
    };
    std::atomic<const DropRules*> g_rules = nullptr;

    void PublishRules(const Config& config)
    {
        auto* rules = new DropRules{config.keepItemIds, config.blockItemIds, config.keepItemTypes, config.blockItemTypes, config.logDrops};
        g_rules.store(rules);
    }

    std::wstring GetDllDirectory(HMODULE module)
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring dir = path;
        const size_t slash = dir.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
    }

    bool IsGameWindowFocused()
    {
        HWND foreground = GetForegroundWindow();
        if (!foreground)
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        return pid == GetCurrentProcessId();
    }

    Game::RollLootFn g_origRollLoot = nullptr;
    uintptr_t g_enemyLootReturn = 0; // 只過濾從「敵人掉落」那一處呼叫的擲骰
    Game::GetItemParamFn g_getItemParam = nullptr;
    uintptr_t g_paramRootGlobal = 0; // 全域變數位址，內容為根指標

    bool Contains(const uint16_t* list, size_t count, uint16_t value)
    {
        for (size_t i = 0; i < count; ++i)
            if (list[i] == value)
                return true;
        return false;
    }

    struct KeepRules
    {
        const uint16_t* ids;
        size_t idCount;
        const uint16_t* blockIds; // 優先於類型保留
        size_t blockIdCount;
        const uint16_t* types;
        size_t typeCount;
        const uint16_t* blockTypes; // 非空時只擋這些類型
        size_t blockTypeCount;
    };

    bool ShouldKeep(uint16_t id, int type, const KeepRules& rules)
    {
        if (Contains(rules.ids, rules.idCount, id))
            return true;
        if (Contains(rules.blockIds, rules.blockIdCount, id))
            return false;
        if (type >= 0 && Contains(rules.types, rules.typeCount, static_cast<uint16_t>(type)))
            return true;
        if (rules.blockTypeCount == 0)
            return false; // 未設定黑名單：全部擋掉
        // 有黑名單：只擋列出的類型；查不到類型的也保留（寧可放過，不可誤擋關鍵道具）
        return type < 0 || !Contains(rules.blockTypes, rules.blockTypeCount, static_cast<uint16_t>(type));
    }

    // 從擲骰結果移除不保留的道具（原地壓縮並調整 end）。
    // 以 SEH 保護：結構不符時回傳 false，結果保持原樣。此函式內不可有需要解構的物件。
    // apply = false（熱鍵停用中）時只記錄、不移除，方便對照畫面上掉出來的東西。
    bool FilterLoot(Game::ItemVector* out, const KeepRules& rules, bool log, bool apply)
    {
        __try
        {
            Game::ItemData* begin = out->begin;
            Game::ItemData* end = out->end;
            if (begin == end)
                return true; // 沒擲出任何道具（空 vector 的 begin/end 皆為 null）
            if (!begin || end < begin || end - begin > 256)
                return false;

            Game::ItemData* write = begin;
            for (Game::ItemData* it = begin; it != end; ++it)
            {
                const int type = Mod::GetItemType(it->id);
                const bool kept = ShouldKeep(it->id, type, rules);
                if (log)
                    Log::Info("[DROP] 敵人掉落 id=0x%04X type=0x%02X count=%u → %s%s", it->id, type & 0xFF, it->count,
                              apply ? "" : "（停用中，照常掉落）啟用時會", kept ? "保留" : "移除");
                if (kept || !apply)
                {
                    if (write != it)
                        std::memmove(write, it, sizeof(Game::ItemData));
                    ++write;
                }
            }
            out->end = write;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* __fastcall RollLootDetour(void* ctx, Game::ItemVector* out, void* enemy, float param)
    {
        void* result = g_origRollLoot(ctx, out, enemy, param);
        const DropRules* current = g_rules.load();
        const bool apply = Mod::IsEnabled();
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_enemyLootReturn && out && current && g_featureReady &&
            (apply || current->logDrops))
        {
            const KeepRules rules{current->keepIds.data(),    current->keepIds.size(),
                                  current->blockIds.data(),   current->blockIds.size(),
                                  current->keepTypes.data(),  current->keepTypes.size(),
                                  current->blockTypes.data(), current->blockTypes.size()};
            if (!FilterLoot(out, rules, current->logDrops, apply))
                Log::Warn("[DROP] 擲骰結果結構不符，本次不處理");
        }
        return result;
    }

    bool InModule(const Scanner::Module& game, uintptr_t address)
    {
        return address >= game.base && address < game.base + game.size;
    }

    // 道具類型查詢：沒有它就無法辨識關鍵道具，失敗時整個功能停用（避免誤擋關鍵道具）。
    bool SetupItemParamLookup(const Scanner::Module& game)
    {
        const uintptr_t site = Scanner::FindUnique(game, Game::kItemParamLookupPattern, "ItemParamLookup");
        if (!site)
            return false;
        const uintptr_t root = Scanner::ResolveRip(site, 3, 7);
        const uintptr_t call = site + Game::kItemParamCallOffset;
        uint8_t opcode = 0;
        if (!Mem::Read(call, opcode) || opcode != 0xE8)
        {
            Log::Error("ItemParamLookup +0x%zX 不是 call 指令（0x%02X）", Game::kItemParamCallOffset, opcode);
            return false;
        }
        const uintptr_t fn = Scanner::ResolveRip(call, 1, 5);
        if (!InModule(game, root) || !InModule(game, fn))
        {
            Log::Error("ItemParamLookup 解析出的位址不在 nioh2.exe 範圍內");
            return false;
        }
        g_paramRootGlobal = root;
        g_getItemParam = reinterpret_cast<Game::GetItemParamFn>(fn);
        Log::Info("GetItemParam = nioh2.exe+0x%llX，參數根指標 = nioh2.exe+0x%llX",
                  static_cast<unsigned long long>(fn - game.base), static_cast<unsigned long long>(root - game.base));
        return true;
    }

    // 掃描所有需要的特徵碼並建立 hook。任何一項失敗都回傳 false，功能保持停用。
    bool SetupGameHooks(const Scanner::Module& game)
    {
        if (!SetupItemParamLookup(game))
            return false;

        const uintptr_t site = Scanner::FindUnique(game, Game::kEnemyLootRollCallPattern, "EnemyLootRollCall");
        if (!site)
            return false;

        const uintptr_t call = site + Game::kEnemyLootRollCallOffset;
        uint8_t opcode = 0;
        if (!Mem::Read(call, opcode) || opcode != 0xE8)
        {
            Log::Error("EnemyLootRollCall +0x%zX 不是 call 指令（0x%02X）", Game::kEnemyLootRollCallOffset, opcode);
            return false;
        }
        const uintptr_t target = Scanner::ResolveRip(call, 1, 5);
        if (!InModule(game, target))
        {
            Log::Error("RollLoot 位址 0x%llX 不在 nioh2.exe 範圍內", static_cast<unsigned long long>(target));
            return false;
        }
        Log::Info("RollLoot = nioh2.exe+0x%llX", static_cast<unsigned long long>(target - game.base));

        g_enemyLootReturn = site + Game::kEnemyLootRollReturnOffset;
        return Hooks::Create(target, &RollLootDetour, &g_origRollLoot, "RollLoot");
    }

    bool GetWriteTime(const std::wstring& path, FILETIME& out)
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
            return false;
        out = attr.ftLastWriteTime;
        return true;
    }

    // ini 存檔後自動重新載入（不必重開遊戲）。
    void ReloadIniIfChanged(FILETIME& lastWrite)
    {
        FILETIME now{};
        if (!GetWriteTime(g_iniPath, now) || CompareFileTime(&now, &lastWrite) == 0)
            return;
        lastWrite = now;
        Sleep(200); // 等編輯器寫完

        Config fresh;
        if (!fresh.Load(g_iniPath))
            return;
        Log::Info("偵測到 ini 變更，重新載入");
        fresh.Dump();
        if (fresh.enabled != g_config.enabled)
            g_enabled = fresh.enabled;
        g_config.enabled = fresh.enabled;
        g_config.toggleKey = fresh.toggleKey;
        g_config.logDrops = fresh.logDrops;
        g_config.keepItemIds = fresh.keepItemIds;
        g_config.blockItemIds = fresh.blockItemIds;
        g_config.keepItemTypes = fresh.keepItemTypes;
        g_config.blockItemTypes = fresh.blockItemTypes;
        PublishRules(g_config);
    }

    void HotkeyLoop()
    {
        bool wasDown = false;
        ULONGLONG lastToggle = 0;
        FILETIME iniWrite{};
        GetWriteTime(g_iniPath, iniWrite);
        DWORD sinceIniPoll = 0;
        while (!g_stop)
        {
            sinceIniPoll += kHotkeyPollMs;
            if (sinceIniPoll >= kIniPollMs)
            {
                sinceIniPoll = 0;
                ReloadIniIfChanged(iniWrite);
            }

            const bool down = (GetAsyncKeyState(g_config.toggleKey) & 0x8000) != 0;
            const ULONGLONG now = GetTickCount64();
            if (down && !wasDown && now - lastToggle >= kHotkeyCooldownMs && IsGameWindowFocused())
            {
                lastToggle = now;
                const bool enabled = !g_enabled;
                g_enabled = enabled;
                Log::Info("熱鍵切換：擋掉敵人掉落 %s", enabled ? "開啟" : "關閉");
            }
            wasDown = down;
            Sleep(kHotkeyPollMs);
        }
    }
}

namespace Mod
{
    DWORD WINAPI InitThread(LPVOID param)
    {
        auto module = static_cast<HMODULE>(param);
        Sleep(kStartupDelayMs);

        const std::wstring dllDir = GetDllDirectory(module);
        Log::Init(dllDir);
        Log::Info("Nioh2DropFilter v%s 已載入", NIOH2DROPFILTER_VERSION);

        g_iniPath = dllDir + L"\\Nioh2DropFilter.ini";
        if (!g_config.Load(g_iniPath))
            Log::Warn("找不到 Nioh2DropFilter.ini，使用預設值");
        g_config.Dump();
        g_enabled = g_config.enabled;
        PublishRules(g_config);

        Scanner::Module game;
        DWORD waited = 0;
        while (!Scanner::GetModule(L"nioh2.exe", game))
        {
            if (g_stop || waited >= kModuleWaitTimeoutMs)
            {
                Log::Error("找不到 nioh2.exe 模組，停用");
                return 0;
            }
            Sleep(500);
            waited += 500;
        }
        Log::Info("nioh2.exe base=0x%llX size=0x%zX", static_cast<unsigned long long>(game.base), game.size);

#ifdef _DEBUG
        if (g_config.enableWatch)
            DebugWatch::Start(dllDir);
#else
        if (g_config.enableWatch)
            Log::Warn("EnableWatch=1 只在 Debug 版有效，Release 版忽略");
#endif

        // v0.5.0 由 Nioh2AutoPickup 改名而來：舊資料夾沒刪時兩個 DLL 會重複 hook 同一個函式，交給舊版處理。
        if (GetModuleHandleW(L"Nioh2AutoPickup.dll"))
        {
            Log::Warn("偵測到舊版 Nioh2AutoPickup.dll 也已載入，本版不掛 hook。"
                      "請刪除 Mods\\Nioh2AutoPickup 資料夾後重開遊戲");
        }
        else if (Hooks::Init() && SetupGameHooks(game))
        {
            g_featureReady = true;
            Log::Info("初始化完成，擋掉落功能就緒（目前%s）", g_enabled ? "開啟" : "關閉");
        }
        else
        {
            Log::Warn("擋掉落功能已安全停用（不影響遊戲）；熱鍵與 log 仍正常運作");
        }

        HotkeyLoop();
        return 0;
    }

    void Shutdown()
    {
        g_stop = true;
        g_featureReady = false;
#ifdef _DEBUG
        DebugWatch::Stop();
#endif
        Hooks::Shutdown();
        Log::Info("卸載");
        Log::Shutdown();
    }

    bool IsEnabled()
    {
        return g_featureReady && g_enabled;
    }

    int GetItemType(uint16_t itemId)
    {
        if (!g_getItemParam || !g_paramRootGlobal)
            return -1;
        __try
        {
            const uintptr_t root = *reinterpret_cast<const uintptr_t*>(g_paramRootGlobal);
            if (!root)
                return -1;
            const uintptr_t holder = *reinterpret_cast<const uintptr_t*>(root + Game::kParamRootToHolder);
            if (!holder)
                return -1;
            void* table = *reinterpret_cast<void* const*>(holder + Game::kHolderToParamTable);
            if (!table)
                return -1;
            const uint8_t* param = g_getItemParam(table, itemId);
            return param ? param[Game::kItemParamTypeOffset] : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }
}
