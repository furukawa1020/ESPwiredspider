# ESP32 L9110S DC motor HTTP / WSS

現在はL9110S経由のDCモーター1台を、`axis: "x"` で実際に動かします。LEDだけの確認モードは終了しています。Wi-Fi/APの設定とAPI認証なしの構成は維持しています。

## 配線

- GPIO18 → L9110SのIA（モジュールのA-IA / IN1）
- GPIO19 → 同じチャンネルのIB（A-IB / IN2）
- モーター → 対応する2つの出力端子
- GND → ESP32・ドライバー・モーター電源で共通
- ドライバー電源 → 使用するモーターとドライバーの定格に合う外部電源

| 指令 | GPIO18 | GPIO19 |
| --- | --- | --- |
| direction: 1 | HIGH | LOW |
| direction: -1 | LOW | HIGH |
| 停止 | LOW | LOW |

PWMは使わず、HIGH/LOWで駆動します。正転・逆転の物理的な向きはモーターの配線で決まります。起動時は停止。反転前は50msの停止を挟みます。以前のTB6612用配線表は、このファームウェアには適用しません。
メーカーの参考資料: https://www.hgsemi.net/upload/pdf/L9110S(%E4%B8%AD%E6%96%87)%20202308.pdf

## HTTP API（認証なし）

ブラウザーで `http://192.168.4.1/` を開くと、正転・逆転・停止と動作時間指定の操作ページを使えます。別のPCやスマホでも、ESP32の同じWi-Fiに接続して開いてください。インターネット接続は不要です。

ネットワーク改訂2では、外部Wi-Fi未設定時はAP専用モード、20MHz幅、192.168.4.1/24とDHCP開始192.168.4.2を明示しています。外部Wi-Fi設定済みの場合はAP+STAです。GET/POST/OPTIONSのCORSに対応しています。statusのuptime_ms、ap_clients、ap_starts/ap_stops、http_requestsで稼働状況を確認できます。SSIDとIPの表示だけで通信成功とは判断せず、実際のHTTP応答で確認してください。

実機検証（2026-09-12）: USBシリアルを閉じた状態で120秒間・99回のHTTP状態取得に成功し、AP停止・再起動なしを確認。Wi-Fiの切断と再接続を2回検証。操作ページとCORSプリフライト成功、別端末での画面表示もユーザー確認済み。正転/逆転各1000ms・500msの出力切り替えと自動停止、3000ms指令の途中停止をAPIで検証しています。回転センサーによる機械的な動作確認や、無期限の稼働保証ではありません。検証ログはローカルの.pio/network-revision2-verification.jsonと.pio/motor-http-demo.jsonに保存しています。

ESP32のWi-Fi `Rail-ESP32-831d16f0` に接続して、`http://192.168.4.1` を使います。APパスワードはNVSに保存され、再起動しても維持します。シリアル115200bpsで `access` と改行を送るとSSIDとパスワードを取得できます。

```http
POST /api/v1/rail/move
Content-Type: application/json

{"axis":"x","direction":1,"duration_ms":1000}
```

- directionは1または-1、duration_msは整数1～60000。
- 現在動かせる軸はxのみ。y/zへのmoveは422を返します。
- 同じ軸の新しい指令は前の動作を置き換えます。
- 応答202 `{"command_id":"http-...","status":"accepted"}` は受付を示します。実際の回転を検出するセンサーはありません。
- `POST /api/v1/rail/stop` に `{"axis":"x"}` または `{"axis":null}` で停止。
- `GET /api/v1/rail/status` でactive、remaining_ms、gpio18、gpio19を取得できます。modeはdc_l9110s、simulatedはfalse。
- 指定時間の停止処理はESP32内で行うため、HTTPクライアントが切断されても指定時間で停止します。

GPIO16の内蔵RGB LEDも連動します。正転は赤点灯、逆転は赤点滅、停止で消灯。ONの電源LEDとは別です。

## 外部中央サーバー

別リポジトリの中央サーバーにも同じHTTPリクエスト形式を実装できます。ESP32は設定後 `wss://<server_host>:443/ws/device` へ実機用Bearerトークンで接続します。HTTP APIの認証とは別です。

ESP32のhello: `{"type":"hello","device_id":"rail-1","simulated":false,"axes":["x"]}`。
サーバーはwelcomeとheartbeatに現在のUNIXミリ秒server_time_msを設定。ESP32は毎秒heartbeatを送り、2秒応答がない場合は中央サーバーから開始した動作を停止します。NTP時刻との差は5秒未満が必要です。

中央サーバーから送る指令:
```json
{"type":"move","command_id":"<一意なID>","axis":"x","direction":1,"duration_ms":1000,"expires_at_ms":0}
```
expires_at_msは送信時に現在時刻+2000ms等へ置換します。command_idは最大63バイト。停止は `{"type":"stop","command_id":"<一意なID>","axis":null}`。イベントはtype=event、command_id、status=started/completed/stopped/failed、messageです。直近32件の重複IDは再実行しません。

## 書き込み・設定

ルートから `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flash-dc.ps1 -Port COM12`。ビルドのみは-BuildOnly。

中央サーバーへの接続設定はconfig.example.jsonをconfig.local.jsonへコピーし、Wi-Fi・ホスト・実機トークンを設定します。
```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" scripts/configure-dc.py --port COM12 --file firmware/rail_dc/config.local.json --ca root-ca.pem
```
TLSのCA証明書検証を行います。設定前でもAPと直接HTTPは動作します。

停止状態確認: `scripts/verify-dc.py --port COM12` をpyserialのあるPythonで実行。
モーターのHTTP実機確認: `scripts/demo-led-http.py --port COM12 --restore-profile <元のWi-Fiプロファイル> --hold-open --motor`。実際に短時間正転・逆転させるため、明示的に--motorを指定します。テスト終了時は停止し、PCを元のWi-Fiへ戻します。
