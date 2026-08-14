# Bve_MQTT_arduino_opta

[Bve_MQTT_IO](https://github.com/yukinoshitaworks/Bve_MQTT_IO)(BVE Trainsim 用 MQTT 連携プラグイン)が発行する MQTT トピックを購読し、**Arduino Opta** + 拡張モジュール(D1608S / Solid State)の物理リレーへ反映するファームウェアです。BVE の運転状況(ATS-P表示灯・知らせ灯など)を、実物のパイロットランプやリレー出力として再現します。

## 対応関係

BVE(Bve_MQTT_IOプラグイン)→ MQTT → 本スケッチ → Opta のリレーという流れです。

```
BVE (Bve_MQTT_IOプラグイン)  --MQTT-->  Arduino Opta (本スケッチ)  --I2C-->  拡張モジュール D1608S
                                        |
                                        +--> 本体リレー D0/D1/D2
```

## 必要なハードウェア

- BveをプレイするWindows PC([mosquitto](https://mosquitto.org/download/)のインストールによるMQTTブローカーの構築が必要です)。[Node-RED Dashboard](https://github.com/yukinoshitaworks/Bve_Node-RED_Dashboard)の構築も併せてご参照ください。
- Arduino Opta(Lite,RS485いずれも可)
- Opta拡張モジュール D1608S(Solid State リレー、8ch)を1台接続
- Windows PCとOptaをつなぐスイッチングハブorWi-Fiルーター及びLANケーブル

## 必要なライブラリ(Arduino IDE)

Arduino IDE のボードマネージャで **Arduino Mbed OS Opta Boards** を追加した上で、ライブラリマネージャから以下を導入してください。

- `Ethernet`
- `PubSubClient`
- `ArduinoJson`
- `Arduino_Opta_Blueprint`(`OptaBlue.h` / `OptaController` / `DigitalStSolidExpansion` を提供)

## 設定変更(書き込み前に必須)

スケッチ冒頭の以下の定数を、実際の環境に合わせて書き換えてください。

```cpp
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01};  // Opta自身のMACアドレス(重複しないもの)
IPAddress ip(192, 168, 11, 6);                       // Opta自身の固定IP
IPAddress broker(192, 168, 11, 2);                    // MQTTブローカー(BveをプレイするPC)のIP
```

MQTT接続時のクライアントID(`Opta001`)は `reconnect()` 内で固定です。複数台の Opta を同一ブローカーに接続する場合は、台数分ユニークな値に変更してください。

## MQTTトピックとリレー出力の対応

### `bve/panel`(JSON配列)→ 拡張モジュール(D1608S)ch0-5 + ch6(現示ベル)

| panel配列インデックス | 意味 | 拡張モジュール出力ch |
|---|---|---|
| `[2]` | P電源 | ch0 |
| `[3]` | パターン接近 | ch1 |
| `[4]` | ブレーキ動作 | ch2 |
| `[5]` | ブレーキ開放 | ch3 |
| `[6]` | ATS-P | ch4 |
| `[7]` | 故障 | ch5 |

各値は `1` で ON、それ以外で OFF として拡張モジュールのリレーに反映されます。
上記6chのいずれかが変化した瞬間、**ch6 が1秒間だけ ON になるパルス出力**(現示ベル鳴動)を行います。

### `bve/pilot`(単一値)→ Opta本体リレー1(D0)

値が `1` のとき ON、`0` のとき OFF。知らせ灯(全戸閉表示)に対応します。本体LED `LED_D0` も連動して点灯します。

### `bve/sound`(JSON配列)→ Opta本体リレー2(D1)・リレー3(D2)

| sound配列インデックス | 出力先 |
|---|---|
| `[0]` | ATS-Sxベル(D1) |
| `[1]` | ATS-Sxチャイム(D2) |

値が `-1` のとき ON、それ以外(`-10000` など無音相当値)のとき OFF。本体LED `LED_D1`/`LED_D2` も連動します。

### `bve/speed`

現在の実装では `subscribe` のみ行われており、`callback()` 内にハンドラがありません(受信しても何も反映されません)。速度連動の出力を追加したい場合は `callback()` に分岐を追加してください。

### `opta/input`(Publish)

1秒間隔でアナログ入力 `A0` の値を Publish します(現状は未使用。マスコンや車掌スイッチなどの入力をBVE側へ戻す用途を想定)。

## 動作確認

シリアルモニタ(115200bps)を開くと、以下がすべて確認できます。

- 起動時: 取得したIPアドレス、検出された拡張モジュール数とその種類
- MQTT接続試行・成功/失敗ログ
- 受信した各トピックの生データと、JSON配列の全要素
- リレーのON/OFF切り替えログ

`D1608S(Solid State)を認識` と表示されない場合は、拡張モジュールの接続順序・電源・アドレス設定を確認してください。

## 著作権について

本コードの(生成AIを用いた)改造/Pull Request/Isuueは歓迎いたします。ご自身が製作した車両データや路線データに組み込む際も連絡は不要です(配布される場合readme等に記載いただければ嬉しいです)。
ユーザー各自の所有している実物機器への対応や、他の作者さんの車両データに対応した改造なども問題ありませんが、データ作者の皆様にご迷惑が掛からない形で進めていただくようお願いいたします。

## 注意事項

- ネットワーク設定(`mac`/`ip`/`broker`)はハードコードのため、環境が変わるたびにスケッチを書き換えて再アップロードする必要があります
- 拡張モジュールは `OptaController.getExpansion(0)` で1台目固定です。複数拡張モジュールを使う構成には未対応です
- 今後マスコンや各種計器類、車掌スイッチなども対応を検討
