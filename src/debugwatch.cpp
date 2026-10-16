// 逆向用除錯功能只編進 Debug 版；Release 版不含 debugwatch（含 dumpinv）。
#ifdef _DEBUG

#include "debugwatch.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <share.h>
#include <vector>

#include "game.h"
#include "log.h"
#include "mod.h"
#include "scanner.h"

namespace
{
    constexpr int kMaxHits = 64;
    constexpr int kMaxFrames = 10;
    constexpr DWORD kPollMs = 250;
    constexpr DWORD kSummaryMs = 3000;

    struct Hit
    {
        std::atomic<uintptr_t> rip{0};
        std::atomic<long> count{0};
        CONTEXT context{}; // 第一次命中時的暫存器
        uintptr_t frames[kMaxFrames] = {}; // 呼叫堆疊（返回位址）
        // 每層 unwind 後的 nonvolatile 暫存器：rbx rbp rsi rdi r12 r13 r14 r15
        uint64_t frameRegs[kMaxFrames][8] = {};
        int frameCount = 0;
        std::atomic<bool> ready{false};
        bool reported = false;
    };

    Hit g_hits[kMaxHits];
    std::atomic<int> g_hitCount = 0;
    std::atomic<bool> g_tableLock = false;
    std::atomic<bool> g_stop = false;
    PVOID g_handler = nullptr;
    HANDLE g_thread = nullptr;
    std::wstring g_watchPath;

