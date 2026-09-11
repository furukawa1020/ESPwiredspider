# ESP32 + ULN2003 + 28BYJ-48 動作確認

ESP32 DevKitC / ESP-WROOM-32 向けの、モータ1台用PlatformIOプロジェクトです。
Arduinoフレームワークを使用し、外部ライブラリは不要です。
ビルド環境は公式Espressif32 6.12.0に固定し、依存パッケージを `.pio/packages` に保存します。
初回ビルド時はツールチェーンとArduinoフレームワークのダウンロードが必要です。
`board = esp32dev` は [PlatformIO公式のボード設定](https://docs.platformio.org/en/stable/boards/espressif32/esp32dev.html)に準拠しています。

## 配線

| ULN2003 | 接続先 |
| --- | --- |
| IN1 | ESP32 GPIO13 |
| IN2 | ESP32 GPIO14 |
| IN3 | ESP32 GPIO16 |
| IN4 | ESP32 GPIO17 |
| モータ用5ピンコネクタ | 28BYJ-48 5V |
| 電源 + | 外部5V |
| GND | 外部5VのGNDとESP32のGNDを共通接続 |

## ビルド・書き込み・操作

PlatformIOのターミナルで、このフォルダを開いて実行します。

```sh
pio run
pio run -t upload
pio device monitor
```

ポートを自動検出できない場合は `pio device list` で確認し、
`pio run -t upload --upload-port COM3`、`pio device monitor --port COM3` のように指定してください。
`COM3` は実際のポート名に置き換えます。モニタ終了は `Ctrl+C` です。

シリアル通信は115200bpsです。起動直後はコイルOFFで、入力するまで動きません。

| 入力 | 動作 |
| --- | --- |
| `f` | 約1回転正転し、コイルOFF |
| `b` | 約1回転逆転し、コイルOFF |
| `r` | 連続正転 |
| `l` | 連続逆転 |
| `s` | 動作を中断し、IN1〜IN4をすべてLOWにする |

大文字も使用できます。改行は無視します。不明な文字でヘルプを表示します。
新しい動作指令は進行中の指令を置き換えます。`f` / `b` の途中でも `s` で停止できます。
停止してコイルをOFFにすると、モータは電気的な保持力を失います。

## 初回確認

1. 配線と共通GNDを確認して書き込み、シリアルモニタを開きます。
2. `f` を送信し、約8.2秒で約1回転して `Done: coils off` が表示されることを確認します。
3. `b` で逆方向、`r` / `l` で連続回転、`s` で停止とコイルOFFを確認します。
4. `f` の途中でも `s` を送り、完了を待たずに停止することを確認します。

振動するだけの場合は、まずIN1〜IN4の接続順、5ピンコネクタ、外部5Vと共通GNDを確認します。
正転・逆転はこのコードで定義した方向です。

## 実装

`src/main.cpp` の `STEPS_PER_REV = 4096` と `STEP_INTERVAL_US = 2000` を使用します。
4096ハーフステップは仮の1回転で、正確な移動量は実機で校正してください。
8相のハーフステップを `micros()` で更新し、最後の相も2000µs保持してからコイルをOFFにします。
時間カウンタの周回に対応し、処理が遅れた場合はステップをまとめて出力しません。

`HalfStepMotor` にピン・相・残りステップ数・タイミングをまとめています。
将来はモータごとにインスタンスを追加し、`loop()` で各 `update()` を呼び出せます。
現在の対象はモータ1台のみで、Wi-Fi・BLE・XYZ制御は実装していません。

## PC上の制御ロジック検証

GCCのC++コンパイラがある場合、PowerShellで次を実行できます。

```powershell
New-Item -ItemType Directory -Force .pio | Out-Null
g++ -std=c++11 -Wall -Wextra -Werror -I tests/host tests/host/motor_test.cpp -o .pio/motor_host_test.exe
if ($LASTEXITCODE -eq 0) { & .\.pio\motor_host_test.exe }
```

GPIO出力と時計を模擬し、正逆の相順序、4096ステップ、最終相の保持、
途中停止、指令の置き換え、タイマー周回を確認します。実機の回転確認は別途必要です。

### この環境での確認結果

- PC上の制御ロジックテスト：成功（GCC 13.2.0）。
- ESP32向けビルド：依存フレームワークの展開中に `No space left on device` で中断。
  空き容量を確保してから `pio run` を再実行してください。
- 実機への書き込み・回転確認：未実施。
