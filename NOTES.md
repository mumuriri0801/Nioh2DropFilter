# 逆向筆記

> 格式見 CLAUDE.md「NOTES.md 格式」。只有實際驗證過的才寫成事實，其餘標「推測」。

> 2026-10-16：專案改名 Nioh2AutoPickup → **Nioh2DropFilter**（v0.5.0）。下文歷史紀錄中的舊檔名保留原樣。

## 環境

- 遊戲：Nioh 2 The Complete Edition（PC）
- `nioh2.exe` 大小：33,014,240 bytes（版本號待確認，請在檔案內容 → 詳細資料查看）
- Mod loader：`dinput8.dll` proxy，啟動後約 5 秒載入 `Mods/<資料夾>/*.dll`（見 `Mods/loader.log`）
- 同時載入的其他 mod：Enhanced Controller Haptics、ParryYokai、PerfectGuardNative（之後 hook 時注意衝突）

- 實測（2026-10-15）：`nioh2.exe` 映像大小 0x34D9000，base 每次啟動不同（ASLR），確認需要 AOB 掃描
- 實測：每按一次熱鍵，`GetAsyncKeyState` 偵測到兩次按下，相隔約 150–200ms（原因未確認，推測是其他 mod 或輸入層重送）→ 已加 400ms 冷卻，實測按 3 次 = 切換 3 次，正常
- 實測：log 已改為共享開啟（`_SH_DENYNO`），遊戲執行中可直接讀

## 進度

- [x] 階段 1：骨架
- 2026-10-15 方向改為「敵人 / BOSS 死亡不掉落一般道具與裝備，只保留關鍵道具」
- [~] 玩家物件與座標（找到物件與讀取用座標，不再是必要）
- [x] 地上掉落物管理結構
- [x] 撿取函式
- [x] 敵人掉落擲骰 hook（v0.2.0，2026-10-15 實測：一般敵人不再掉落，log 記到 10 個道具全數移除）
  - 實測看到的道具 ID：0x11F8、0xD15D、0xD624、0xB714、0x7D7B、0xCA5C、0xA134、0x05E7、0x1DBF
  - 沒掉東西的敵人：vector begin/end 皆為 null（v0.2.1 修正誤報警告）
