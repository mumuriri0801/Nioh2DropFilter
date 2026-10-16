#include "scanner.h"

#include <Windows.h>
#include <Psapi.h>
#include <cctype>
#include <cstring>

#include "log.h"

namespace Scanner
{
    bool GetModule(const wchar_t* name, Module& out)
    {
        HMODULE handle = GetModuleHandleW(name);
        if (!handle)
            return false;

        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info)))
            return false;

        out.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        out.size = info.SizeOfImage;
        return true;
    }

    std::vector<int> ParsePattern(const std::string& pattern)
    {
        std::vector<int> bytes;
        size_t i = 0;
        while (i < pattern.size())
        {
            if (std::isspace(static_cast<unsigned char>(pattern[i])))
            {
                ++i;
                continue;
            }
            if (pattern[i] == '?')
            {
                bytes.push_back(-1);
                i += (i + 1 < pattern.size() && pattern[i + 1] == '?') ? 2 : 1;
                continue;
            }
            if (i + 1 >= pattern.size() || !std::isxdigit(static_cast<unsigned char>(pattern[i])) ||
                !std::isxdigit(static_cast<unsigned char>(pattern[i + 1])))
                return {};
            bytes.push_back(std::stoi(pattern.substr(i, 2), nullptr, 16));
            i += 2;
        }
        return bytes;
    }

    std::vector<uintptr_t> FindAll(const Module& module, const std::string& pattern)
    {
        std::vector<uintptr_t> results;
        const std::vector<int> bytes = ParsePattern(pattern);
        if (bytes.empty() || module.base == 0)
            return results;

        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return results;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module.base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return results;

        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
        {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;

            const auto* start = reinterpret_cast<const uint8_t*>(module.base + section->VirtualAddress);
            const size_t size = section->Misc.VirtualSize;
            if (size < bytes.size())
                continue;

            for (size_t i = 0; i <= size - bytes.size(); ++i)
            {
                bool match = true;
                for (size_t j = 0; j < bytes.size(); ++j)
                {
                    if (bytes[j] != -1 && start[i + j] != static_cast<uint8_t>(bytes[j]))
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                    results.push_back(reinterpret_cast<uintptr_t>(start + i));
            }
        }
        return results;
    }

    uintptr_t FindUnique(const Module& module, const std::string& pattern, const char* name)
    {
        if (ParsePattern(pattern).empty())
        {
            Log::Error("特徵碼 [%s] 格式錯誤：%s", name, pattern.c_str());
            return 0;
        }

        const std::vector<uintptr_t> results = FindAll(module, pattern);
        if (results.empty())
        {
            Log::Error("特徵碼 [%s] 找不到（遊戲可能已更新）", name);
            return 0;
        }
        if (results.size() > 1)
        {
            Log::Error("特徵碼 [%s] 不唯一（%zu 個符合），為安全起見不使用", name, results.size());
            return 0;
        }

        Log::Info("特徵碼 [%s] 找到：nioh2.exe+0x%llX", name,
                  static_cast<unsigned long long>(results[0] - module.base));
        return results[0];
    }

    uintptr_t ResolveRip(uintptr_t instruction, size_t dispOffset, size_t length)
    {
        int32_t disp = 0;
        if (!Mem::Read(instruction + dispOffset, disp))
            return 0;
        return instruction + length + static_cast<intptr_t>(disp);
    }
}

namespace Mem
{
    bool SafeRead(const void* address, void* out, size_t size)
    {
        if (!address || !out)
            return false;
        __try
        {
            std::memcpy(out, address, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t ReadChain(uintptr_t base, const std::vector<ptrdiff_t>& offsets)
    {
        uintptr_t address = base;
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            if (address == 0)
                return 0;
            address += offsets[i];
            if (i + 1 < offsets.size() && !Read(address, address))
                return 0;
        }
        return address;
    }
}
