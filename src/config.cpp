#include "config.h"

#include <Windows.h>
#include <cwchar>

#include "log.h"

namespace
{
    std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key)
    {
        wchar_t buffer[1024] = {};
        GetPrivateProfileStringW(section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)), path.c_str());
        return buffer;
    }

    // 支援十進位與 0x 開頭的十六進位。
    int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback)
    {
        const std::wstring value = ReadString(path, section, key);
        if (value.empty())
            return fallback;
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value.c_str(), &end, 0);
        return end != value.c_str() ? static_cast<int>(parsed) : fallback;
    }

    bool ReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool fallback)
    {
        return ReadInt(path, section, key, fallback ? 1 : 0) != 0;
    }

    // 逗號分隔的 ID 清單，例如 "0xCFA3, 0x1234, 4660"
    std::vector<uint16_t> ReadIdList(const std::wstring& path, const wchar_t* section, const wchar_t* key)
    {
        std::vector<uint16_t> ids;
        const std::wstring value = ReadString(path, section, key);
        const wchar_t* p = value.c_str();
        while (*p)
        {
            wchar_t* end = nullptr;
            const unsigned long id = std::wcstoul(p, &end, 0);
            if (end == p)
            {
                ++p;
                continue;
            }
            if (id <= 0xFFFF)
                ids.push_back(static_cast<uint16_t>(id));
            else
                Log::Warn("[Drops] 清單中的 0x%lX 超出範圍，略過", id);
            p = end;
        }
        return ids;
    }
}

bool Config::Load(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    enabled = ReadBool(path, L"General", L"Enabled", enabled);
    toggleKey = ReadInt(path, L"General", L"ToggleKey", toggleKey);

    logDrops = ReadBool(path, L"Drops", L"LogDrops", logDrops);
    keepItemIds = ReadIdList(path, L"Drops", L"KeepItemIds");
    blockItemIds = ReadIdList(path, L"Drops", L"BlockItemIds");
    keepItemTypes = ReadIdList(path, L"Drops", L"KeepItemTypes");
    blockItemTypes = ReadIdList(path, L"Drops", L"BlockItemTypes");

    enableWatch = ReadBool(path, L"Debug", L"EnableWatch", enableWatch);

    if (toggleKey <= 0 || toggleKey > 0xFE)
    {
        Log::Warn("ToggleKey=0x%X 不合理，改用預設 F6 (0x75)", toggleKey);
        toggleKey = 0x75;
    }
    return true;
}

void Config::Dump() const
{
    Log::Info("設定：Enabled=%d ToggleKey=0x%02X", enabled, toggleKey);
    Log::Info("掉落：LogDrops=%d KeepItemIds=%zu 個", logDrops, keepItemIds.size());
    for (uint16_t id : keepItemIds)
        Log::Info("  保留 ID 0x%04X", id);
    Log::Info("掉落：BlockItemIds=%zu 個（優先於類型保留）", blockItemIds.size());
    for (uint16_t id : blockItemIds)
        Log::Info("  擋 ID 0x%04X", id);
    Log::Info("掉落：KeepItemTypes=%zu 個", keepItemTypes.size());
    for (uint16_t type : keepItemTypes)
        Log::Info("  保留類型 0x%02X", type);
    if (blockItemTypes.empty())
        Log::Info("掉落：BlockItemTypes 未設定 → 除保留清單外全部擋掉");
    else
    {
        Log::Info("掉落：BlockItemTypes=%zu 個（只擋這些類型，其他照常掉落）", blockItemTypes.size());
        for (uint16_t type : blockItemTypes)
            Log::Info("  擋類型 0x%02X", type);
    }
    Log::Info("除錯：EnableWatch=%d", enableWatch);
}
