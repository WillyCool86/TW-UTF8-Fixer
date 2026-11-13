# TW UTF-8 Fixer

修正 **Premiere Pro** 與 **After Effects** 中，簡體第三方外掛的參數名稱亂碼，並轉成**繁體中文**顯示。

適用 Windows。改寫自 [AdoreLowen/AE-UTF8-Fixer](https://github.com/AdoreLowen/AE-UTF8-Fixer)，感謝原作者。

---

## 解決什麼問題

在繁體中文 Windows 上，簡體的外掛在效果控制面板裡，參數名稱和下拉選單會變成看不懂的亂碼。

裝了這個外掛之後：

- 亂碼會恢復成正常文字
- 簡體字會自動轉成繁體字
- Premiere / AE 內建效果不受影響

## 下載

到右側的 **[Releases](../../releases/latest)** 下載：

| 檔案 | 給誰用 |
|---|---|
| `!00_PR_UTF8_Fixer.aex` | Premiere Pro |
| `!00_UTF8_Fixer_TC.aex` | After Effects |

兩個軟體都有用，就兩個都下載。**檔名不要改**，開頭的 `!00_` 是為了讓它比其他外掛先載入。

---

## 安裝：Premiere Pro

1. 關閉 Premiere Pro。
2. 打開檔案總管，網址列貼上這行後按 Enter：
   ```
   C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore
   ```
3. 把 `!00_PR_UTF8_Fixer.aex` 拖進去。跳出「需要系統管理員權限」時按「繼續」。
4. 重新開啟 Premiere Pro。

> 如果沒有效果，試試**按住 Shift 再開 Premiere**，讓它重新掃描所有外掛。

## 安裝：After Effects

1. 關閉 After Effects。
2. 打開 AE 的外掛資料夾（年份依你的版本而定）：
   ```
   C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins
   ```
3. **如果裡面已經有原版的 `!00_UTF8_Fixer.aex`，請先刪掉。** 兩個同時存在會互相干擾。
4. 把 `!00_UTF8_Fixer_TC.aex` 拖進去。
5. 重新開啟 After Effects。

> AE 版需要 After Effects 26.2 以上（跟原版相同）。

---

## 移除

刪掉上面放進去的 `.aex` 檔，重新開啟軟體即可。這個外掛不會修改任何其他檔案。

---

## 常見問題

**Q：裝了之後，所有簡體外掛都會變繁體嗎？**
會，只要是「簡體舊編碼」的外掛都會自動處理，不用一個一個設定。

**Q：可以維持簡體嗎？**
- After Effects：目前固定轉繁體。想要簡體請用[原版](https://github.com/AdoreLowen/AE-UTF8-Fixer)。
- Premiere Pro：在繁體中文 Windows 上不行。Premiere 用繁體編碼（Big5）讀取外掛名稱，Big5 裡沒有簡體字，硬放只會變成 `?`。

**Q：有些字變成 `?`？**
極少數簡體字在繁體編碼裡沒有對應的字（只影響 Premiere 版）。

**Q：轉出來的用詞怪怪的？**
簡轉繁是逐字轉換，用詞仍是大陸用語，例如「視頻」不會變成「影片」。

**Q：有些外掛在 AE 裡還是簡體？**
那個外掛本來就不會亂碼（使用萬國碼），這個工具不會經手它。

**Q：Premiere 版沒有作用，要怎麼回報？**
按 `Win + R`，貼上下面這行按 Enter，把記事本內容附在 [Issues](../../issues) 裡：
```
notepad %TEMP%\PR_UTF8_Fixer.log
```

---

## 運作原理

| | Premiere Pro 版 | After Effects 版 |
|---|---|---|
| 外掛類型 | 效果外掛（不會出現在效果面板） | AEGP |
| 攔截方式 | MinHook 攔截第三方外掛的入口與 `add_param` | 同原版 |
| 輸出編碼 | 繁中系統：Big5；其他系統：UTF-8 | UTF-8 |
| 簡轉繁 | 是（僅繁中系統） | 是 |
| 內建效果 | 只在第三方外掛執行期間轉換，內建效果不碰 | 同原版 |

## 自行編譯

需要 Visual Studio 2022（含 C++ 工具）與 CMake。

1. 向 Adobe 下載 [After Effects SDK](https://developer.adobe.com/after-effects/)，把裡面的 `Headers`、`Resources`、`Util` 三個資料夾放到本專案根目錄的 `AE_SDK` 資料夾。
2. 在「Developer PowerShell for VS 2022」執行：
   ```
   cmake -B build-pr -S premiere -DCMAKE_BUILD_TYPE=Release
   cmake --build build-pr --config Release
   cmake -B build-ae -S after-effects -DCMAKE_BUILD_TYPE=Release
   cmake --build build-ae --config Release
   ```

Adobe SDK 受 Adobe 授權條款規範，因此本專案不附帶。

---

## 免責聲明

非 Adobe 官方工具。外掛透過攔截程式內部函式運作，軟體大改版後可能失效，請自行承擔使用風險。

## 授權

[MIT License](LICENSE)。核心攔截與轉碼邏輯來自 AdoreLowen 的原專案。
