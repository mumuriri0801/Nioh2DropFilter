// peinfo：對磁碟上的 nioh2.exe 做靜態分析（逆向輔助用，不屬於 mod 本體）。
// 位址一律使用 image base 0x140000000 的 VA。
//
//   peinfo <exe> func <va>          由 .pdata 找出包含 va 的函式範圍
//   peinfo <exe> calls <va>         找出所有 call/jmp rel32 到 va 的位置
//   peinfo <exe> xref <va>          找出所有 RIP 相對引用 va 的位置（lea/mov 等，近似比對）
//   peinfo <exe> qword <va>         在檔案中搜尋 8 byte 值 va（例如 vtable 裡的函式指標）
//   peinfo <exe> bytes <va> [n]     印出 va 起 n 個位元組
//   peinfo <exe> aob "<pattern>"    以 AOB 特徵碼搜尋 .text，印出所有符合位置

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    struct Section
    {
        char name[9];
        uint32_t va, vsize, raw, rawSize, flags;
    };

    std::vector<uint8_t> g_file;
    std::vector<Section> g_sections;
    uint64_t g_imageBase = 0;

    bool Load(const wchar_t* path)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f)
            return false;
        fseek(f, 0, SEEK_END);
        g_file.resize(static_cast<size_t>(ftell(f)));
        fseek(f, 0, SEEK_SET);
        fread(g_file.data(), 1, g_file.size(), f);
        fclose(f);

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_file.data());
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_file.data() + dos->e_lfanew);
        g_imageBase = nt->OptionalHeader.ImageBase;
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        {
            Section s{};
            memcpy(s.name, sec->Name, 8);
            s.va = sec->VirtualAddress;
            s.vsize = sec->Misc.VirtualSize;
            s.raw = sec->PointerToRawData;
            s.rawSize = sec->SizeOfRawData;
            s.flags = sec->Characteristics;
            g_sections.push_back(s);
        }
        return true;
    }

    const Section* FindSection(const char* name)
    {
        for (const auto& s : g_sections)
            if (strncmp(s.name, name, 8) == 0)
                return &s;
        return nullptr;
    }

    // RVA → 檔案內指標；不在任何區段的原始資料內則回傳 nullptr
    const uint8_t* At(uint32_t rva)
    {
        for (const auto& s : g_sections)
            if (rva >= s.va && rva < s.va + s.rawSize)
                return g_file.data() + s.raw + (rva - s.va);
        return nullptr;
    }

    uint32_t ToRva(uint64_t va) { return static_cast<uint32_t>(va - g_imageBase); }
    uint64_t ToVa(uint32_t rva) { return g_imageBase + rva; }

    int CmdFunc(uint64_t va)
    {
        const Section* pdata = FindSection(".pdata");
        if (!pdata)
            return 1;
        const uint32_t rva = ToRva(va);
        auto* fn = reinterpret_cast<const RUNTIME_FUNCTION*>(g_file.data() + pdata->raw);
        const size_t count = pdata->vsize / sizeof(RUNTIME_FUNCTION);
        for (size_t i = 0; i < count; ++i)
        {
            if (rva >= fn[i].BeginAddress && rva < fn[i].EndAddress)
            {
                printf("函式 %llX - %llX（unwind %X）\n", ToVa(fn[i].BeginAddress), ToVa(fn[i].EndAddress),
                       fn[i].UnwindData);
                // chained unwind：真正的函式起點可能在更前面
                const uint8_t* unwind = At(fn[i].UnwindData);
                if (unwind && (unwind[0] >> 3) & UNW_FLAG_CHAININFO)
                {
                    const uint8_t codes = unwind[2];
                    auto* parent = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + ((codes + 1) & ~1) * 2);
                    printf("  chained → 主函式 %llX - %llX\n", ToVa(parent->BeginAddress), ToVa(parent->EndAddress));
                }
                return 0;
            }
        }
        printf("找不到（可能是 leaf function）\n");
        return 0;
    }

    void ForEachText(const auto& fn)
    {
        for (const auto& s : g_sections)
        {
            if (!(s.flags & IMAGE_SCN_MEM_EXECUTE))
                continue;
            const uint8_t* base = g_file.data() + s.raw;
            const uint32_t size = std::min(s.rawSize, s.vsize);
            fn(base, s.va, size);
        }
    }

    int CmdCalls(uint64_t target)
    {
        int n = 0;
        ForEachText([&](const uint8_t* p, uint32_t va, uint32_t size) {
            for (uint32_t i = 0; i + 5 <= size; ++i)
            {
                if (p[i] != 0xE8 && p[i] != 0xE9)
                    continue;
                const int32_t rel = *reinterpret_cast<const int32_t*>(p + i + 1);
                if (ToVa(va + i + 5) + rel == target)
                {
                    printf("%s %llX\n", p[i] == 0xE8 ? "call" : "jmp ", ToVa(va + i));
                    ++n;
                }
            }
        });
        printf("共 %d 個\n", n);
        return 0;
    }

    int CmdXref(uint64_t target)
    {
        int n = 0;
        ForEachText([&](const uint8_t* p, uint32_t va, uint32_t size) {
            for (uint32_t i = 0; i + 4 <= size; ++i)
            {
                const int32_t disp = *reinterpret_cast<const int32_t*>(p + i);
                // disp 之後可能還接 0/1/4 byte 的 immediate
                for (int tail : {0, 1, 4})
                {
                    if (ToVa(va + i + 4 + tail) + disp == target)
                    {
                        // 印出 disp 前 3 byte 協助判斷指令（例：48 8D 05 = lea rax,[rip+...]）
                        printf("%llX  前導 %02X %02X %02X  tail=%d\n", ToVa(va + i), i >= 3 ? p[i - 3] : 0,
                               i >= 2 ? p[i - 2] : 0, i >= 1 ? p[i - 1] : 0, tail);
                        ++n;
                    }
                }
            }
        });
        printf("共 %d 個\n", n);
        return 0;
    }

    int CmdQword(uint64_t value)
    {
        int n = 0;
        for (const auto& s : g_sections)
        {
            const uint8_t* base = g_file.data() + s.raw;
            for (uint32_t i = 0; i + 8 <= s.rawSize; i += 8)
            {
                if (*reinterpret_cast<const uint64_t*>(base + i) == value)
                {
                    printf("%llX  [%.8s]\n", ToVa(s.va + i), s.name);
                    ++n;
                }
            }
        }
        printf("共 %d 個\n", n);
        return 0;
    }

    int CmdBytes(uint64_t va, int count)
    {
        const uint8_t* p = At(ToRva(va));
        if (!p)
            return 1;
        for (int i = 0; i < count; ++i)
            printf("%02X%s", p[i], (i % 16 == 15) ? "\n" : " ");
        printf("\n");
        return 0;
    }

    int CmdAob(const char* pattern)
    {
        std::vector<int> bytes;
        for (const char* c = pattern; *c;)
        {
            if (*c == ' ') { ++c; continue; }
            if (*c == '?') { bytes.push_back(-1); c += (c[1] == '?') ? 2 : 1; continue; }
            char hex[3] = {c[0], c[1], 0};
            bytes.push_back(static_cast<int>(strtol(hex, nullptr, 16)));
            c += 2;
        }
        int n = 0;
        ForEachText([&](const uint8_t* p, uint32_t va, uint32_t size) {
            for (uint32_t i = 0; i + bytes.size() <= size; ++i)
            {
                bool ok = true;
                for (size_t j = 0; j < bytes.size() && ok; ++j)
                    ok = bytes[j] < 0 || p[i + j] == bytes[j];
                if (ok)
                {
                    printf("%llX\n", ToVa(va + i));
                    ++n;
                }
            }
        });
        printf("共 %d 個\n", n);
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 4 || !Load(argv[1]))
    {
        printf("用法：peinfo <exe> func|calls|xref|qword|bytes|aob <參數>\n");
        return 1;
    }
    const std::wstring cmd = argv[2];
    const uint64_t va = std::wcstoull(argv[3], nullptr, 16);
    if (cmd == L"func") return CmdFunc(va);
    if (cmd == L"calls") return CmdCalls(va);
    if (cmd == L"xref") return CmdXref(va);
    if (cmd == L"qword") return CmdQword(va);
    if (cmd == L"bytes") return CmdBytes(va, argc >= 5 ? _wtoi(argv[4]) : 64);
    if (cmd == L"aob")
    {
        char narrow[1024] = {};
        WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, narrow, sizeof(narrow), nullptr, nullptr);
        return CmdAob(narrow);
    }
    printf("未知指令\n");
    return 1;
}
