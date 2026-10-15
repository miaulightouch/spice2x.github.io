# Arcade Room

Vue 3 + TypeScript 的 SpiceAPI WebSocket 控制器。靜態網站，直接由瀏覽器連線至機台；不需要中繼後端。

## 功能

- 手機單欄、底部導覽；桌面雙欄。方向鍵固定十字排列，48px 操作區域。
- 多機台設定：名稱、WebSocket 位址及選擇性記住 API 密碼。
- SERVICE／TEST 脈衝、自動放開、解除按鍵覆寫、投幣。
- 重啟遊戲、結束遊戲、重啟 Windows 與關機；需 API 密碼，操作前確認，斷線不自動重送。
- 16 碼卡片、瀏覽器卡片清單、讀取機台預設卡片、P1／P2 數字鍵。
- 四組場景、60–250% 獨立縮放、0.1% 微調、位移、等比例、線性濾波、畫面複製及儲存。
- 獨立 WebSocket 畫面擷取；不支援 JPEG 的版本仍可使用校正數值與格線。
- 明確標示的示範模式，完全不發送網路操作。
- PWA manifest 與靜態介面離線快取；機台操作仍需要可互通的網路。

## 使用

1. 在 spice2x 啟用 API，例如 `-api 1337 -apipass YOUR_PASSWORD`。WebSocket 位址為 `ws://機台IP:1338`（API 連接埠加一）。請依實際啟動方式加入參數。
2. 手機／電腦需能連線到機台 IP，Windows 防火牆需允許該 WebSocket 連接埠。
3. 在「連線」輸入位址與 API 密碼，按下連線並允許瀏覽器的本機網路存取。
4. 「畫面」的調整會立即套用；「儲存到機台」才會寫入設定檔，需要機台啟用 API 密碼。重新啟動遊戲仍依 spice2x 設計回到場景 1。

完整畫面校正需要 [Resize API 預發行版](https://github.com/miaulightouch/spice2x.github.io/releases/tag/26-10-16-resize-api.1) 或包含相同 API 的較新版本。這是遊戲影像縮放／位移，不會改變 Windows 視窗大小或輸出解析度。

**調整會即時套用，儲存供下次啟動使用。** 修改場景數值或切換場景時會自動啟用縮放；明確取消「啟用縮放」後可儲存停用狀態。儲存後會讀回核對當前設定；這不代表已驗證設定檔或實際渲染結果。影像變形目前沿用 Direct3D 9 路徑，Direct3D 11 沒有接上這套校正。網站的「機台畫面擷取」是單張快照，校正後需按「更新機台畫面」重新擷取。

新版 Chromium 可讓 HTTPS Pages 在使用者授權後連到 `ws://` 私有 IP、`localhost` 或 `.local` 主機。請優先使用私有 IP。一般自訂 DNS 名稱與其他瀏覽器可能需要有效 TLS 的 `wss://` 入口；spice2x 的原生 WebSocket 本身沒有 TLS。參考 [Chrome 本機網路存取說明](https://github.com/GoogleChrome/modern-web-guidance/blob/main/skills/modern-web-guidance/guides/security/local-network-access.md)。

密碼預設僅留在目前頁面的記憶體中；勾選「記住密碼」後才會寫入 localStorage。卡片與機台設定也儲存在這個瀏覽器，不會上傳 GitHub。RC4 是 SpiceAPI 的相容協定，不等同 TLS；需要加密傳輸時使用 WSS。

若連線在 SERVICE／TEST 脈衝途中中斷，重連後可按「解除按鍵覆寫」。控制器不會自動重送投幣、卡片或其他寫入。

## 開發

需要 Node.js 22.12 或更新版本。

```sh
cd controller
npm ci
npm run dev
npm test
npm run build
```

`npm run build` 執行 Vue／TypeScript 型別檢查並輸出 `dist/`。正式版包含本地打包的 Vue 與所有靜態資產，沒有 CDN 相依性。`base: './'` 支援 GitHub 專案 Pages 路徑與自訂網域。

測試涵蓋真實本機 WebSocket 測試伺服器、RC4 已知向量、共用雙向串流、UTF-8／NUL 框架、會話換鑰、要求序列化、錯誤回應、逾時取消、校正佇列、示範操作與密碼儲存選擇。Windows 遊戲與實際瀏覽器的 LAN 權限仍需現場驗證。

## GitHub Pages：從分支發布

網站原始碼放在 `feature/web-controller` 分支的 `controller/`；編譯產物放在 `gh-pages` 的根目錄。控制器不需要變更 `.github/workflows`。

Settings → Pages：

| 設定 | 值 |
| --- | --- |
| Source | **Deploy from a branch** |
| Branch | **gh-pages** |
| Folder | **/ (root)** |

發布網址：<https://miaulightouch.github.io/spice2x.github.io/>（需先在設定頁啟用）。

後續在已設定 GitHub 寫入權限、`origin` 指向自己 fork 的 checkout 執行：

```sh
cd controller
npm ci
npm test
npm run deploy:pages
```

發布腳本先建置，再建立暫時 worktree，只更新 `gh-pages`；一般 Git push 若遠端前進會拒絕，不會強制覆寫。它不修改主分支或 GitHub Actions。GitHub 自己會為 branch Pages 執行內建的發布工作，無需在 repository 新增 workflow。

## 協定

`src/api/client.ts` 實作 UTF-8 JSON 加 `0x00` 的二進位 WebSocket 訊息。每個連線的要求／回應共用一個持續 RC4 串流，因此只允許一個 RPC 等待回應。連線後呼叫 `control.session_refresh`，用舊串流解碼該回應後才切換會話金鑰。每五分鐘在同一佇列中再次換鑰。

斷線、逾時或格式錯誤會關閉該連線並取消尚未送出的要求；重新連線不會重播操作。畫面擷取使用另一個連線，與操作佇列分開。校正更新合併快速滑桿輸入，儲存或切換場景前等待未完成更新。

## 授權

此目錄隨 repository 的 GPL-3.0 授權條款提供；`public/LICENSE.txt` 為授權全文。Vue 依 MIT 授權使用，保留於 `public/THIRD_PARTY_NOTICES.txt`。
