# Nioh 2 掉落控制模組（Nioh2DropFilter）

## 專案目標

替《仁王 2》PC 版（Steam，The Complete Edition）製作一個原生 x64 DLL 模組：
**敵人 / BOSS 死亡時不掉落一般道具與裝備，只保留劇情 / 任務必要的關鍵道具。**

> 2026-10-15 方向變更：原本的目標是「靠近掉落物自動撿起」，改為擋掉掉落。2026-10-16 專案由 Nioh2AutoPickup 改名為 Nioh2DropFilter（v0.5.0）。
> 關鍵道具在遊戲背包裡是獨立分頁，代表遊戲內部有類別可判斷。

- 由 [Nioh2ModLoader](https://www.nexusmods.com/nioh2/mods/671) 載入（`dinput8.dll` proxy）
- 安裝位置：`<遊戲目錄>/mods/Nioh2DropFilter/Nioh2DropFilter.dll`
- 只在單機 / 離線模式使用，不處理連線模式

## 開發環境

- Windows 11 x64
- Visual Studio 2022（C++ 桌面開發工作負載），C++20
- 從命令列用 `msbuild` 編譯 **x64 Release**（不要建置 x86）
- 第三方函式庫：
  - [MinHook](https://github.com/TsudaKageyu/minhook)：函式 hook，以原始碼放在 `external/minhook/`
  - 設定檔用簡單的 ini 解析（自己寫或用 Windows `GetPrivateProfileString`）
- 逆向工具（由使用者在本機操作）：Cheat Engine、x64dbg

## 硬性規則

1. **不要在 `DllMain` 裡做任何初始化**。`DLL_PROCESS_ATTACH` 只呼叫 `DisableThreadLibraryCalls` 並 `CreateThread` 開一個初始化執行緒，執行緒裡先 `Sleep` 等遊戲載入完成再開始。
2. **不准寫死記憶體位址**。所有函式和全域指標都用 AOB 特徵碼掃描 `nioh2.exe` 模組取得，RIP 相對位址要正確解析。
3. 每個特徵碼掃描失敗時要寫 log 並**安全地停用功能**，絕不能讓遊戲當掉。
4. 讀取遊戲記憶體前要檢查指標是否為 null；必要時用 SEH（`__try/__except`）保護。
5. 所有 log 寫到 DLL 同目錄的 `Nioh2DropFilter.log`，包含時間戳記。Debug 版另開 console 視窗。
6. 不要修改存檔、不要碰任何連線 / 反作弊相關的程式碼。
7. 每次改完程式都要用 msbuild 編譯確認可以通過，有錯誤自己修好再回報。

## 專案結構（目標）

```
Nioh2DropFilter/
├─ CLAUDE.md            ← 本檔
├─ NOTES.md             ← 逆向筆記：特徵碼、結構偏移、函式簽名
├─ Nioh2DropFilter.sln
├─ external/minhook/
├─ src/
│  ├─ dllmain.cpp       ← 只負責開執行緒
│  ├─ mod.cpp / mod.h   ← 初始化流程、主邏輯
│  ├─ log.cpp / log.h
│  ├─ config.cpp / config.h   ← 讀 Nioh2DropFilter.ini
│  ├─ scanner.cpp / scanner.h ← AOB 掃描、RIP 相對位址解析
│  ├─ hooks.cpp / hooks.h     ← MinHook 包裝
│  └─ game.h            ← 遊戲結構定義（依 NOTES.md 更新）
└─ Nioh2DropFilter.ini  ← 範例設定檔
```

## 設定檔（Nioh2DropFilter.ini）

```ini
[General]
; 1 = 擋掉敵人 / BOSS 死亡時的掉落（道具與裝備）
Enabled=1
; 開關熱鍵（虛擬鍵碼，預設 F6）
ToggleKey=0x75

[Drops]
; 把每個擲出的掉落道具 ID 寫進 log
LogDrops=1
; 照常掉落的道具 ID（關鍵道具）
KeepItemIds=

[Debug]
; 逆向用硬體中斷點監看（Nioh2DropFilter.watch.txt）
EnableWatch=0
```

實作方式見 NOTES.md「敵人掉落流程」：hook 掉落擲骰函式，從結果 vector 移除不保留的道具。

## 開發階段

請照順序做，每個階段完成並確認可運作後再進下一階段。

### 階段 1：骨架（可直接完成）
- 建立上面的專案結構、整合 MinHook
- 初始化執行緒、log、讀 ini、熱鍵開關
- AOB 掃描器：支援 `48 8B 05 ?? ?? ?? ??` 格式（`??` 為萬用字元），並提供 RIP 相對位址解析函式
- 編譯成功；使用者把 DLL 放進 mods 資料夾後，log 應出現「已載入」訊息

### 階段 2：逆向（使用者操作，你負責指導與分析）
需要找出以下三樣東西，結果記錄到 `NOTES.md`：
1. **玩家物件與座標**：教使用者用 Cheat Engine 掃描移動時變化的 float 座標，再找出指向玩家物件的指標鏈與存取它的指令
2. **地上掉落物的管理結構**：從單一掉落物的座標反推其物件，再找出遊戲用來管理所有掉落物的清單 / 陣列 / 鏈結串列
3. **撿取函式**：對背包某道具數量使用「Find out what writes to this address」，手動撿取一次後沿呼叫堆疊往上追，找到「撿起物品」的函式與其參數

使用者會貼 Cheat Engine / x64dbg 的輸出給你。你要：
- 解釋這段組合語言在做什麼、推測參數意義（x64 呼叫慣例：rcx, rdx, r8, r9）
- 產生**唯一且穩定**的 AOB 特徵碼（避開會因更新改變的位址與偏移，用 `??` 遮罩）
- 告訴使用者下一步該在哪裡設中斷點或掃描什麼

### 階段 3：實作
- hook 一個每幀都會執行的函式（例如玩家 update 或掉落物 update）
- 每幀（或每 N 幀節流）走訪掉落物清單，距離玩家小於 `Radius` 且符合過濾條件的，呼叫撿取函式
- 避免同一個物件重複撿取；在死亡、過場動畫、讀取畫面、選單開啟時停用
- 優先考慮更簡單的替代方案：若找到「玩家可互動物件」的判定，模擬按下撿取可能比直接呼叫撿取函式更安全

### 階段 4：測試與發布
- 離線測試，**測試前先備份存檔**（`Documents\KoeiTecmo\NIOH2\Savedata\`）
- 確認遊戲更新後特徵碼失效時能安全停用
- 撰寫 README（安裝步驟、設定說明、相容性）

## NOTES.md 格式

每找到一個東西就照這個格式記錄：

```markdown
### <名稱，例如 PickupItem 函式>
- 遊戲版本：
- 特徵碼：`48 89 5C 24 ?? 57 48 83 EC 20 ...`
- 掃描後處理：直接使用 / RIP 相對（offset +3, 指令長度 7）
- 簽名 / 結構：`void __fastcall PickupItem(void* player, void* dropObj, int flags)`
- 偏移：+0x40 座標 (float x3) ...
- 如何找到的：
- 備註 / 未確認：
```

## 與使用者合作的方式

- 使用者熟悉程式開發，但**不熟悉遊戲逆向工程**，指導逆向步驟時要具體到點哪個選單、掃描什麼類型的值
- 一律用**繁體中文**回覆
- 不確定的推測要明確標示「推測」，不要當成事實寫進程式碼
- 修改程式碼前先說明要改什麼；每次結束時更新 `NOTES.md`

## 第一個任務

請完成「階段 1：骨架」，編譯成功後告訴我 DLL 輸出的路徑，以及我要怎麼放進遊戲測試。