    // 用 .pdata 的 unwind 資訊從例外當下的 context 往上回溯。
    int CaptureFrames(const CONTEXT& start, uintptr_t* frames, uint64_t (*regs)[8], int maxFrames)
    {
        CONTEXT ctx = start;
        int count = 0;
        while (count < maxFrames)
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (fn)
            {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
            else
            {
                // leaf function：返回位址就在 [rsp]
                if (!Mem::SafeRead(reinterpret_cast<const void*>(ctx.Rsp), &ctx.Rip, sizeof(ctx.Rip)))
                    break;
                ctx.Rsp += 8;
            }
            if (ctx.Rip == 0)
                break;
            const uint64_t saved[8] = {ctx.Rbx, ctx.Rbp, ctx.Rsi, ctx.Rdi, ctx.R12, ctx.R13, ctx.R14, ctx.R15};
            memcpy(regs[count], saved, sizeof(saved));
            frames[count++] = ctx.Rip;
        }
        return count;
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* ctx = info->ContextRecord;
        if (!(ctx->Dr6 & 0x1))
            return EXCEPTION_CONTINUE_SEARCH;

        // 資料中斷點是 trap：Rip 指向「觸發指令的下一條」。
        const uintptr_t rip = ctx->Rip;
        const int n = g_hitCount.load();
        bool found = false;
        for (int i = 0; i < n; ++i)
        {
            if (g_hits[i].rip.load() == rip)
            {
                g_hits[i].count.fetch_add(1);
                found = true;
                break;
            }
        }
        if (!found)
        {
            bool expected = false;
            while (!g_tableLock.compare_exchange_weak(expected, true))
                expected = false;
            const int m = g_hitCount.load();
            bool again = false;
            for (int i = n; i < m; ++i)
            {
                if (g_hits[i].rip.load() == rip)
                {
                    g_hits[i].count.fetch_add(1);
                    again = true;
                    break;
                }
            }
            if (!again && m < kMaxHits)
            {
                Hit& h = g_hits[m];
                h.context = *ctx;
                h.frameCount = CaptureFrames(*ctx, h.frames, h.frameRegs, kMaxFrames);
                h.count = 1;
                h.rip = rip;
                h.ready = true;
                g_hitCount = m + 1;
            }
            g_tableLock = false;
        }

        ctx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // 對所有其他執行緒設定 DR0/DR7。dr7 = 0 代表清除。
    int ApplyToThreads(uintptr_t address, DWORD64 dr7)
    {
        std::vector<DWORD> threads;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return 0;
        THREADENTRY32 entry{sizeof(entry)};
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry))
        {
            if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self)
                threads.push_back(entry.th32ThreadID);
        }
        CloseHandle(snap);

        int applied = 0;
        for (DWORD tid : threads)
        {
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
            if (!thread)
                continue;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &ctx))
                {
                    ctx.Dr0 = address;
                    ctx.Dr6 = 0;
                    ctx.Dr7 = dr7;
                    if (SetThreadContext(thread, &ctx))
                        ++applied;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        return applied;
    }

    std::string DescribeAddress(uintptr_t address)
    {
        HMODULE module = nullptr;
        char buffer[MAX_PATH + 32];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module) &&
            module)
        {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(module, path, MAX_PATH);
            const char* name = strrchr(path, '\\');
            snprintf(buffer, sizeof(buffer), "%s+0x%llX", name ? name + 1 : path,
                     static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(address));
        }
        return buffer;
    }

    void ReportHit(const Hit& h)
    {
        const CONTEXT& c = h.context;
        Log::Info("[WATCH] 新指令：下一條指令位於 %s（RIP=0x%llX）", DescribeAddress(c.Rip).c_str(),
                  static_cast<unsigned long long>(c.Rip));
        Log::Info("[WATCH]   RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX", c.Rax, c.Rbx, c.Rcx, c.Rdx);
        Log::Info("[WATCH]   RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
        Log::Info("[WATCH]   R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX", c.R8, c.R9, c.R10, c.R11);
        Log::Info("[WATCH]   R12=%016llX R13=%016llX R14=%016llX R15=%016llX", c.R12, c.R13, c.R14, c.R15);

        uint8_t bytes[48] = {};
        if (Mem::SafeRead(reinterpret_cast<const void*>(c.Rip - 32), bytes, sizeof(bytes)))
        {
            char hex[48 * 3 + 8] = {};
            char* p = hex;
            for (int i = 0; i < 48; ++i)
                p += sprintf_s(p, hex + sizeof(hex) - p, i == 32 ? "| %02X " : "%02X ", bytes[i]);
            Log::Info("[WATCH]   bytes[RIP-32..RIP+16]: %s", hex);
        }
        for (int i = 0; i < h.frameCount; ++i)
        {
            const uint64_t* r = h.frameRegs[i];
            Log::Info("[WATCH]   呼叫堆疊 #%d：%s", i, DescribeAddress(h.frames[i]).c_str());
            Log::Info("[WATCH]     RBX=%llX RBP=%llX RSI=%llX RDI=%llX", r[0], r[1], r[2], r[3]);
            Log::Info("[WATCH]     R12=%llX R13=%llX R14=%llX R15=%llX", r[4], r[5], r[6], r[7]);
        }
    }

    void ResetTable()
    {
        for (auto& h : g_hits)
        {
            h.ready = false;
            h.rip = 0;
            h.count = 0;
            h.reported = false;
        }
        g_hitCount = 0;
    }

    // dumpinv <hex 位址> <格數>：把背包格子（0x88 bytes）的 ID、數量、+0x14、道具類型寫進 log。
    void DumpInventory(uintptr_t address, int count)
    {
        int typeCounts[257] = {};
        int valid = 0;
        for (int i = 0; i < count; ++i)
        {
            Game::ItemData slot{};
            if (!Mem::SafeRead(reinterpret_cast<const void*>(address + i * Game::kItemDataSize), &slot, sizeof(slot)))
                break;
            if (slot.id != slot.id2 || slot.id == 0 || slot.id == 0xFFFF)
                continue;
            uint32_t f14 = 0;
            memcpy(&f14, reinterpret_cast<const uint8_t*>(&slot) + 0x14, sizeof(f14));
            const int type = Mod::GetItemType(slot.id);
            ++typeCounts[type < 0 ? 256 : type];
            ++valid;
            Log::Info("[INV] #%d id=0x%04X count=%u f14=%u type=%s0x%02X", i, slot.id, slot.count, f14,
                      type < 0 ? "?" : "", type & 0xFF);
        }
        Log::Info("[INV] 共 %d 格有效；類型統計：", valid);
        for (int t = 0; t < 256; ++t)
            if (typeCounts[t])
                Log::Info("[INV]   type 0x%02X：%d 格", t, typeCounts[t]);
        if (typeCounts[256])
            Log::Info("[INV]   查不到類型：%d 格", typeCounts[256]);
    }

    void ApplyCommand(const char* line)
    {
        char dumpAddr[32] = {};
        int dumpCount = 0;
        if (sscanf_s(line, "dumpinv %31s %d", dumpAddr, static_cast<unsigned>(sizeof(dumpAddr)), &dumpCount) == 2)
        {
            DumpInventory(std::strtoull(dumpAddr, nullptr, 16), dumpCount);
            return;
        }

        char mode[8] = {};
        char addrText[32] = {};
        int size = 4;
        const int fields = sscanf_s(line, "%7s %31s %d", mode, static_cast<unsigned>(sizeof(mode)), addrText,
                                    static_cast<unsigned>(sizeof(addrText)), &size);

        if (fields >= 1 && _stricmp(mode, "off") == 0)
        {
            const int n = ApplyToThreads(0, 0);
            Log::Info("[WATCH] 已關閉（%d 個執行緒）", n);
            return;
        }
        if (fields < 2)
        {
            Log::Warn("[WATCH] 指令格式錯誤：%s", line);
            return;
        }

        DWORD64 rw = 0;
        if (_stricmp(mode, "w") == 0)
            rw = 1;
        else if (_stricmp(mode, "rw") == 0)
            rw = 3;
        else
        {
            Log::Warn("[WATCH] 未知模式：%s", mode);
            return;
        }

        DWORD64 len = 0;
        switch (size)
        {
        case 1: len = 0; break;
        case 2: len = 1; break;
        case 4: len = 3; break;
        case 8: len = 2; break;
        default:
            Log::Warn("[WATCH] 大小必須是 1/2/4/8：%d", size);
            return;
        }

        const uintptr_t address = std::strtoull(addrText, nullptr, 16);
        if (address == 0 || (address % size) != 0)
        {
            Log::Warn("[WATCH] 位址無效或未對齊：%s", addrText);
            return;
        }

        ApplyToThreads(0, 0);
        ResetTable();
        const DWORD64 dr7 = 0x1 | (rw << 16) | (len << 18);
        const int n = ApplyToThreads(address, dr7);
        Log::Info("[WATCH] 開始監看 %s 0x%llX（%d bytes），已套用到 %d 個執行緒", mode,
                  static_cast<unsigned long long>(address), size, n);
    }

    bool ReadCommandIfChanged(FILETIME& lastWrite, char* out, size_t outSize)
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExW(g_watchPath.c_str(), GetFileExInfoStandard, &attr))
            return false;
        if (CompareFileTime(&attr.ftLastWriteTime, &lastWrite) == 0)
            return false;
        lastWrite = attr.ftLastWriteTime;

        FILE* f = _wfsopen(g_watchPath.c_str(), L"rb", _SH_DENYNO);
        if (!f)
            return false;
        const size_t n = fread(out, 1, outSize - 1, f);
        fclose(f);
        out[n] = '\0';
        // 去掉 UTF-8 BOM
        if (n >= 3 && static_cast<uint8_t>(out[0]) == 0xEF && static_cast<uint8_t>(out[1]) == 0xBB &&
            static_cast<uint8_t>(out[2]) == 0xBF)
            memmove(out, out + 3, n - 2);
        return true;
    }

    DWORD WINAPI WatchThread(LPVOID)
    {
        FILETIME lastWrite{};
        DWORD sinceSummary = 0;
        long lastTotal = 0;
        while (!g_stop)
        {
            char command[256];
            if (ReadCommandIfChanged(lastWrite, command, sizeof(command)))
                ApplyCommand(command);

            const int n = g_hitCount.load();
            for (int i = 0; i < n; ++i)
            {
                Hit& h = g_hits[i];
                if (h.ready && !h.reported)
                {
                    h.reported = true;
                    ReportHit(h);
                }
            }

            sinceSummary += kPollMs;
            if (sinceSummary >= kSummaryMs)
            {
                sinceSummary = 0;
                long total = 0;
                for (int i = 0; i < n; ++i)
                    total += g_hits[i].count.load();
                if (total != lastTotal)
                {
                    lastTotal = total;
                    for (int i = 0; i < n; ++i)
                        Log::Info("[WATCH] 統計：%s 共 %ld 次", DescribeAddress(g_hits[i].rip.load()).c_str(),
                                  g_hits[i].count.load());
                }
            }
            Sleep(kPollMs);
        }
        return 0;
    }
}

namespace DebugWatch
{
    void Start(const std::wstring& dllDir)
    {
        if (g_thread)
            return;
        g_watchPath = dllDir + L"\\Nioh2DropFilter.watch.txt";
        g_handler = AddVectoredExceptionHandler(1, Handler);
        if (!g_handler)
        {
            Log::Error("[WATCH] AddVectoredExceptionHandler 失敗");
            return;
        }
        g_thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
        Log::Info("[WATCH] 除錯監看已啟用，指令檔：Nioh2DropFilter.watch.txt");
    }

    void Stop()
    {
        g_stop = true;
        if (g_handler)
        {
            ApplyToThreads(0, 0);
            RemoveVectoredExceptionHandler(g_handler);
            g_handler = nullptr;
        }
    }
}

#endif // _DEBUG
