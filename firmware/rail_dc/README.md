# ESP32 DC XYZ

ESP32は外部の中央サーバーへWi-Fi / WSSで接続します。HTTP APIは別のリポジトリの中央サーバーに実装してください。このファームウェア自身はHTTPサーバーを提供しません。既存のルートのステッピングモーター用プログラム、server/の旧APIとは別のプロジェクトです。

## 中央サーバーのREST契約

```http
POST /api/v1/rail/move
Authorization: Bearer <API_TOKEN>
Content-Type: application/json

{"axis":"x","direction":1,"duration_ms":500}
```

- axis: x / y / z
- direction: 1（プラス） / -1（マイナス）。物理的な向きはモーターの配線で決まります。
- duration_ms: 整数1～60000。実際の出力開始からの時間です。距離・座標の計測には別途エンコーダー等が必要です。
- 同じ軸の新しい指令は前の動作を置き換えます。他の軸は独立して動きます。
- 反転時は通常50msの出力停止を挟みます。初期PWMは128/255です。

中央サーバーは入力を検証し、接続している実機へ次のJSONを送ります。command_idは一意な文字列（最大63バイト）、expires_at_msはサーバー現在時刻+2000ms等のUNIXミリ秒です。下記のプレースホルダーは送信前に置換してください。

```json
{"type":"move","command_id":"<unique-id>","axis":"x","direction":1,"duration_ms":500,"expires_at_ms":0}
```

HTTPの成功応答は実機からstartedイベントを受信した後に返してください。実機未接続は503、開始確認のタイムアウトは504などで扱い、未実行の指令を再接続後に再送しないでください。

停止指令: `{"type":"stop","command_id":"<unique-id>","axis":null}`。axis=nullは全軸、"x"等ならその軸のみです。

## WebSocket契約

ESP32が `wss://<server_host>:443/ws/device` に `Authorization: Bearer <device_token>` 付きで接続します。APIトークンと実機トークンは分け、中央サーバーで認証してください。

1. ESP32 → hello: `{"type":"hello","device_id":"rail-1","simulated":false,"axes":["x","y","z"]}`
2. サーバー → welcome: `{"type":"welcome","server_time_ms":<現在のUNIXミリ秒>}`
3. ESP32は毎秒 `{"type":"heartbeat"}` を送信。サーバーは `{"type":"heartbeat","server_time_ms":<現在のUNIXミリ秒>}` を返信します。
4. ESP32 → イベント: `{"type":"event","command_id":"...","status":"started","message":"..."}`。statusはstarted/completed/stopped/failed。

サーバー時刻とNTP時刻の差が5秒未満の応答で接続を有効化します。切断または2秒間の応答途絶で全軸の出力を停止します。ネットワーク処理とモーターの時間制御は別タスクです。直近32件のcommand_idを記憶し重複実行を防ぎます。置換された旧指令の状態は中央サーバー側でも更新してください。

## 配線（TB6612FNG × 2）

| 軸 | ドライバー | IN1 | IN2 | PWM |
| --- | --- | --- | --- | --- |
| x | 1台目 A | GPIO13 | GPIO14 | GPIO16 |
| y | 1台目 B | GPIO18 | GPIO19 | GPIO21 |
| z | 2台目 A | GPIO25 | GPIO26 | GPIO27 |

両方のSTBYをGPIO23、VCCを3.3V、GNDをESP32とモーター電源の共通GNDに接続。各モーターは対応するAO1/AO2またはBO1/BO2へ接続します。VMはモーターの定格に合う外部電源へ接続します（前提の6Vモーターなら6V）。ESP32の3.3Vからモーターへ給電しないでください。未使用の2台目B入力はGNDへ接続します。ULN2003用の配線とは異なります。

起動時は全出力停止・STBY LOWです。無指令時の停止は保持制御ではありません。負荷で動くZ軸には機械的な保持機構が必要です。移動端の検出は未実装です。

## 書き込みと接続設定

ルートから `powershell -File scripts/flash-dc.ps1 -Port COM11`。ビルドのみは `-BuildOnly`。他リポジトリへ移す場合はこのフォルダーとスクリプトを同じ相対位置に配置するかplatformio.iniのpackages_dirを環境に合わせて変更してください。

config.example.jsonをconfig.local.jsonへコピーし、Wi-Fi、中央サーバーのホスト名、実機トークンを設定。サーバー証明書に対応するルートCAのPEMファイルを用意します。

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" scripts/configure-dc.py --port COM11 --file firmware/rail_dc/config.local.json --ca root-ca.pem
```

設定はESP32のNVSに保存され再起動します。未設定では接続も動作もしません。TLS証明書検証を有効にしており、自己署名の場合も適切なCAが必要です。シリアル115200bpsで `status` と改行を送ると状態JSON、`stop` と改行で停止します。接続設定はシリアルのconfigure JSONでも可能です。

単体テスト: `g++ -std=c++11 -Wall -Wextra -Werror firmware/rail_dc/test/controller_test.cpp -o .pio/dc_controller_test.exe` でコンパイルし実行します。これは制御ロジックのテストであり、実配線・走行や外部サーバーとの結合を確認するものではありません。
