# Nioh2DropFilter（仁王 2 掉落控制 mod）

《仁王 2》PC 版（Steam，The Complete Edition）的原生 x64 DLL mod。

**敵人 / BOSS 死亡時不再掉落一般道具與裝備，只保留「重要道具」（含任務限定的鑰匙類道具）。**

> v0.5.0 起由舊名 Nioh2AutoPickup 改名（早期構想是自動撿取，實際功能是過濾掉落）。從舊版升級請見〈從 Nioh2AutoPickup 升級〉。

## 功能

- hook 遊戲的掉落擲骰函式，在「敵人死亡掉落」這個呼叫點把擲出的道具從結果中移除，所以地上根本不會生成掉落物
- 依道具參數裡的「道具類型」過濾：
  - 類型 `0x03` = 重要道具（背包「重要道具」分頁，**已確認**），預設照常掉落
  - 預設只擋 ini 黑名單列出的類型（消耗品、素材、彈藥、武器、防具等），**沒列出的類型照常掉落**，查不到類型的道具也照常掉落，避免誤擋關鍵道具
- 只過濾「敵人死亡掉落」那一處擲骰呼叫；同一擲骰函式的其他 2 個呼叫者（用途未確認）不受影響。寶箱等其他來源是否走同一流程**未確認**，推測不受影響
- 熱鍵（預設 F6）即時開關；ini 存檔後約 1 秒自動重新載入，不必重開遊戲
- 所有動作寫進 `Nioh2DropFilter.log`

## 安裝

