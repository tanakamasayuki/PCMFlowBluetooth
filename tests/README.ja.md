# テスト

> English version: [README.md](README.md)

PCMFlowBluetooth の自動テストスイート。親 [PCMFlow のテストスイート](https://github.com/tanakamasayuki/PCMFlow/tree/main/tests)と同じ規約に従う。

- [pytest-embedded](https://docs.espressif.com/projects/pytest-embedded/en/latest/) + Arduino CLI バックエンド。
- プロファイル 2 本 — `lang-ship:host`(ロジック検証、CI 高速)と `esp32:esp32:esp32`(実機検証、フットプリント測定)。
- 機能ごとのサブディレクトリに `<feature>.ino` / `sketch.yaml` / `test_<feature>.py`。
- アサーションは `EXPECT_TRUE` / `EXPECT_EQ` / `EXPECT_NEAR` マクロと `TEST done N/M` の Serial プロトコル。
- 実機 2 台テストは `peer/` 配下([EspBle の `tests/peer/`](https://github.com/tanakamasayuki/EspBle/tree/main/tests/peer) と同じ構成)。

## なぜホストプロファイルが大半をカバーするのか

EspBle は `architectures=esp32` で Bluetooth スタックを必要とするため、ホストプロファイルではビルドできない。そこで本ライブラリは、EspBle の配線以外をすべて EspBle 非依存にする層構成を採っている([SPEC §3.1](../SPEC.ja.md))。

```
EspBleA2dpSinkAdapter   EspBle 依存。ESP32 のみ。コールバック配線だけ。
        |
A2dpSinkStream          EspBle 非依存。キュー + デコーダ。PCMSource を実装。
        |
SbcDecoder              EspBle 非依存。SBC フレーム → PCM。
```

キュー溢れ、リセット、不正フレーム、コーデック再設定、`PCMSource` の契約はすべてホストで検証する。ESP32 側のテストは、実機でしか確認できないもの — トランスポート配線、タイミング、フットプリント — に絞る。

## ディレクトリ構成

- `smoke/` — ビルドとハーネス配線の確認。アンブレラヘッダをコンパイルし、バージョンを表示する。*(実装済み)*
- *(予定)* `sbc_decoder/` — 既知 SBC ベクタ、mono/dual/stereo/joint stereo、16/32/44.1/48 kHz、bitpool 範囲、不正フレームの拒否と再同期、`reset()`。
- *(予定)* `encoded_queue/` — パケット単位の atomic 格納、部分パケット禁止、溢れ時の全体 drop、リングのラップ、`mediaMtu` 1 個分の下限。
- *(予定)* `a2dp_sink_stream/` — 複数フレームパケットの反復、コーデック再設定での旧データ破棄、PCM 溢れポリシー、全カウンタ、`isEof()` が常に false。
- *(予定)* `external_source/` — `PCMFlow::setInputSource()` → `pump()` → `readFrames()` の統合。
- *(予定)* `peer/a2dp_sbc_receive/` — 実機 2 台での A2DP。接続、コーデック設定、連続 SBC 受信、PCM デコード、suspend/resume、切断、再接続。

## SBC テストベクタ

`tools/gen_sbc_vectors.py` が、vendor した Broadcom SBC エンコーダを**ホストでビルド**して、既知 PCM と対になる SBC フレームを生成する。エンコーダをホスト専用にビルドすることで、arduino-esp32 の `libbt.a` が既に公開している `OI_CODEC_SBC_*` / `SBC_Encoder` シンボルと衝突しない([SPEC §11.3](../SPEC.ja.md))。

`peer/a2dp_sbc_receive/` の peer 側は、実時間エンコードではなく**事前生成した SBC フレームを PROGMEM に埋め込んで** `EspBleClassicA2dpSource::send()` から送る。決定論的なので、DUT 側の PCM を厳密に検証できる。

## 実機 2 台の構成

`host` / `device` は pytest-embedded-cli における親側と 2 台目の呼び分けであり、A2DP のロールを指すものではない。DUT が A2DP Sink、peer が A2DP Source を担当する。

`.env` のポート名は EspBle / EspBleBluedroid リポジトリと共通にしてあるので、常時配線された同じ ESP32 ペアをどのリポジトリからでも使える。シリアルポートは排他保持されるため、同時実行しても待つだけである。

## 実行

```sh
# host(既定)
uv run --env-file .env pytest

# 実機 ESP32
uv run --env-file .env pytest --profile esp32

# 実機 2 台テスト
uv run --env-file .env pytest peer/ \
  --profile esp32_peer_host \
  --peer-profile device:esp32_peer_device
```

`.env.example` を `.env` にコピーして、環境に合わせてシリアルポートを調整する。
