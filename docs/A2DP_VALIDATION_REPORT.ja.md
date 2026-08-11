# A2DP検証・修正 完了報告

> English: [A2DP_VALIDATION_REPORT.md](A2DP_VALIDATION_REPORT.md)

対象依頼: `EspBle/docs/REQUEST_PCMFLOWBLUETOOTH_A2DP_VALIDATION.ja.md`(2026-08-11)
報告日: 2026-08-12

## 1. 修正内容と追加した回帰条件

### 修正

`SbcDecoder::reset()` が、backend reset を呼ぶ前に **所有している decoder scratch 領域全体** をゼロ初期化するようにした([src/SbcDecoder.cpp](../src/SbcDecoder.cpp))。

```cpp
memset(&context_, 0, sizeof(context_));
memset(decoderData_, 0, decoderDataBytes_);
OI_CODEC_SBC_DecoderReset(...);
```

依頼書の原因推定どおり、`OI_CODEC_SBC_DecoderReset()` は scratch 領域への pointer を再構築するだけで、その中の synthesis filter buffer は初期化しない。context のみを消去していたため、前 stream の filter history が次 stream へ残り、初回は `malloc()` の未初期化内容に依存していた。

### 追加した回帰条件

ホスト試験 [tests/sbc_decoder/](../tests/sbc_decoder/) に、同一 SBC ベクタを復号した PCM sample 列の hash(FNV-1a)、peak、frame 数で以下を比較する条件を追加した。

| 条件名 | 内容 |
|---|---|
| `reset-same-samples` | `reset()` 前後で hash が完全一致する |
| `begin-matches-reset-samples` / `-peak` / `-pcm` | **新規に確保した別インスタンス**を `begin()` した直後の状態が reset 基準と一致する |
| `reconfigure-matches-reset-samples` | channel 数を変更してから戻しても reset 基準と一致する |

**重要な知見**: `reset()` 同士の比較だけではこのバグを検出できない。修正を一時的に外して確認したところ、`reset-same-samples` は通過し(reset 前後がどちらも同じように汚染されるため)、落ちたのは新規確保インスタンスと比較する条件だけだった。

```
FAIL begin-matches-reset-samples expected=1448164885 actual=1130142731
FAIL begin-matches-reset-peak    expected=12499      actual=32768
TEST done 155/157
```

このとき現れる peak 32768 は、依頼書が実機で観測した飽和値と一致する。未初期化 scratch を検出できるのは、`malloc()` 直後の領域を持つ別インスタンスとの比較だけである。この契約は [SPEC §4.1](../SPEC.md) に明文化した。

## 2. host test 結果

`tests/` で `uv run pytest --ignore=peer` を実行。

```
5 passed in 115.56s
```

| suite | 結果 |
|---|---|
| `smoke/` | pass |
| `encoded_queue/` | pass |
| `sbc_decoder/` | pass — `TEST done 157/157`(修正前の回帰条件では 152、依頼対応で 157) |
| `a2dp_sink_stream/` | pass |
| `external_source/` | pass |

## 3. 無印ESP32 2台E2E の結果

[tests/peer/a2dp_sbc_receive/](../tests/peer/a2dp_sbc_receive/) を追加。DUT = `EspBleClassicA2dpSink` + `EspBleA2dpSinkAdapter`、peer = `EspBleClassicA2dpSource`。両者とも `ESPBLE_CLASSIC_ONLY` + `ESPBLE_CLASSIC_CUSTOM_HOST` でビルドし、EspBle と PCMFlowBluetooth の双方を local directory 指定で参照する。

lifecycle: connect → start → decode → suspend → resume → decode → disconnect → reconnect → decode

```
uv run --env-file .env pytest peer/ -q
1 passed in 52.21s
```

| burst | 開始条件 | rate/ch/bits | frames | peak | hash |
|---|---|---|---|---|---|
| 1 | 初回接続 | 48000 / 2 / 16 | 8192 | 12403 | `e511d892` |
| 2 | suspend → resume | 48000 / 2 / 16 | 8192 | 12403 | `e511d892` |
| 3 | 切断 → 再接続 | 48000 / 2 / 16 | 8192 | 12403 | `e511d892` |

依頼書が技術 probe で得た `peak 12403 / hash e511d892` と完全に一致した。契約どおり、開始条件を跨いで一致することを assert している(値そのものは build 依存として扱う)。

callback 順序は固定していない。実測では `SBC_SOURCE_CODEC` が `PCM_A2DP_CONNECTED` より先に出ており、依頼書の指摘どおり codec が先に来る実機挙動をそのまま許容している。

### 使用 revision

| 項目 | revision |
|---|---|
| Arduino-ESP32 | 3.3.11 |
| EspBle | local checkout `29849db`(library.properties は 1.2.0、Classic 対応は未リリース) |
| PCMFlowBluetooth | 本リポジトリ(0.1.0、未リリース) |
| vendored SBC | ESP-IDF `08e0d30a74ad0bfd5a34933142b80f45619ee410` |
| DUT | ESP32-D0WD-V3 `ec:e3:34:70:3a:86` |
| peer | ESP32-D0WD-V3(`0070070e9b0c`) |

## 4. PCM format、MTU、payload境界、drop/decode/overflow の観測値

| 項目 | 観測値 |
|---|---|
| negotiated PCM format | 48000 Hz / 2ch / 16 bit |
| SBC config | blocks=16, subbands=8, bitpool 2-53, stereo |
| media MTU | 995 byte |
| payload 境界 | 944 byte = SBC 8 frame を **1 packet** で送信(分割なし) |
| 1 burst | 8 packet / 7552 byte / 64 SBC frame → 8192 PCM frame |
| 累計 | 3 burst / 24 packet / 192 SBC frame、`would_block=0` |
| packet drop | 0 |
| invalid frame | 0 |
| decode failure | 0 |
| PCM overflow | 0 |
| foreign connection packet | 0 |
| `lastError()` | `None` |

## 5. 残る制限と次phaseへ送る項目

### 残る制限

- `ConcurrentUpdate`(単一コンシューマ規約違反の検出)は防御的経路であり、自動テストで拒否そのものは検証していない。競合に負けないと観測できないため。ガードが必ず解放されることは検証済み([SPEC §6](../SPEC.md))。
- EspBle の Classic 対応が未リリースのため、example と peer test は EspBle を local directory 指定で参照している。公開後に version 指定へ切り替える。
- example は PCM を取り出すところまでで、実際の音は出ない(device I/O は本ライブラリの範囲外)。
- 実機 E2E は既知ベクタの短い burst のみ。連続受信時の挙動は測定していない。
- SBC decode 専用。encoder は host のベクタ生成にのみ使用し、ファームウェアには含めていない。

### 次phaseへ送る項目(依頼書で初期 A2DP release を妨げないとされた項目)

- HFP Client / Audio Gateway 用 mSBC adapter
- HFP CVSD adapter
- PCM から A2DP Source へ送る SBC encoder adapter
- PCMFlowDevice 等を接続した speaker / microphone 実出力(PCMSource / PCMSink 境界の外側で検証)
- 長時間連続受信時の heap、queue、callback latency 測定

いずれも [SPEC §15](../SPEC.md) の後続フェーズと一致している。
