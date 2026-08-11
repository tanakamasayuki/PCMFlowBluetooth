# PCMFlowBluetooth

> English version: [README.md](README.md)

[PCMFlow](https://github.com/tanakamasayuki/PCMFlow) の Bluetooth Classic オーディオアドオン。[EspBle](https://github.com/tanakamasayuki/EspBle) が扱うエンコード済みオーディオ payload を、PCMFlow の `PCMSource` / `PCMSink` 境界へ接続する。

Bluetooth プロファイルもデバイス I/O も再実装しない。担当するのは **コーデック**、**パケットキュー**、**PCM 境界** の 3 つだけである。

初期リリースは、無印 ESP32 上で **A2DP Sink の SBC** を interleaved signed 16-bit PCM へデコードする。A2DP Source と HFP は同じ設計で後続する。

PCMFlowBluetooth 自身のコードは **MIT**。SBC コーデックは **Apache-2.0** の AOSP / ESP-IDF Bluedroid ソースを vendor している([`src/external/LICENSE_sbc.md`](src/external/LICENSE_sbc.md))。

完全な仕様は [SPEC.ja.md](SPEC.ja.md) を参照。

> **状態:** 開発初期。仕様とテストハーネスは整備済み、実装は進行中。

---

## 位置づけ

```text
EspBle A2DP/HFP transport
        ↓ エンコード済み payload / コーデック設定
PCMFlowBluetooth
        ↓ PCMSource / PCMSink (signed 16-bit PCM)
PCMFlow
        ↓ PCMSource / PCMSink
PCMFlowDevice、EspUsbHost、I2S、スピーカー、マイク等
```

| レイヤ | 担当 |
|---|---|
| EspBle | プロファイル、接続、コーデックネゴシエーション、エンコード済み payload |
| **PCMFlowBluetooth** | **SBC デコード、パケットキュー、`PCMSource` / `PCMSink` アダプタ** |
| PCMFlow | PCM バッファ、フォーマット変換、リサンプル、ゲイン |
| デバイスライブラリ | I2S、DAC、USB Audio、ボードのスピーカー/マイク |

## 構成クラス

| クラス | 方向 | インターフェイス | 対象 |
|---|---|---|---|
| `SbcDecoder` | SBC フレーム → PCM | — | 全ターゲット |
| `A2dpSinkStream` | エンコード済みパケット → PCM | `PCMSource` | 全ターゲット |
| `EspBleA2dpSinkAdapter` | EspBle A2DP Sink → PCM | `PCMSource` | ESP32 のみ |

Bluetooth スタックを必要とするのはアダプタだけである。その下は EspBle 非依存なので、キューとデコーダのロジックは実機と同様にホストでもビルド・単体テストできる。

## 使い方

```cpp
#include <EspBleClassic.h>
#include <PCMFlow.h>
#include <PCMFlowBluetooth.h>

EspBleClassic classic;
EspBleA2dpSinkAdapter a2dp;
PCMFlow audio;

void setup() {
  classic.begin();
  classic.a2dpSink().begin();
  a2dp.begin(classic.a2dpSink());
  audio.setInputSource(a2dp);
}

void loop() {
  classic.update();   // EspBle の制御イベント配送
  a2dp.update();      // SBC デコード
  audio.pump();       // PCMFlow のパイプライン
  // audio.readFrames(...) → I2S / DAC / 任意の出力
}
```

PCMFlowBluetooth が Bluetooth スタックを起動・停止することはない。スケッチが EspBle 経由で所有する。

## 依存

- [PCMFlow](https://github.com/tanakamasayuki/PCMFlow) ≥ 0.2.1 — 必須。
- [EspBle](https://github.com/tanakamasayuki/EspBle) — `EspBleA2dpSinkAdapter`(ESP32 のみ)に必要。それ以外の部分は EspBle なしでビルドできる。EspBle は `library.json` を公開していないため、宣言は `library.properties` のみ。PlatformIO ではリポジトリから直接導入する。

> **EspBle の Classic Bluetooth 対応はまだリリースに入っていない。** `EspBleA2dpSinkAdapter` は `<EspBleClassic.h>` がある場合にのみ宣言されるため、素の EspBle をインストールした環境では存在せず、`PCMFLOWBLUETOOTH_HAS_ESPBLE_ADAPTER` は 0 になる。リリースされるまでは [EspBle](https://github.com/tanakamasayuki/EspBle) をこのリポジトリの隣に clone すること(サンプルの `sketch.yaml` はそれを前提にしている)。

## 対象プラットフォーム

**Classic Bluetooth を搭載した** Espressif SoC。リファレンスターゲットは無印 ESP32。ESP32-S3 / -C3 / -C6 / -H2 に Classic Bluetooth は存在せず、本ライブラリはそれを偽装しない — これらのターゲットではアダプタが宣言されないだけである。

移植性のあるコア(`SbcDecoder`、`A2dpSinkStream`)はどこでもビルドできる。これがホストテストプロファイルを成立させている。

## PCMFlow ファミリー

| ライブラリ | 役割 |
|---|---|
| [PCMFlow](https://github.com/tanakamasayuki/PCMFlow) | 親:PCM パイプライン、リングバッファ、WAV/MP3/FLAC |
| [PCMFlowG711](https://github.com/tanakamasayuki/PCMFlowG711) / [PCMFlowG722](https://github.com/tanakamasayuki/PCMFlowG722) / [PCMFlowOpus](https://github.com/tanakamasayuki/PCMFlowOpus) | VoIP コーデック |
| [PCMFlowUDP](https://github.com/tanakamasayuki/PCMFlowUDP) | ネットワーク搬送(RAW / VBAN / RTP) |
| **PCMFlowBluetooth** | **Bluetooth Classic オーディオ(A2DP / HFP)** |
| [PCMFlowDevice](https://github.com/tanakamasayuki/PCMFlowDevice) | デバイス出力(I2S、DAC、ボードスピーカー) |

## ライセンス

PCMFlowBluetooth 自身のコード: **MIT**([LICENSE](LICENSE))。

vendor した SBC コーデック(`src/external/sbc/`): **Apache-2.0** — Open Interface North America / The Android Open Source Project / Broadcom Corporation / Espressif Systems。詳細は [`src/external/LICENSE_sbc.md`](src/external/LICENSE_sbc.md)。

Bluetooth® のワードマークとロゴは Bluetooth SIG, Inc. の登録商標であり、本プロジェクトはその使用許諾を受けていない。

## テスト

[tests/README.ja.md](tests/README.ja.md) を参照。

## レポート

- [docs/A2DP_VALIDATION_REPORT.ja.md](docs/A2DP_VALIDATION_REPORT.ja.md) — 無印ESP32 2台による A2DP Sink 検証。decoder reset 修正、host / 実機の結果、観測した transport 値。
