#include "log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace
{
    std::mutex g_mutex;
    FILE* g_file = nullptr;
#ifdef _DEBUG
    FILE* g_console = nullptr;
#endif

    void Write(const char* level, const char* fmt, va_list args)
    {
        char message[2048];
        vsnprintf(message, sizeof(message), fmt, args);

        SYSTEMTIME t;
        GetLocalTime(&t);
        char line[2200];
        snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s\n",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, message);

        std::lock_guard lock(g_mutex);
        if (g_file)
        {
            fputs(line, g_file);
            fflush(g_file);
        }
#ifdef _DEBUG
        if (g_console)
            fputs(line, g_console);
#endif
    }
}

namespace Log
{
    void Init(const std::wstring& dllDir)
    {
        std::lock_guard lock(g_mutex);
        if (g_file)
            return;

        const std::wstring path = dllDir + L"\\Nioh2DropFilter.log";
        const std::wstring previous = dllDir + L"\\Nioh2DropFilter.previous.log";
        MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        // _SH_DENYNO：遊戲執行中也能用記事本等工具讀取 log。
        g_file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);

#ifdef _DEBUG
        if (AllocConsole())
        {
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleTitleW(L"Nioh2DropFilter (Debug)");
            freopen_s(&g_console, "CONOUT$", "w", stdout);
        }
#endif
    }

    void Shutdown()
    {
        std::lock_guard lock(g_mutex);
        if (g_file)
        {
            fclose(g_file);
            g_file = nullptr;
        }
#ifdef _DEBUG
        if (g_console)
        {
            fclose(g_console);
            g_console = nullptr;
            FreeConsole();
        }
#endif
    }

    void Info(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write("INFO ", fmt, args);
        va_end(args);
    }

    void Warn(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write("WARN ", fmt, args);
        va_end(args);
    }

    void Error(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write("ERROR", fmt, args);
        va_end(args);
    }
}
