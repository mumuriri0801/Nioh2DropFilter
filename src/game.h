#pragma once

#include <cstddef>
#include <cstdint>

// 遊戲結構與特徵碼定義。內容一律以 NOTES.md 中「已確認」的項目為準，推測的偏移要加註「推測」。

namespace Game
{
    // 敵人掉落流程（SpawnEnemyLoot，nioh2.exe+0x1095EB0）中呼叫擲骰函式的位置：
    //   F3 0F 10 9D xx xx xx xx    movss xmm3, [rbp+...]
    //   4D 8B C6                   mov r8, r14            ; 敵人角色
    //   48 8D 54 24 xx             lea rdx, [rsp+...]     ; 結果 vector
    //   48 8D 4D xx                lea rcx, [rbp+...]
    //   E8 xx xx xx xx             call RollLoot          ; ← +0x14
    //   48 8B 7C 24 xx             mov rdi, [rsp+...]     ; ← +0x19 = 返回位址
    //   48 3B 7C 24                cmp rdi, [rsp+...]
    inline constexpr const char* kEnemyLootRollCallPattern =
        "F3 0F 10 9D ?? ?? ?? ?? 4D 8B C6 48 8D 54 24 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B 7C 24 ?? 48 3B 7C 24";
    inline constexpr size_t kEnemyLootRollCallOffset = 0x14;   // E8 所在位置
    inline constexpr size_t kEnemyLootRollReturnOffset = 0x19; // call 的下一條指令

    // 擲骰結果中的一筆道具，與背包格子同一種結構（已確認 0x88 bytes，迴圈 add rdi, 88h）。
    inline constexpr size_t kItemDataSize = 0x88;
    struct ItemData
    {
        uint16_t id;     // +0x00 道具 ID（已確認：良質皮甲片 = 0xCFA3）
        uint16_t id2;    // +0x02 推測：基礎 ID（皮甲片時與 id 相同）
        uint16_t count;  // +0x04 數量（已確認）
        uint8_t rest[kItemDataSize - 6];
    };
    static_assert(sizeof(ItemData) == kItemDataSize);

    // MSVC std::vector<ItemData> 的記憶體配置
    struct ItemVector
    {
        ItemData* begin;
        ItemData* end;
        ItemData* capacity;
    };

    // 加入背包函式（nioh2.exe+0xA059F0）中查道具參數的位置：
    //   48 8B 05 xx xx xx xx       mov rax, [rip+...]     ; 根指標 ← +0x00（disp +3，長度 7）
    //   48 8B 48 20                mov rcx, [rax+20h]
    //   41 0F B7 1E                movzx ebx, word [r14]  ; 道具 ID
    //   8B D3                      mov edx, ebx
    //   48 8B 89 28 04 00 00       mov rcx, [rcx+428h]    ; 道具參數表
    //   E8 xx xx xx xx             call GetItemParam      ; ← +0x18
    //   4C 8B E0                   mov r12, rax
    inline constexpr const char* kItemParamLookupPattern =
        "48 8B 05 ?? ?? ?? ?? 48 8B 48 20 41 0F B7 1E 8B D3 48 8B 89 28 04 00 00 E8 ?? ?? ?? ?? 4C 8B E0";
    inline constexpr size_t kItemParamCallOffset = 0x18;
    inline constexpr ptrdiff_t kParamRootToHolder = 0x20;  // [root + 0x20]
    inline constexpr ptrdiff_t kHolderToParamTable = 0x428; // [holder + 0x428]
    inline constexpr ptrdiff_t kItemParamTypeOffset = 0x4;  // 道具參數 +0x4 byte = 類型（已確認被用來分流；1~8、0x24 推測是裝備）

    // const void* GetItemParam(void* table, uint32_t itemId)：找不到回傳 nullptr（每筆 0x1A0 bytes）
    using GetItemParamFn = const uint8_t*(__fastcall*)(void* table, uint32_t itemId);

    // void* RollLoot(void* ctx, ItemVector* out, void* enemy, float param)
    // 參數意義為推測：ctx = 呼叫者 stack 上的擲骰狀態，enemy = 死亡的敵人角色。
    using RollLootFn = void*(__fastcall*)(void* ctx, ItemVector* out, void* enemy, float param);

    struct Vec3
    {
        float x, y, z;
    };
}