- [x] 辨識關鍵道具：類型 0x03 = 重要道具，黑名單過濾（v0.3.0，見下方「關鍵道具的處理方式」）
- [~] 階段 4：測試與發布（2026-10-15）
  - [x] `README.md`（繁中：功能、安裝、ini 說明與類型對照、相容性）
  - [x] 特徵碼唯一性複查（peinfo，`nioh2.exe` 33,014,240 bytes）：EnemyLootRollCall → `0x14109601D`（1 個）、
        ItemParamLookup → `0x140A05A69`（1 個）
  - [x] 安全停用審查（`mod.cpp`）：`SetupGameHooks` 先跑 `SetupItemParamLookup`，任一特徵碼找不到 / 不唯一、
        +0x14 / +0x18 不是 `E8`、解析位址不在 nioh2.exe 範圍、`MH_Initialize` / `MH_CreateHook` / `MH_EnableHook`
        失敗，都回傳 false → 不建立 hook、`g_featureReady` 保持 false，log 寫「擋掉落功能已安全停用」。
        hook 是最後一步才建立，所以失敗時遊戲記憶體完全沒被修改
  - [x] `[Debug] EnableWatch` 預設 0（ini 與 `config.h` 皆是）
  - [x] Release 移除 debugwatch（含 `dumpinv`）：`debugwatch.cpp/.h` 與 `mod.cpp` 的呼叫都包在 `#ifdef _DEBUG`；
        Release 版 `EnableWatch=1` 只寫警告並忽略。已確認 Release DLL 不含 `dumpinv` / `watch.txt` 字串，Debug 版保留。
        **之後要逆向（watch / dumpinv）必須用 Debug 版 DLL**（`bin\Debug\`）
  - [x] msbuild x64 Release 編譯通過

- `nioh2.exe` 磁碟上的檔案未加密，可用 dumpbin 反組譯：
  `dumpbin /disasm /range:0x<起>,0x<迄> nioh2.exe`（image base 0x140000000，.text 0x140001000–0x1415325E7）

## 工具

- `tools/memscan`：自製記憶體掃描（取代 Cheat Engine 的 float 掃描），`build.bat` 編譯；`findint`、`first int` + `next delta 1` 可找整數
- `tools/peinfo`：靜態分析 nioh2.exe（`func` 查函式範圍、`calls`/`xref` 交叉引用、`qword` 搜指標、`aob` 驗證特徵碼唯一性）
- mod 內建硬體中斷點監看：ini `[Debug] EnableWatch=1`，在 DLL 旁寫 `Nioh2DropFilter.watch.txt`（例：`w 237DDF73F64 4`），命中的指令寫進 log（**僅 Debug 版**）

## 進行中：玩家座標（尚未確認本體）

- 座標格式（推測）：float4 `(X, Y, Z, 1.0)`，Y 為高度，單位約公分（腳底與頭部高度差約 150）
- 實測的世界座標量級：X≈11600–12900、Y≈1330–1480、Z≈10600–10900（神社斜坡附近）
- 已排除（是副本，寫入無效）：
  - Y=腳底+~95 的副本（推測是碰撞體中心），寫入後每幀被遊戲蓋回 → 是「被寫入」的輸出副本
  - 另一份 `(X, 腳底Y, Z, 1)` 前面接著 `(-0.1, 96.5, 0.07, 1)` 的結構，寫入後不再更新 → 舊位置快照
- stack 上有即時的 `(X, 腳底Y, Z, 1.0)` 暫存
- 遊戲失去焦點時是否暫停：**未確認**。角色不動時，「腳底+100」副本完全不被寫入（4 秒 0 次），之前看到的背景計數可能只是使用者當時還在走動
- 自動化：`tools/findplayer.ps1`（約 18 秒）自動找玩家座標、heap 副本並寫 watch.txt；`-NoWatch` 只找不設監看
- 找副本的快速方法：`memscan findvec <xlo> <xhi> <ylo> <yhi> <zlo> <zhi>` 找 stack 上的 `(X, 腳底Y, Z, 1)`，再用 `memscan find <X>` 找 heap 副本
- watch 結果（2026-10-15）：對「腳底+100」副本的 Y 監看寫入，命中：
  - `nioh2.exe+0xCBEB7`：`movaps [rax+70h], xmm0`，xmm0 來自 `[rdi]`（呼叫者 stack 上的資料）。所在函式先 `lock cmpxchg [rcx+60h]` 取鎖，`call 0x1400CBAB0` 從池取節點、`call 0x1400BDCA0` 初始化，再寫 +0x70/+0x80 兩組 vec4。**推測**：每幀建立的臨時查詢物件（碰撞/感測球體），不是角色本體
  - `nioh2.exe+0xBE220`：setter，`[rcx+70h] = vec`（w 清 0）
  - `nioh2.exe+0xCC0A2` 附近：同樣模式，命中 1 次
- 呼叫堆疊（第二次 watch）：
  - 每幀寫入路徑：`BE220`(setter) ← `BDDE0` ← `nioh2.exe+0x81252D` ← `0x814FE0` ← `0x80E75F` ← `0x693B1E` ← `0x2EA3C1` ← 執行緒入口
  - 另一路徑經過 `Nioh2PerfectGuard.dll+0x160EC`（它 hook 了 `nioh2.exe+0x869xxx` 附近；**推測**是角色動作更新）
- `0x1408124CB` 附近（**推測**是碰撞元件更新）：
  - `rdx=[rdi+10h]`（**推測：擁有者 = 角色物件**），`r8=rbx+10h`（局部偏移，含骨骼 ID 於 +0x30）
  - `call 0x1407B7750(out, owner, offsetInfo)` 算出世界座標 → 與 shape 目前位置比較，不同就 `BDDE0` 更新
- `0x1407B7750`：`owner+4` (word) == 0 且 `owner+0x350` 非空時查骨骼表；否則 `call 0x1408C2FD0`
- `0x1408C4CF0(outMatrix, owner, boneId)`：讀 `owner+0x20`（**推測**是模型/骨架），`owner+4`==3 時改用 `owner+0x1C0`；骨骼矩陣經 vtable 取得
- 下一步：watch 已改成記錄每層 frame 的 nonvolatile 暫存器，重開後取 `0x81252D` 那層的 RDI，讀 `[RDI+10h]` 得到角色物件

### 角色物件（第三次 watch，2026-10-15）
- 在 `nioh2.exe+0x866EA5 / 0x869FC4 / 0x870885` 三層 frame 的 RDI 都是同一個物件（**推測**為玩家角色），
  `CC0A6` 命中時也在 R13
- 該物件內有多份即時座標 vec3：`+0xF00`、`+0xF10`、`+0xFC0`、`+0x1060`、`+0x1130`、`+0x1180`
- 寫入這 6 個 Y 欄位 → 下一幀全部被蓋回，角色沒反應 → 都是衍生值，但**讀取用途已足夠**（自動撿取只需讀位置）
- 指標搜尋：heap 上有 4 處存著角色指標（例：某物件 +0x8），heap 之間互相指來指去，暴力指標鏈不好收斂
- `Nioh2AutoPickup.dll` 自己的 watch 命中表也會存這個指標，搜尋時要排除
- 尚未完成：從 `nioh2.exe` 的全域變數穩定取得角色指標

## 已找到的項目

> 遊戲版本：`nioh2.exe` 33,014,240 bytes（2026-10-15 的安裝）。下列 VA 以 image base 0x140000000 計。

### 背包道具格子
- 位置：nioh2.exe .data（實測良質皮甲片在 `nioh2.exe+0x20BA260`，全域靜態，非 heap）
- 結構（與掉落物道具資料相同，0x88 bytes）：`+0x00` u16 道具 ID（良質皮甲片 = 0xCFA3）、`+0x02` u16（推測：基礎 ID）、`+0x04` u16 數量、`+0x10` u32 旗標
- 設定數量：`nioh2.exe+0xC4D030`（約）`if (!(item->flags>>18 & 1)) item->count = dx`
- 數量上限 9999；到上限時撿取不會寫入此欄位

### 掉落物管理器
- 全域指標：`[0x142286D90]`（例：`mov rbx, [rip+...]` 於 `0x14109C86D`）
- 結構：`+0x00` vtable、`+0x08/+0x10` 16 個 bucket（各 16 bytes，鏈結串列）、`+0x20` 掉落物數量
- 每個掉落物：節點位址 − 0xE8 = 物件起點；`+0x00` u32 類型（`0x64` = 道具掉落，`0x12D` 也是同類）、`+0x150` 指向道具元件
- 道具元件：`+0x48` 道具資料（0x88 bytes）、`+0x58` 旗標（bit13 推測：已撿取）、`+0x110` byte 類別（程式比對 3 / 0x21 / 0x22 / 0x23，意義未確認）
- `0x14109C7D0` DropSystem::Update(dt)：由主迴圈 `0x140A586E0` 每幀呼叫；道具掉落物超過 200 個時刪最舊
- `0x141097FA0` AddDrop(u32 類型, u16 id)：配置 0x168 bytes、`0x141092FD0` 建構、串進 bucket、計數 +1
- `0x14109AEB0` RemoveDrop(manager, drop)
- `0x14109B830` 撿取（在 `.rdata 0x1417083F0` 的狀態函式表內，每筆 0x28 bytes）：
  `call 0x140A059F0(背包管理器 [0x141E66418], out, &道具元件->+0x48, …)` 把道具加入背包

### 敵人掉落流程（擋掉落的 hook 點）
- `0x141095EB0` SpawnEnemyLoot(enemy, pos, u8, u8)：
  1. `0x141096031: call 0x140C45980` RollLoot(ctx, std::vector<ItemData>* out, enemy, float) 擲骰
  2. 對每個結果（stride 0x88）：`0x141094A80` 生成類型 0x64 掉落物 → 複製道具資料到 `drop->+0x150->+0x48`
- RollLoot 有 3 個呼叫者：`0x141096031`（敵人掉落）、`0x1410967C0`（`0x1410965D0` 內，多次擲骰取一，用途未確認）、`0x1407DA5B9`（用途未確認）
- **特徵碼**（唯一，已用 peinfo 確認）：`F3 0F 10 9D ?? ?? ?? ?? 4D 8B C6 48 8D 54 24 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B 7C 24 ?? 48 3B 7C 24`
  - 掃描後處理：+0x14 為 `E8` → RIP 相對（disp offset +1，指令長度 5）得 RollLoot；+0x19 為返回位址
- mod 做法：hook RollLoot，`_ReturnAddress()` == 敵人掉落返回位址時，從 vector 移除不在 `KeepItemIds` 的道具
- 道具參數表：`0x1407B9D30` GetItemParam(table, id)：雜湊 `[0x141E96B98]` 查索引，每筆 0x1A0 bytes
  - table = `[[0x141E19C68] + 0x20] + 0x428`
  - **特徵碼**（唯一）：`48 8B 05 ?? ?? ?? ?? 48 8B 48 20 41 0F B7 1E 8B D3 48 8B 89 28 04 00 00 E8 ?? ?? ?? ?? 4C 8B E0`（位於加入背包 `0x140A059F0` 內）
    - +0 RIP 相對（disp +3，長度 7）得根指標全域變數；+0x18 `E8` → GetItemParam
  - 參數 `+0x4` byte = **道具類型**（已確認：加入背包依此分流）。觀察到的值：1~8 與 0x24（可堆疊道具，見下方「奈落獄」段；會設 ItemData 旗標 bit17）、
    0x11、0x16、0x17、0x18、0x21、0x22、0x23、0x25、0x26、9~0x1A 範圍。哪個是關鍵道具**未確認**
  - 參數 `+0x0` dword 也會被拿來比對特定值（0x2905、0x1EF1）

### 關鍵道具的處理方式（2026-10-15 決定）
- 使用者說明：任務中才拿得到的鑰匙類道具（通關後消失）不會留在背包，所以無法只靠背包分頁對照類型
- 改用**黑名單**：`BlockItemTypes` 只擋確定是一般掉落的類型，沒列出的類型照常掉落（查不到類型也保留）
- 收集方法：`LogDrops=1` 記錄每個掉落的類型；`dumpinv <位址> <格數>`（寫進 watch.txt）列出背包各格類型
- dumpinv 結果（339 格）：0x24 素材 105 格（全 9999，皮甲片，已確認）、0x01 67 格（3~9999）、0x02 107 格（全 99）、
  0x21 27 格（全 1，含掉落過的 0x1DBF）、**0x03 15 格 = 重要道具分頁（使用者確認數量 15，已確認）**、
  0x04~0x06 各 2 格、0x25 4 格、0x0B/0x0D/0x0E/0x0F/0x12/0x1B/0x1E 各 1 格（身上裝備）
- 掉落 log 實測類型：0x01、0x0A、0x16、0x1B、0x1C、0x21，以及 **0x03 兩筆（0xC96D、0x4199，不在背包現有重要道具中 → 推測是任務限定道具 / 鑰匙）**
- 加入背包程式碼把 9~0x1A 視為一組、0x1B 起另一組 → **推測** 0x09~0x1A 武器、0x1B~0x1F 防具
- 預設 ini（v0.3.0）：`KeepItemTypes=0x03`、`BlockItemTypes=0x01,0x02,0x21,0x24,0x09~0x1F`；其他類型照常掉落
- v0.3.0 起 ini 存檔後約 1 秒自動重新載入（2026-10-15 實測成功）
- v0.3.0 實測：0x03 的 0xC96D、0x4199 敵人重生後再次掉落並保留（確認是固定掉落的任務道具）；
  0x04 = **箭（已確認，使用者截圖）** → 推測 0x04~0x06 為彈藥，已加入黑名單
  - 背包陣列：皮甲片那格在 `nioh2.exe+0x20BA260`，實測有效格約在它的 −971 ~ +817 格範圍（每格 0x88）

### 奈落獄（爬塔）掉落（2026-10-15，v0.4.0）
- 使用者回報奈落獄還會掉東西。log 顯示奈落獄敵人掉落**同樣經過 hook 點**（`0x141096031`），沒有走別的路徑；
  地上剩下的是被規則保留的兩筆：
  - `0xAE24` type 0x03 count=5：**推測**是酒盅（奈落獄提高掉寶率用，截圖中的橘色碗與選單圖示相同）
  - `0xFF4A` type 0x25：拾取名稱為「…碗」→ **推測 0x25 = 茶器**
- 朋友回報飾品也會掉；log 出現 `0xFAE8 type=0x20`（23:56:07，被保留），使用者截圖地上拾取名稱為「印籠」（飾品）→ 0x20 = 飾品可信度高。
  ini 熱重載後（23:56:28）的 0x20（0x7967、0x6C63）都已移除
- 加入背包 `0x140A061CC`：`lea eax,[rcx-9]; cmp al,11h`（0x09~0x1A）與 `sub cl,1Bh; cmp cl,5`（**0x1B~0x20**）兩組，
  通過後設「新道具」旗標 bit7 → **推測** 0x1B~0x1F 五個防具部位 + **0x20 飾品**
- 修正先前推測：`0x140A05B09` 對 1~8 與 0x24 設旗標 bit17、`0x140A061AD` 對同一組走數量累加路徑 → 1~8、0x24 是**可堆疊道具**，不是裝備
- 0x22~0x24 與 1~8 同一組（`0x140A05EB0`）；0x26 有獨立處理（`0x140A05E14`），意義未確認
- v0.4.0：新增 ini `BlockItemIds`（判斷順序 KeepItemIds → BlockItemIds → KeepItemTypes → BlockItemTypes），
  預設 `BlockItemIds=0xAE24`，`BlockItemTypes` 加入 0x20、0x25
- 已安裝的 Mods ini 已換新並熱重載（舊 ini 備份為 `Nioh2AutoPickup.ini.bak`）；DLL 已於使用者關遊戲後換成 v0.4.0（舊 DLL 備份為 `Nioh2AutoPickup.dll.v0.3.0.bak`），酒盅擋除待遊戲內驗證
- v0.4.1（2026-10-16）：使用者問「繪卷」會不會被擋（不確定是哪一筆）。上次 session 最後一筆 `0xC3C2` type 0x03 被保留，**推測**是繪卷，未確認。
  改成熱鍵停用時也記錄掉落（`（停用中，照常掉落）啟用時會保留/移除`），不過濾；用來對照畫面上的道具名稱與 ID。
  已安裝到 Mods（舊 DLL 備份 `Nioh2AutoPickup.dll.v0.4.0.bak`），dist 只留 `Nioh2AutoPickup-v0.4.1.zip`
- 繪卷（2026-10-16）：使用者背包「繪卷」分頁為百鬼夜行繪卷，容量 43/80。加入背包 `0x140A05FAE`：`cmp bl,26h` 後走訪
  `[rsi+0x293F28]` 起的 0x88 格陣列、上限 `cmp rcx,50h`（80 格）→ **推測 0x26 = 百鬼夜行繪卷**（高可信度，未實測掉落 log）。
  原本就不在黑名單；ini 改為 `KeepItemTypes=0x03, 0x26` 明確保留（專案與 Mods 的 ini 都已更新）。
  待驗證：繪卷掉落時 log 應出現 `type=0x26 … 保留`
