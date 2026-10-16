#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Config
{
    // [General]
    bool enabled = true;   // 是否擋掉敵人掉落（可用熱鍵即時切換）
    int toggleKey = 0x75;  // VK_F6

    // [Drops]
    bool logDrops = true;                // 把每個擲出的道具 ID 寫進 log
    std::vector<uint16_t> keepItemIds;   // 這些道具 ID 照常掉落
    std::vector<uint16_t> blockItemIds;  // 這些道具 ID 一定擋掉，優先於 keepItemTypes（例如類型 0x03 的酒盅）
    std::vector<uint16_t> keepItemTypes; // 這些道具類型（參數 +0x4）照常掉落，例如關鍵道具
    // 黑名單：有設定時只擋這些類型，沒列出的類型（例如任務鑰匙）照常掉落；留空 = 全部擋掉
    std::vector<uint16_t> blockItemTypes;

    // [Debug]
    bool enableWatch = false; // 逆向用硬體中斷點監看，見 debugwatch.h

    // 讀取 ini；檔案不存在時保留預設值並回傳 false。
    bool Load(const std::wstring& path);
    void Dump() const;
};
