// memscan：逆向輔助用的小型記憶體掃描工具（類似 Cheat Engine 的 float 掃描），不屬於 mod 本體。
//
// 用法：
//   memscan first [int|int16]           記錄目前記憶體快照（未知初始值；預設 float）
//   memscan next <cond>                 與上一次比較並篩選
//       cond: inc | dec | changed | unchanged | eq <v> [tol] | range <lo> <hi> | nonzero | delta <d>
//   memscan list [n]                    列出候選位址與目前數值
//   memscan read <addr> [count]         以 float 顯示 addr 起的數值（同時顯示 hex）
//   memscan write <addr> <float>        寫入 float
//   memscan status                      候選數量
//   memscan find <float> [tol]          直接搜尋 float
//   memscan findint <int> [2|4]         直接搜尋整數（int16 / int32），之後可用 next eq <v> 篩選
//   memscan findvec xlo xhi ylo yhi zlo zhi   搜尋 (x,y,z,1.0) vec4
//   memscan findptr <addr> [slack] [n]  搜尋指向 [addr-slack, addr] 的指標
//
// 掃描所有已提交、可寫（RW / WC / ERW）的記憶體。

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr float kMaxAbs = 1.0e6f; // 座標不可能超出的範圍，用來排除雜訊

    struct Candidate
    {
        uint64_t address;
        float value;
        uint32_t type; // 0 = float，2 = int16，4 = int32
    };

    float ReadAs(const void* p, uint32_t type)
    {
        switch (type)
        {
        case 2: return static_cast<float>(*static_cast<const int16_t*>(p));
        case 4: return static_cast<float>(*static_cast<const int32_t*>(p));
        default: return *static_cast<const float*>(p);
        }
    }

    struct RegionHeader
    {
        uint64_t base;
        uint64_t size;
    };

    fs::path DataDir()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        fs::path dir = fs::path(path).parent_path() / "data";
        fs::create_directories(dir);
        return dir;
    }

    DWORD FindProcess(const wchar_t* name)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return 0;
        PROCESSENTRY32W entry{sizeof(entry)};
        DWORD pid = 0;
        for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
        {
            if (_wcsicmp(entry.szExeFile, name) == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        }
        CloseHandle(snap);
        return pid;
    }

    bool Plausible(float v)
    {
        return std::isfinite(v) && std::fabs(v) < kMaxAbs;
    }

    std::vector<MEMORY_BASIC_INFORMATION> ScanRegions(HANDLE process)
    {
        std::vector<MEMORY_BASIC_INFORMATION> regions;
        uintptr_t address = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        while (VirtualQueryEx(process, reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            const DWORD prot = mbi.Protect & 0xFF;
            const bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
            if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & PAGE_GUARD))
                regions.push_back(mbi);
            address = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        return regions;
    }

    enum class Cond
    {
        Inc,
        Dec,
        Changed,
        Unchanged,
        Eq,
        Range,
        NonZero,
        Delta
    };

    struct Filter
    {
        Cond cond = Cond::Changed;
        float a = 0, b = 0;

        bool Test(float oldV, float newV) const
        {
            if (!Plausible(newV))
                return false;
            switch (cond)
            {
            case Cond::Inc: return newV > oldV;
            case Cond::Dec: return newV < oldV;
            case Cond::Changed: return newV != oldV;
            case Cond::Unchanged: return newV == oldV;
            case Cond::Eq: return std::fabs(newV - a) <= b;
            case Cond::Range: return newV >= a && newV <= b;
            case Cond::NonZero: return std::fabs(newV) > 1.0e-4f;
            case Cond::Delta: return newV - oldV == a;
            }
            return false;
        }
    };

    bool ParseFilter(int argc, wchar_t** argv, Filter& f)
    {
        if (argc < 3)
            return false;
        const std::wstring c = argv[2];
        if (c == L"inc") f.cond = Cond::Inc;
        else if (c == L"dec") f.cond = Cond::Dec;
        else if (c == L"changed") f.cond = Cond::Changed;
        else if (c == L"unchanged") f.cond = Cond::Unchanged;
        else if (c == L"nonzero") f.cond = Cond::NonZero;
        else if (c == L"delta" && argc >= 4)
        {
            f.cond = Cond::Delta;
            f.a = std::wcstof(argv[3], nullptr);
        }
        else if (c == L"eq" && argc >= 4)
        {
            f.cond = Cond::Eq;
            f.a = std::wcstof(argv[3], nullptr);
            f.b = argc >= 5 ? std::wcstof(argv[4], nullptr) : 0.01f;
        }
        else if (c == L"range" && argc >= 5)
        {
            f.cond = Cond::Range;
            f.a = std::wcstof(argv[3], nullptr);
            f.b = std::wcstof(argv[4], nullptr);
        }
        else
            return false;
        return true;
    }

    std::vector<Candidate> LoadCandidates(const fs::path& path)
    {
        std::vector<Candidate> result;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
            return result;
        const auto size = fs::file_size(path);
        result.resize(size / sizeof(Candidate));
        fread(result.data(), sizeof(Candidate), result.size(), f);
        fclose(f);
        return result;
    }

    void SaveCandidates(const fs::path& path, const std::vector<Candidate>& list)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
            return;
        fwrite(list.data(), sizeof(Candidate), list.size(), f);
        fclose(f);
    }

    // type：0 = float，2 = int16，4 = int32。寫在快照檔開頭。
    int CmdFirst(HANDLE process, const fs::path& dir, uint32_t type)
    {
        fs::remove(dir / "candidates.bin");
        FILE* out = nullptr;
        if (_wfopen_s(&out, (dir / "snapshot.bin").c_str(), L"wb") != 0 || !out)
        {
            printf("無法寫入快照檔\n");
            return 1;
        }
        fwrite(&type, sizeof(type), 1, out);
        std::vector<uint8_t> buffer;
        uint64_t total = 0;
        size_t count = 0;
        for (const auto& r : ScanRegions(process))
        {
            buffer.resize(r.RegionSize);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process, r.BaseAddress, buffer.data(), r.RegionSize, &read) || read != r.RegionSize)
                continue;
            RegionHeader h{reinterpret_cast<uint64_t>(r.BaseAddress), r.RegionSize};
            fwrite(&h, sizeof(h), 1, out);
            fwrite(buffer.data(), 1, r.RegionSize, out);
            total += r.RegionSize;
            ++count;
        }
        fclose(out);
        printf("快照完成：%zu 個區段，%.1f MB\n", count, total / 1048576.0);
        return 0;
    }

    int CmdNextFromSnapshot(HANDLE process, const fs::path& dir, const Filter& filter)
    {
        FILE* in = nullptr;
        if (_wfopen_s(&in, (dir / "snapshot.bin").c_str(), L"rb") != 0 || !in)
        {
            printf("沒有快照，請先執行 first\n");
            return 1;
        }
        uint32_t type = 0;
        fread(&type, sizeof(type), 1, in);
        const size_t step = type == 2 ? 2 : 4;
        std::vector<Candidate> result;
        std::vector<uint8_t> oldBuf, newBuf;
        RegionHeader h{};
        while (fread(&h, sizeof(h), 1, in) == 1)
        {
            oldBuf.resize(h.size);
            newBuf.resize(h.size);
            fread(oldBuf.data(), 1, h.size, in);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process, reinterpret_cast<void*>(h.base), newBuf.data(), h.size, &read) || read != h.size)
                continue;
            for (size_t off = 0; off + step <= h.size; off += step)
            {
                const float o = ReadAs(oldBuf.data() + off, type);
                const float n = ReadAs(newBuf.data() + off, type);
                // 整數模式只看 0..100000（道具數量範圍），減少雜訊
                const bool ok = type == 0 ? Plausible(o) : (o >= 0.0f && o <= 100000.0f);
                if (ok && filter.Test(o, n))
                    result.push_back({h.base + off, n, type});
            }
        }
        fclose(in);
        fs::remove(dir / "snapshot.bin");
        SaveCandidates(dir / "candidates.bin", result);
        printf("剩下 %zu 個候選\n", result.size());
        return 0;
    }

    int CmdNext(HANDLE process, const fs::path& dir, const Filter& filter)
    {
        if (fs::exists(dir / "snapshot.bin"))
            return CmdNextFromSnapshot(process, dir, filter);

        std::vector<Candidate> list = LoadCandidates(dir / "candidates.bin");
        if (list.empty())
        {
            printf("沒有候選，請先執行 first\n");
            return 1;
        }
        // 以 64KB 為單位批次讀取，避免每個候選各呼叫一次 ReadProcessMemory。
        constexpr uint64_t kBlock = 0x10000;
        std::vector<uint8_t> block(kBlock);
        std::vector<Candidate> result;
        uint64_t blockBase = UINT64_MAX;
        bool blockValid = false;
        for (const Candidate& c : list)
        {
            const uint64_t base = c.address & ~(kBlock - 1);
            if (base != blockBase)
            {
                blockBase = base;
                SIZE_T read = 0;
                // 區塊中可能有部分不可讀，失敗時退回逐一讀取
                blockValid = ReadProcessMemory(process, reinterpret_cast<void*>(base), block.data(), kBlock, &read) &&
                             read == kBlock;
            }
            float now = 0;
            if (blockValid)
                now = ReadAs(block.data() + (c.address - base), c.type);
            else
            {
                uint8_t raw[4] = {};
                if (!ReadProcessMemory(process, reinterpret_cast<void*>(c.address), raw, c.type == 2 ? 2 : 4, nullptr))
                    continue;
                now = ReadAs(raw, c.type);
            }
            if (filter.Test(c.value, now))
                result.push_back({c.address, now, c.type});
        }
        SaveCandidates(dir / "candidates.bin", result);
        printf("剩下 %zu 個候選\n", result.size());
        return 0;
    }

    // 直接搜尋已知數值（不需要快照），結果取代目前候選。
    int CmdFind(HANDLE process, const fs::path& dir, float value, float tol)
    {
        fs::remove(dir / "snapshot.bin");
        std::vector<Candidate> result;
        std::vector<uint8_t> buffer;
        for (const auto& r : ScanRegions(process))
        {
            buffer.resize(r.RegionSize);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process, r.BaseAddress, buffer.data(), r.RegionSize, &read) || read != r.RegionSize)
                continue;
            const auto* f = reinterpret_cast<const float*>(buffer.data());
            const uint64_t base = reinterpret_cast<uint64_t>(r.BaseAddress);
            for (size_t i = 0; i < r.RegionSize / 4; ++i)
            {
                if (std::fabs(f[i] - value) <= tol)
                    result.push_back({base + i * 4, f[i], 0});
            }
        }
        SaveCandidates(dir / "candidates.bin", result);
        printf("找到 %zu 個\n", result.size());
        return 0;
    }

    // 搜尋精確整數（size = 2 或 4，依大小對齊），結果取代目前候選。
    int CmdFindInt(HANDLE process, const fs::path& dir, int32_t value, uint32_t size)
    {
        fs::remove(dir / "snapshot.bin");
        std::vector<Candidate> result;
        std::vector<uint8_t> buffer;
        for (const auto& r : ScanRegions(process))
        {
            buffer.resize(r.RegionSize);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process, r.BaseAddress, buffer.data(), r.RegionSize, &read) || read != r.RegionSize)
                continue;
            const uint64_t base = reinterpret_cast<uint64_t>(r.BaseAddress);
            for (size_t off = 0; off + size <= r.RegionSize; off += size)
            {
                const int32_t v = size == 2 ? *reinterpret_cast<const int16_t*>(buffer.data() + off)
                                            : *reinterpret_cast<const int32_t*>(buffer.data() + off);
                if (v == value)
                    result.push_back({base + off, static_cast<float>(v), size});
            }
        }
        SaveCandidates(dir / "candidates.bin", result);
        printf("找到 %zu 個\n", result.size());
        return 0;
    }

    // 搜尋 (x, y, z, 1.0) 形式的 vec4，三軸各自落在範圍內。候選位址指向 x。
    int CmdFindVec(HANDLE process, const fs::path& dir, const float (&lo)[3], const float (&hi)[3])
    {
        fs::remove(dir / "snapshot.bin");
        std::vector<Candidate> result;
        std::vector<uint8_t> buffer;
        for (const auto& r : ScanRegions(process))
        {
            buffer.resize(r.RegionSize);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process, r.BaseAddress, buffer.data(), r.RegionSize, &read) || read != r.RegionSize)
                continue;
            const auto* f = reinterpret_cast<const float*>(buffer.data());
            const uint64_t base = reinterpret_cast<uint64_t>(r.BaseAddress);
            const size_t count = r.RegionSize / 4;
            for (size_t i = 0; i + 3 < count; ++i)
            {
                if (f[i + 3] == 1.0f && f[i] >= lo[0] && f[i] <= hi[0] && f[i + 1] >= lo[1] && f[i + 1] <= hi[1] &&
                    f[i + 2] >= lo[2] && f[i + 2] <= hi[2])
                    result.push_back({base + i * 4, f[i], 0});
            }
        }
        SaveCandidates(dir / "candidates.bin", result);
        printf("找到 %zu 個\n", result.size());
        return 0;
    }

    // 搜尋指向 [lo, hi] 範圍的 8 byte 指標（8 對齊），印出位址與所屬區段類型。不影響候選清單。
    int CmdFindPtr(HANDLE process, uint64_t lo, uint64_t hi, size_t limit)
    {
        std::vector<uint8_t> buffer;
        size_t found = 0;
        for (const auto& r : ScanRegions(process))
        {
            buffer.resize(r.RegionSize);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process, r.BaseAddress, buffer.data(), r.RegionSize, &read) || read != r.RegionSize)
                continue;
            const auto* q = reinterpret_cast<const uint64_t*>(buffer.data());
            const uint64_t base = reinterpret_cast<uint64_t>(r.BaseAddress);
            for (size_t i = 0; i < r.RegionSize / 8; ++i)
            {
                if (q[i] < lo || q[i] > hi)
                    continue;
                const char* type = r.Type == MEM_IMAGE ? "IMAGE" : r.Type == MEM_MAPPED ? "MAPPED" : "PRIVATE";
                if (found < limit)
                    printf("%016llX -> %016llX  [%s, region %016llX]\n", static_cast<unsigned long long>(base + i * 8),
                           static_cast<unsigned long long>(q[i]), type, static_cast<unsigned long long>(base));
                ++found;
            }
        }
        printf("共 %zu 個\n", found);
        return 0;
    }

    int CmdList(HANDLE process, const fs::path& dir, size_t limit)
    {
        const std::vector<Candidate> list = LoadCandidates(dir / "candidates.bin");
        printf("共 %zu 個候選\n", list.size());
        for (size_t i = 0; i < list.size() && i < limit; ++i)
        {
            uint8_t raw[4] = {};
            ReadProcessMemory(process, reinterpret_cast<void*>(list[i].address), raw, list[i].type == 2 ? 2 : 4, nullptr);
            const float now = ReadAs(raw, list[i].type);
            printf("%3zu  %016llX  %12.4f\n", i, static_cast<unsigned long long>(list[i].address), now);
        }
        return 0;
    }

    int CmdRead(HANDLE process, uint64_t address, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const uint64_t a = address + i * 4;
            uint32_t raw = 0;
            if (!ReadProcessMemory(process, reinterpret_cast<void*>(a), &raw, 4, nullptr))
            {
                printf("%016llX  (無法讀取)\n", static_cast<unsigned long long>(a));
                continue;
            }
            float f = 0;
            memcpy(&f, &raw, 4);
            printf("%016llX  +%03X  %08X  %14.4f\n", static_cast<unsigned long long>(a), i * 4, raw, f);
        }
        return 0;
    }

    int CmdWrite(HANDLE process, uint64_t address, float value)
    {
        SIZE_T written = 0;
        if (!WriteProcessMemory(process, reinterpret_cast<void*>(address), &value, 4, &written) || written != 4)
        {
            printf("寫入失敗（錯誤 %lu）\n", GetLastError());
            return 1;
        }
        printf("已寫入 %016llX = %.4f\n", static_cast<unsigned long long>(address), value);
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2)
    {
        printf("用法：memscan first | next <cond> | list [n] | read <addr> [count] | write <addr> <float> | status\n");
        return 1;
    }
    const std::wstring cmd = argv[1];
    const fs::path dir = DataDir();

    if (cmd == L"status")
    {
        printf("快照：%s，候選：%zu\n", fs::exists(dir / "snapshot.bin") ? "有" : "無",
               LoadCandidates(dir / "candidates.bin").size());
        return 0;
    }

    const DWORD pid = FindProcess(L"nioh2.exe");
    if (!pid)
    {
        printf("找不到 nioh2.exe，請先開遊戲\n");
        return 1;
    }
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                                 FALSE, pid);
    if (!process)
    {
        printf("OpenProcess 失敗（錯誤 %lu）\n", GetLastError());
        return 1;
    }

    int rc = 1;
    if (cmd == L"first")
    {
        uint32_t type = 0;
        if (argc >= 3 && std::wstring(argv[2]) == L"int") type = 4;
        if (argc >= 3 && std::wstring(argv[2]) == L"int16") type = 2;
        rc = CmdFirst(process, dir, type);
    }
    else if (cmd == L"next")
    {
        Filter f;
        if (ParseFilter(argc, argv, f))
            rc = CmdNext(process, dir, f);
        else
            printf("條件錯誤：inc | dec | changed | unchanged | eq <v> [tol] | range <lo> <hi>\n");
    }
    else if (cmd == L"find" && argc >= 3)
        rc = CmdFind(process, dir, std::wcstof(argv[2], nullptr), argc >= 4 ? std::wcstof(argv[3], nullptr) : 0.01f);
    else if (cmd == L"findint" && argc >= 3)
        rc = CmdFindInt(process, dir, _wtoi(argv[2]), argc >= 4 && _wtoi(argv[3]) == 2 ? 2u : 4u);
    else if (cmd == L"findvec" && argc >= 8)
    {
        const float lo[3] = {std::wcstof(argv[2], nullptr), std::wcstof(argv[4], nullptr), std::wcstof(argv[6], nullptr)};
        const float hi[3] = {std::wcstof(argv[3], nullptr), std::wcstof(argv[5], nullptr), std::wcstof(argv[7], nullptr)};
        rc = CmdFindVec(process, dir, lo, hi);
    }
    else if (cmd == L"findptr" && argc >= 3)
    {
        // findptr <目標> [往前容許的位移，預設 0] [最多列幾筆]
        const uint64_t target = std::wcstoull(argv[2], nullptr, 16);
        const uint64_t slack = argc >= 4 ? std::wcstoull(argv[3], nullptr, 16) : 0;
        rc = CmdFindPtr(process, target - slack, target, argc >= 5 ? std::wcstoul(argv[4], nullptr, 10) : 100);
    }
    else if (cmd == L"list")
        rc = CmdList(process, dir, argc >= 3 ? std::wcstoul(argv[2], nullptr, 10) : 50);
    else if (cmd == L"read" && argc >= 3)
        rc = CmdRead(process, std::wcstoull(argv[2], nullptr, 16), argc >= 4 ? _wtoi(argv[3]) : 16);
    else if (cmd == L"write" && argc >= 4)
        rc = CmdWrite(process, std::wcstoull(argv[2], nullptr, 16), std::wcstof(argv[3], nullptr));
    else
        printf("未知指令\n");

    CloseHandle(process);
    return rc;
}