1. 先安裝 [Nioh2ModLoader](https://www.nexusmods.com/nioh2/mods/671)（`dinput8.dll` 放在 `nioh2.exe` 同目錄）
2. 在遊戲目錄的 `Mods\` 底下建立 `Nioh2DropFilter\` 資料夾，放入：
   ```
   <遊戲目錄>\Mods\Nioh2DropFilter\Nioh2DropFilter.dll
   <遊戲目錄>\Mods\Nioh2DropFilter\Nioh2DropFilter.ini
   ```
3. 啟動遊戲。mod loader 約在啟動 5 秒後載入 mod，mod 再等 5 秒開始初始化
4. 打開 `Mods\Nioh2DropFilter\Nioh2DropFilter.log`，看到下面這行就代表正常運作：
   ```
   初始化完成，擋掉落功能就緒（目前開啟）
   ```
   如果看到「擋掉落功能已安全停用」，請見下方〈相容性〉。

移除：刪除 `Mods\Nioh2DropFilter\` 資料夾即可。mod 不會修改存檔或遊戲檔案。

### 從 Nioh2AutoPickup 升級

1. 關閉遊戲
2. 如果想沿用舊設定，把 `Mods\Nioh2AutoPickup\Nioh2AutoPickup.ini` 改名為 `Nioh2DropFilter.ini` 放進新資料夾（v0.4.x 的設定格式相同）
3. **刪除整個 `Mods\Nioh2AutoPickup\` 資料夾**。新舊版同時存在時，新版會在 log 警告並自動不掛 hook，避免重複處理

## 設定（Nioh2DropFilter.ini）

ini 存檔後約 1 秒會自動重新載入（`Enabled`、`ToggleKey`、`[Drops]` 全部即時生效；`[Debug]` 需重開遊戲）。
數值可以寫十進位或 `0x` 開頭的十六進位，清單用逗號分隔。

### [General]

| 設定 | 預設 | 說明 |
|---|---|---|
| `Enabled` | `1` | `1` = 擋掉敵人 / BOSS 掉落；`0` = 不擋（遊戲原樣） |
| `ToggleKey` | `0x75`（F6） | 開關熱鍵，填 [Windows 虛擬鍵碼](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)。只在遊戲視窗在前景時有效，切換結果會寫進 log |

### [Drops]

| 設定 | 預設 | 說明 |
|---|---|---|
| `LogDrops` | `1` | 把每個擲出的掉落寫進 log，例如 `[DROP] 敵人掉落 id=0x11F8 type=0x01 count=1 → 移除`。用來查某個道具的 ID / 類型；按 F6 停用時也會記錄（標示「停用中，照常掉落」），可關掉 mod 打怪，對照畫面上掉出來的東西。不需要可設 `0` |
| `KeepItemIds` | （空） | 一定照常掉落的道具 ID，例如 `KeepItemIds=0x1234, 0x5678`。優先順序最高 |
| `BlockItemIds` | `0xAE24` | 一定擋掉的道具 ID，**優先於 `KeepItemTypes`**，用來擋類型是 `0x03` 但不需要的東西。`0xAE24` = 奈落獄的酒盅（推測） |
| `KeepItemTypes` | `0x03, 0x26` | 一定照常掉落的道具類型。`0x03` = 重要道具、`0x26` = 百鬼夜行繪卷，建議保留 |
| `BlockItemTypes` | 見下表 | 黑名單：只擋這些類型，其他類型照常掉落。**留空 = 除了 Keep 清單外全部擋掉**（可能連任務道具都擋，不建議） |

判斷順序：ID 在 `KeepItemIds` → 保留；ID 在 `BlockItemIds` → 移除；類型在 `KeepItemTypes` → 保留；類型在 `BlockItemTypes` → 移除；其他（含查不到類型）→ 保留。

#### 目前已知的道具類型

道具類型 = 遊戲道具參數 `+0x4` 的 byte。「已確認」是有實測或使用者確認，「推測」是從程式碼或背包分布推斷，可能不準。

| 類型 | 內容 | 狀態 | 預設 |
|---|---|---|---|
| `0x01` | 消耗品 | 推測 | 擋 |
| `0x02` | 數量上限 99 的道具 | 推測 | 擋 |
| `0x03` | 重要道具（含任務限定鑰匙類，例如 `0xC96D`、`0x4199`） | **已確認** | **保留** |
| `0x04` | 箭 | **已確認** | 擋 |
| `0x05`、`0x06` | 彈藥 | 推測 | 擋 |
| `0x09` ~ `0x1A` | 武器 | 推測 | 擋 |
| `0x1B` ~ `0x1F` | 防具 | 推測 | 擋 |
| `0x20` | 飾品（遊戲程式把 `0x1B`~`0x20` 當同一組處理） | 推測 | 擋 |
| `0x21` | 魂石類 | 推測 | 擋 |
| `0x24` | 素材（皮甲片等） | **已確認** | 擋 |
| `0x25` | 茶器（奈落獄掉落的「…碗」） | 推測 | 擋 |
| `0x26` | 百鬼夜行繪卷（遊戲用獨立的 80 格陣列存放，與繪卷分頁上限 80 相同） | 推測 | **保留** |
| 其他（`0x07`、`0x08`、`0x22`、`0x23` 等） | 未知 | — | 照常掉落 |

如果發現某種東西該擋卻還在掉，開 `LogDrops=1` 打倒敵人後看 log 中那筆的 `type=`，加進 `BlockItemTypes` 存檔即可（類型是 `0x03` 的就把 `id=` 加進 `BlockItemIds`）；
反之，若某個必要道具被擋掉，把 log 裡它的 `id=` 加進 `KeepItemIds`。

### [Debug]

| 設定 | 預設 | 說明 |
|---|---|---|
| `EnableWatch` | `0` | 開發 / 逆向用的硬體中斷點監看（讀 `Nioh2DropFilter.watch.txt`）。**只有 Debug 版 DLL 有此功能**，Release 版設 `1` 會被忽略（log 會警告）。一般玩家請保持 `0` |

## 相容性與注意事項

- **只支援目前的 `nioh2.exe`**（Steam The Complete Edition，檔案大小 33,014,240 bytes 的版本）
- mod 不寫死任何位址，而是用特徵碼掃描。遊戲更新後若特徵碼找不到或不唯一，log 會出現
  `特徵碼 [...] 找不到（遊戲可能已更新）` 與 `擋掉落功能已安全停用（不影響遊戲）`，**此時不會安裝任何 hook，遊戲照原樣運作**，等 mod 更新即可
- **測試前請先備份存檔**：`Documents\KoeiTecmo\NIOH2\Savedata\`。mod 不碰存檔，但被擋掉的掉落拿不回來
- 只在單機 / 離線遊玩時使用；未針對連線模式（協力、召喚）做任何處理或測試
- 可與 Nioh2 Enhanced Controller Haptics、ParryYokai、PerfectGuardNative 同時載入（開發時的實測環境）
- 被擋掉的道具不會出現在地上，也不會進入背包；任務需要的道具若剛好不是類型 `0x03`，請用 `KeepItemIds` 加回來

## 從原始碼編譯

需要 Visual Studio 2022 或 Build Tools（「使用 C++ 的桌面開發」工作負載）。MinHook 是 git submodule，clone 時要帶 `--recursive`：

```bash
git clone --recursive https://github.com/mumuriri0801/Nioh2DropFilter.git
msbuild Nioh2DropFilter.sln /p:Configuration=Release /p:Platform=x64
```

輸出在 `bin\Release\Nioh2DropFilter.dll`（同目錄會複製一份 `Nioh2DropFilter.ini`）。Debug 版另外包含逆向用的硬體中斷點監看功能。逆向筆記與特徵碼見 [NOTES.md](NOTES.md)。

## 第三方元件

- [MinHook](https://github.com/TsudaKageyu/minhook)（BSD 2-Clause License）

## Log

- `Nioh2DropFilter.log`：本次啟動的 log（可在遊戲執行中直接開啟）
- `Nioh2DropFilter.previous.log`：上一次啟動的 log

回報問題時請附上這兩個檔案。
