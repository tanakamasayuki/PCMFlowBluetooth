# PCMFlowBluetooth 仕様

> English version: [SPEC.md](SPEC.md)

## 1. スコープ

**PCMFlowBluetooth** は、[EspBle](https://github.com/tanakamasayuki/EspBle) が扱う Bluetooth Classic のエンコード済み audio payload と、[PCMFlow](https://github.com/tanakamasayuki/PCMFlow) の `PCMSource` / `PCMSink` 境界を接続する Arduino ライブラリである。

Bluetooth profile もデバイス I/O も再実装しない。担当するのは次の 3 つだけである。

- **コーデック** — SBC のデコード(将来: SBC エンコード、mSBC、CVSD)。
- **パケットキュー** — Bluetooth コールバックコンテキストと利用者タスクを分離する固定長キュー。
- **PCM 境界** — デコード結果を `PCMSource` として、エンコード入力を `PCMSink` として公開する。

初期リリースは **無印 ESP32 の A2DP Sink 受信**を完成対象とする。A2DP Source、HFP Hands-Free / Audio Gateway は同じ設計へ後から追加する。Classic 非搭載 SoC(ESP32-S3 等)で機能を偽装しない。

> **同じファミリの姉妹ライブラリ:** [PCMFlowG711](https://github.com/tanakamasayuki/PCMFlowG711) / [PCMFlowG722](https://github.com/tanakamasayuki/PCMFlowG722) / [PCMFlowOpus](https://github.com/tanakamasayuki/PCMFlowOpus) が VoIP 系コーデックを、[PCMFlowUDP](https://github.com/tanakamasayuki/PCMFlowUDP) がネットワーク搬送を担当する。PCMFlowBluetooth はそれらと同じく `PCMSource` / `PCMSink` でパイプラインへ挿さる。

## 2. 非目標

- **Bluetooth スタック・プロファイルの所有** — 接続、ペアリング、AVDTP ネゴシエーション、SCO 設定はすべて EspBle 側の責務。本ライブラリは `begin()` でスタックを起動しないし、`end()` で停止もしない。
- **デバイス出力** — I2S、DAC、USB Audio、ボード搭載スピーカー/マイク。PCMFlowDevice または利用者スケッチが持つ。
- **汎用 PCM 処理** — リサンプル、ゲイン、ミキシング、フォーマット変換は PCMFlow が持つ。
- **AAC その他のオプション A2DP コーデック** — SBC は A2DP の必須コーデックであり、初期リリースはこれだけを扱う。
- **AVRCP / メタデータ / 音量制御 UI** — EspBle 側の領域。
- **デュアルホスト固有の最適化** — Classic audio が安定してから扱う(§15)。
- **非 Espressif SoC** — Classic Bluetooth を持つ Espressif SoC のみ。

## 3. 依存方向とレイヤ

```text
EspBle A2DP/HFP transport
        ↓ エンコード済み payload / コーデック設定
PCMFlowBluetooth
        ↓ PCMSource / PCMSink (signed 16-bit PCM)
PCMFlow
        ↓ PCMSource / PCMSink
PCMFlowDevice、EspUsbHost、I2S、スピーカー、マイク等
```

| レイヤ | 責務 |
|---|---|
| EspBle | プロファイル、接続、コーデックネゴシエーション、エンコード済み payload、timestamp、frame 数、SCO 品質情報 |
| PCMFlowBluetooth | SBC/mSBC/CVSD の encode/decode、パケット/フレーム分割、キュー、PLC、`PCMSource`/`PCMSink` アダプタ |
| PCMFlow | PCM バッファ、フォーマット変換、リサンプル、ゲイン等の汎用 PCM 処理 |
| デバイスライブラリ | I2S、DAC、USB Audio、ボードのスピーカー/マイク |

PCMFlowBluetooth はデバイスドライバを含めず、EspBle や PCMFlow コアにデバイス依存を追加しない。

### 3.1 ライブラリ内部の 2 層構成

本ライブラリ自身も、**EspBle 非依存のコア**と、**EspBle を配線するだけの薄いアダプタ**に分ける。

```text
EspBleA2dpSinkAdapter   ← EspBle 依存。ESP32 のみ。コールバック配線だけ。
        ↓ pushEncoded() / setCodecConfig() / reset()
A2dpSinkStream          ← EspBle 非依存。bounded queue + decoder + PCM ring。PCMSource を実装。
        ↓
SbcDecoder              ← EspBle 非依存。SBC フレーム列 → PCM。
```

この分割は必須である。EspBle はホストプロファイル(`lang-ship:host`)でビルドできないため、コアが EspBle 型に依存していると、姉妹ライブラリと同じ「ホストで全ロジックを単体テストする」体制が成立しない。この構成なら、キュー溢れ、リセット、不正フレーム、コーデック再設定といった振る舞いはすべてホストプロファイルで検証でき、ESP32 実機テストはトランスポート配線の確認に絞れる。

## 4. 公開 API

アンブレラヘッダは `PCMFlowBluetooth.h`。バックエンドのコーデック型(`OI_CODEC_SBC_*` など)は公開ヘッダへ一切露出しない。

### 4.1 `SbcDecoder`

SBC フレーム列を interleaved signed 16-bit PCM へ変換する。EspBle も PCMFlow のパイプラインも知らない。

```cpp
class SbcDecoder {
public:
  bool begin();
  void end();
  void reset();                       // デコーダ状態を初期化(コーデック再設定時)

  // 入力バッファ先頭の 1 フレームをデコードする。
  // consumed には消費した入力バイト数、written には書き込んだ PCM フレーム数が入る。
  // フレーム境界が確定できない/不正な場合は false を返し、consumed に
  // 再同期のためスキップすべきバイト数を入れる。
  bool decodeFrame(const uint8_t *in, size_t inLength, size_t &consumed,
                   int16_t *out, size_t outFrameCapacity, size_t &written);

  const PCMFormat &format() const;    // 最初のフレームをデコードした時点で確定
  bool isReady() const;
};
```

- 出力フォーマットは **SBC ビットストリームのヘッダから決まる**。ネゴシエーション結果(`EspBleClassicA2dpCodecConfig`)は妥当性検証と早期の `format()` 提示に使うが、実際のデコードはフレームヘッダを正とする。
- 1 回の呼び出しで扱うのは 1 フレームだけ。複数フレームを含むパケットの反復は `A2dpSinkStream` が行う。
- ヒープ確保は `begin()` のみ。`decodeFrame()` はヒープを触らない。
- `reset()` は毎回**ビット単位で同一**の初期状態にしなければならない。バックエンド自身の reset は scratch 領域へのポインタを再構築するだけで、そこに置かれた合成フィルタバッファを消さないため、`reset()` 側で領域全体をゼロ初期化する。これを怠ると、再開したストリームが前のストリームのフィルタ履歴を引き継ぎ、さらに最初のストリームは `malloc()` が返した内容を引き継ぐ — 同じ SBC 入力が実行のたびに異なる PCM になる。契約は「`begin()` 直後、suspend 後の `reset()`、再接続後の `reset()` のいずれからでも、同じ入力から同じサンプル列が出ること」である。`tests/sbc_decoder/` が hash で、`tests/peer/a2dp_sbc_receive/` が実機の end-to-end で検証する。

### 4.2 `A2dpSinkStream` — `PCMSource` を実装

本ライブラリの中核。エンコード済みパケットを受け取り、デコードして PCM を出す。

```cpp
enum class PcmOverflowPolicy : uint8_t {
  DropOldest,   // 既定。低遅延を保ち、古い PCM を捨てる
  DropNewest,   // 連続性を優先し、新しい PCM を捨てる
};

class A2dpSinkStream : public PCMSource {
public:
  struct Config {
    size_t encodedQueueBytes = 8192;
    size_t pcmQueueFrames    = 4096;
    PcmOverflowPolicy pcmOverflowPolicy = PcmOverflowPolicy::DropOldest;
  };

  bool begin(const Config &config = Config());
  void end();

  // --- プロデューサ側(Bluetooth コールバックコンテキストから安全に呼べる) ---
  // payload をエンコード済みキューへコピーするだけ。デコードもヒープ確保もしない。
  // キューに payload 全体が入らなければ false を返し、部分コピーは行わない。
  bool pushEncoded(const uint8_t *data, size_t length,
                   uint16_t frameCount, uint32_t timestamp);

  // --- 制御側 ---
  void setCodecConfig(const EncodedAudioFormat &format);  // 変更時は内部で reset()
  void reset();                                            // 全キューとデコーダ状態を破棄

  // --- コンシューマ側 ---
  void update();                       // 単一コンシューマ。デコードして PCM キューへ進める
  size_t availableFrames() const;

  // PCMSource
  const PCMFormat &format() const override;
  size_t readFrames(void *out, size_t frameCount) override;
  bool isEof() const override;         // 常に false(§4.5)
  bool isReady() const override;

  // --- 観測 ---
  uint32_t receivedPacketCount() const;
  uint32_t droppedPacketCount() const;
  uint32_t invalidFrameCount() const;
  uint32_t decodeFailureCount() const;
  uint32_t pcmOverflowFrameCount() const;
  uint32_t timestampDiscontinuityCount() const;
  void resetCounters();

  PCMFlowBluetoothError lastError() const;
  const char *lastErrorName() const;
};
```

`EncodedAudioFormat` は EspBle 非依存の POD で、`EspBleClassicA2dpCodecConfig` から必要な項目だけを写したものである。コアが EspBle 型に依存しないための境界であり、ホストテストでも直接組み立てられる。

```cpp
enum class EncodedAudioCodec : uint8_t { Unknown, Sbc, Msbc, Cvsd };

struct EncodedAudioFormat {
  EncodedAudioCodec codec = EncodedAudioCodec::Unknown;
  uint32_t sampleRate = 0;
  uint8_t  channels   = 0;
  uint8_t  minimumBitpool = 0;
  uint8_t  maximumBitpool = 0;
};
```

### 4.3 `EspBleA2dpSinkAdapter` — EspBle 配線

利用者が EspBle のコールバックを個別に配線しなくてよいようにするだけの薄い層。ESP32(Classic 搭載機)でのみコンパイルされる。

```cpp
class EspBleA2dpSinkAdapter : public PCMSource {
public:
  bool begin(EspBleClassicA2dpSink &transport,
             const A2dpSinkStream::Config &config = A2dpSinkStream::Config());
  void end();
  void update();

  A2dpSinkStream &stream();            // カウンタと詳細設定はここから

  // PCMSource — stream() へ委譲
  const PCMFormat &format() const override;
  size_t readFrames(void *out, size_t frameCount) override;
  bool isEof() const override;
  bool isReady() const override;

  bool connected() const;
  bool streaming() const;
};
```

**有効化条件。** このアダプタが宣言されるのは、`ARDUINO_ARCH_ESP32` と `CONFIG_IDF_TARGET_ESP32` が成立し、**かつ** `<EspBleClassic.h>` が存在する場合だけである。EspBle の Classic 対応はまだリリースに入っていないため、素の EspBle をインストールした環境ではアダプタは存在しない。これは Classic 非搭載 SoC 向けにビルドした場合と同じ結果であり、コンパイルは通るのに無音を返すスタブよりも望ましい。どちらの状態かは `PCMFLOWBLUETOOTH_HAS_ESPBLE_ADAPTER` で判別できる。なお判定に使う `CONFIG_IDF_TARGET_ESP32` はライブラリの `.cpp` には `<Arduino.h>` 経由でしか届かないため、ヘッダで明示的にインクルードしている。

- `begin()` は `onMedia` / `onCodecConfigured` / `onStreamStateChanged` / `onDisconnected` を登録する。**Bluetooth スタックやプロファイルの開始は所有しない** — 利用者が先に `EspBleClassic` と `EspBleClassicA2dpSink::begin()` を呼ぶ。
- `end()` は先に `onMedia({})` を呼んでメディアコールバックを解除し、実行中のコールバックの完了を待ってから内部リソースを解放する。EspBle スタックは停止しない。
- `update()` は EspBle の制御イベント配送(`EspBleClassic::update()`)とは独立で、デコードを進める。利用者ループまたは専用タスクから呼ぶ。
- 初期実装は **単一 A2DP 接続**のみ受け付ける。2 本目の接続の payload は捨て、`foreignConnectionPacketCount()` を進める。
- EspBle は制御イベントを再送しないため、`begin()` は `connected()` / `codecConfig()` を直接問い合わせる。接続後にアタッチされたアダプタが、既に発火済みのイベントを待ち続けてはならない。
- ネゴシエートされたメディア MTU が設定した `maximumPacketBytes` を超えることがあり、その場合フルサイズのパケットが到着のたびに捨てられてしまう。`onConnected` はそれを検出してストリームを再構成する(制御パスでの再確保)。

### 4.4 PCMFlow パイプラインへの接続

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

### 4.5 `isEof()` の扱い

`isEof()` は **常に `false`** を返す。A2DP は無限長のライブストリームであり、切断や一時停止はストリームの終端ではない。ここで `true` を返すと `PCMFlow` のパイプラインが停止し、再接続しても再開しなくなる。ストリームの状態は `isReady()` / `connected()` / `streaming()` で判別する。

## 5. EspBle transport 契約

PCMFlowBluetooth は次の EspBle 公開型だけに依存し、ESP-IDF / Bluedroid の型を自身の公開ヘッダへ露出しない。

| EspBle の型 | 用途 |
|---|---|
| `EspBleClassicA2dpSink` | トランスポート本体 |
| `EspBleClassicA2dpConnection` | `mediaMtu`(キュー下限の根拠)、接続 ID |
| `EspBleClassicA2dpCodecConfig` | ネゴシエーション結果 → `EncodedAudioFormat` へ写す |
| `EspBleClassicEncodedAudioView` | メディア payload。`pushEncoded()` へコピー |
| `EspBleClassicA2dpStreamEvent` | Started / Suspended |
| `EspBleClassicA2dpConnectionId` | 接続 ID の型(`uint16_t` を直接書かない) |

契約(EspBle 側 SPEC と一致させること):

- `EspBleClassicEncodedAudioView::data` は **コールバック中だけ有効な read-only view** である。保持する側はコールバック中にコピーする。EspBle バックエンドはコールバック復帰後に元バッファを必ず一度だけ解放する。
- メディアコールバックは Bluedroid のタスクコンテキストから呼ばれる可能性がある。**ブロック、デコード、ヒープ確保、デバイス I/O を禁止する。**
- コーデック/接続/ストリーム等の制御コールバックは `EspBleClassic::update()` から利用者タスクへ配送される。
- メディアコールバック解除後は、実行中のコールバックが完了してから `end()` が戻り、それ以降コールバックは発生しない。`onMedia({})` は実行中のメディアコールバックの終了を待って解除する。`onMedia()` やトランスポートの `end()` をメディアコールバック自身から呼ばない。
- A2DP payload は A2DP メディアヘッダを除いたエンコード済みコーデックフレーム列であり、`frameCount` 個の SBC フレームを含む。
- `timestamp` はトランスポートが変更しない **opaque な 32-bit 値**である。同期や時間換算には使わず、順序・不連続・ラップの検出にだけ使用する。送信元ごとの単位を仮定しない。
- **制御イベントの順序を仮定しない。** コーデック設定通知が接続通知より先に届く場合がある(実測で確認済み)。

### 5.1 実測値(2026-08-11、EspBle 側 `tests/peer/classic_a2dp_media`)

ESP32-D0WD-V3 同士、Arduino-ESP32 3.3.11、独自 ESP-IDF v5.5.5 Bluedroid アーカイブでの測定。

| 項目 | 値 |
|---|---|
| ネゴシエート結果 | SBC 48,000 Hz / 2ch / raw CIE 4 byte / bitpool 2..53 |
| メディア MTU | 995 byte |
| 送受信 | 既知 payload 13 byte を 1 frame として 100 パケット送信、Sink で 100 パケット / 1,300 byte を欠損なく受信 |
| バックプレッシャ | Source のバースト送信で `WouldBlock` を観測、同一パケット再試行で全パケット配送 |
| timestamp | Source 設定値 1000、1128、1256… を Sink で同値のまま観測 |
| イベント順序 | コーデック設定通知が接続通知より先に届くケースを観測 |
| ライフサイクル | Started → メディア → Suspended → Disconnected まで確認 |
| 実SBC E2E | 944 byteの1パケットにSBC 8フレームを格納し、1バースト8パケットからstereo PCM 8,192フレームを復号 |
| 再開再現性 | suspend前、resume後、切断/再接続後の同一バーストがすべてPCM hash `e511d892`で一致 |
| 復号健全性 | 3バースト/24パケットでdrop、不正フレーム、decode failure、PCM overflow、別接続packetがすべて0 |

13 byte / 1 frame はトランスポート境界のプローブ値であり、実運用上限ではない。PCMFlowBluetoothの実機2台fixtureでは、MTU 995 byteに対して実SBC 944 byte/8フレームのpayloadも検証した。**キューは少なくとも `mediaMtu` 1 個分をアトミックに格納できること**を引き続き下限とする。

## 6. スレッドとコールバック

| 役割 | 実行コンテキスト | 制約 |
|---|---|---|
| `pushEncoded()` | Bluetooth ホストコールバック(プロデューサ) | コピーとカウンタ更新だけ。ヒープ確保・ブロック禁止 |
| `update()` | 利用者ループまたは専用タスク(単一コンシューマ) | 同時呼び出し禁止。検出時は `ConcurrentUpdate` で失敗させる |
| `readFrames()` | PCM コンシューマ | `update()` と別タスクで使えるよう SPSC キューで保護 |

- エンコード済みキューは **SPSC**(プロデューサ = BT コールバック、コンシューマ = `update()`)。
- PCM キューは **SPSC**(プロデューサ = `update()`、コンシューマ = `readFrames()`)。PCMFlow の [`PCMRingBuffer`](https://github.com/tanakamasayuki/PCMFlow/blob/main/src/PCMRingBuffer.h) を再利用する。
- カウンタはラップを許す単調増加 `uint32_t`。`resetCounters()` を別途用意する。
- コールバック解除、`end()`、デストラクタは use-after-free を起こさないライフタイムバリアを持つ。
- **利用者コールバックを内部ロック保持中に呼ばない。**
- `ConcurrentUpdate` は防御的な経路であり、単一コンシューマ規約をすでに破った呼び出し側からしか到達できない。競合に負けないと観測できない試験は不安定になるため、拒否そのものを検証する自動テストは持たない。テストでは代わりにガードが必ず解放されることを検証する。

## 7. バッファリングと負荷制御

- エンコード済みキュー / PCM キューはいずれも **固定上限**を持ち、接続後に無制限成長しない。メモリは `begin()` で確保する。
- メディアコールバック内では既存バッファへコピーするだけで、キュー領域の再確保をしない。
- エンコード済みキューは **長さ前置のレコード列**として実装し、**パケット全体が入らなければパケット全体を捨てる**。部分パケットを残さない(`droppedPacketCount()` を進める)。
- PCM 消費が遅い場合、古い PCM を無制限に遅延再生しない。既定は `DropOldest`(低遅延優先)で、捨てたフレーム数を `pcmOverflowFrameCount()` に積む。
- 次の事象でキューとデコーダ状態を全リセットする: ストリーム停止(Suspended)、切断、コーデック再設定。
- `encodedQueueBytes` の下限は `mediaMtu + レコードヘッダ` 1 個分。`begin()` でこれを下回る設定は拒否する。

## 8. PCM 入出力フォーマット

| 項目 | 値 |
|---|---|
| サンプル形式 | signed 16-bit little-endian、interleaved |
| チャンネル | 1(mono)または 2(stereo) |
| サンプルレート | ネゴシエート結果に従う。A2DP SBC では 16 / 32 / 44.1 / 48 kHz |
| 確定タイミング | コーデックネゴシエーション完了後。それ以前は `format().isValid() == false`、`isReady() == false` |

SBC の joint stereo / dual channel はいずれもデコード後 2ch の interleaved PCM になる。リサンプルもゲインも本ライブラリでは行わない — PCMFlow の担当である。

## 9. メモリ・フットプリント目標

| 項目 | 目標 |
|---|---|
| SBC デコーダ状態 | 2 channel / `SBC_CODEC_FAST_FILTER_BUFFERS` で context 136 byte + decoder data 2,912 byte(ホストビルドでの実測。どちらも 32-bit ターゲット) |
| エンコード済みキュー既定 | 8,192 byte(`mediaMtu` 995 byte の約 8 パケット分) |
| PCM キュー既定 | 4,096 frames(48 kHz stereo で約 85 ms / 16 KB) |
| ヒープ確保 | `begin()` のみ。定常状態でのヒープ確保はゼロ |
| デコード時間 | 実測して記録。Bluetooth コールバックタスクでは決して実行しない |

`esp32` テストプロファイル(arduino-esp32 3.3.11、無印 ESP32、2026-08-12)での実測。Arduino core と本ライブラリのヘッダだけを含む `smoke` イメージ(flash 269,296 byte / RAM 22,116 byte)からの差分として示す。

| テストイメージ | flash 差分 | RAM 差分 | 内容 |
|---|---|---|---|
| `sbc_decoder` | +23,116 byte | +1,656 byte | SBC デコーダ、vendor したコーデック、埋め込みテストベクタ |
| `a2dp_sink_stream` | +35,924 byte | +1,792 byte | 上記に加えてキュー、ストリーム、PCMFlow のリングバッファ |

これらはライブラリ本体のフットプリントではなくテストイメージであり、ベクタと assert 用の足場を含む。実コストの上限を示す値として扱う。

既定値は §15 の実測後に確定する。公開 API は変えずに数値だけを更新する。

## 10. リポジトリ構成

```
PCMFlowBluetooth/
├─ README.md / README.ja.md
├─ SPEC.md   / SPEC.ja.md
├─ CHANGELOG.md
├─ LICENSE                            # MIT (このライブラリ本体)
├─ library.properties                 # Arduino IDE
├─ library.json                       # PlatformIO
├─ keywords.txt
├─ src/
│  ├─ PCMFlowBluetooth.h              # アンブレラヘッダ
│  ├─ EncodedAudioFormat.h            # EspBle 非依存の POD 境界
│  ├─ EncodedPacketQueue.h/.cpp       # 長さ前置レコードの SPSC バイトリング
│  ├─ SbcDecoder.h/.cpp               # SBC フレーム → PCM
│  ├─ A2dpSinkStream.h/.cpp           # コア。PCMSource を実装
│  ├─ EspBleA2dpSinkAdapter.h/.cpp    # EspBle 配線(ESP32 のみ)
│  ├─ pcmflowbluetooth_version.h      # tools/bump_version.py が生成
│  ├─ oi_codec_sbc_private.h          # 生成 shim(角括弧インクルード用、§11.4)
│  └─ external/
│     ├─ LICENSE_sbc.md               # 上流ライセンス + NOTICE + クレジット
│     ├─ UPSTREAM.lock                # 固定した上流コミット
│     ├─ sbc_config.h                 # bt_target.h の代替(§11.3)
│     ├─ sbc_port.c                   # OI_FatalError / OI_LogError の実装
│     └─ sbc/                          # ESP-IDF Bluedroid external/sbc の逐語サブセット
├─ examples/
│  ├─ A2dpSinkToPcm/                  # デコード結果の統計を Serial 表示
│  └─ A2dpSinkWithPCMFlow/            # PCMFlow::setInputSource() へ接続
├─ tests/
│  ├─ README.md / README.ja.md
│  ├─ conftest.py / pyproject.toml
│  ├─ smoke/
│  ├─ sbc_decoder/                    # 既知ベクタ、不正フレーム、reset
│  ├─ encoded_queue/                  # 溢れ、部分パケット禁止、ラップ
│  ├─ a2dp_sink_stream/               # コーデック再設定、PCM 溢れ、カウンタ
│  ├─ external_source/                # PCMFlow::setInputSource() 統合
│  └─ peer/
│     └─ a2dp_sbc_receive/            # DUT + peer 実機 2 台
├─ doc/
│  └─ sibling_library_brief.md
├─ tools/
│  ├─ bump_version.py
│  ├─ sync_sbc.py                     # 保守者ツール: src/external/sbc/ の更新
│  ├─ gen_sbc_symbol_renames.py       # 保守者ツール: §11.3 のブロック再生成
│  └─ gen_sbc_vectors.py              # テスト用 SBC ベクタ生成
└─ .github/
   └─ workflows/
      ├─ release.yml
      └─ compile-examples.yml
```

## 11. Vendor している上流 — SBC コーデック

### 11.1 選定

SBC は **ソース形式の Apache-2.0 実装を vendoring する**。これにより、姉妹ライブラリと同じくホストプロファイルでの単体テストが成立し、`architectures=*` でのコンパイルも保てる。

**上流:** ESP-IDF の [`components/bt/host/bluedroid/external/sbc/`](https://github.com/espressif/esp-idf/tree/master/components/bt/host/bluedroid/external/sbc)。中身は AOSP 由来である。

| 部分 | 由来 | ライセンス |
|---|---|---|
| `decoder/` | Open Interface North America (2006) + The Android Open Source Project (2014) | Apache-2.0 |
| `encoder/` | Broadcom Corporation (1999–2012) | Apache-2.0 |
| `plc/` | Espressif Systems (2015–2021) | Apache-2.0 |

Apache-2.0 は MIT と両立する。ライブラリ本体は MIT で配布し、vendor 部分を `src/external/LICENSE_sbc.md` に明示する。

初期リリースで実際にビルドするのは `decoder/` のみである。`encoder/` と `plc/` は将来フェーズ(§15)のために同じスナップショットから取得するが、コンパイル対象には含めない。

**上流に対するパッチが 1 つだけ必要で、`tools/sync_sbc.py` が適用する。** この codec の固定小数点演算は `long` が 32-bit であることを前提にしている。ESP32(ILP32)を含め、想定された全ターゲットでは成り立つが、64-bit ホストでは `OI_INT32` / `OI_UINT32` / `SINT32` が黙って 64-bit になる。**それでもフレームヘッダは正しく解析され、フレーム長も正しく出るため、動いているように見えたままノイズを出力する。** 該当する 3 つの typedef を固定幅型へ変更する。32-bit ターゲットでは no-op であり、これによって初めてホストテストで codec を検証できるようになる。パッチしたファイルには Apache-2.0 §4(b) が要求する notice を付ける(`src/external/LICENSE_sbc.md` 参照)。置換が上流の変更でマッチしなくなった場合、`sync_sbc.py` は黙ってスキップせずエラーで停止する。

### 11.2 却下した候補

| 候補 | 却下理由 |
|---|---|
| Espressif `esp_audio_codec` v2.4.1 | ESP32 向けバイナリ配布 + `LicenseRef-Espressif-Modified-MIT`(Espressif 製品限定)。ホストプロファイルでリンクできず単体テスト体制が崩れる。`architectures=*` も維持できない |
| BlueZ `sbc` | LGPL-2.1-or-later。MIT ライブラリへの vendoring として条件が重い |
| libbt.a の既存シンボル呼び出し | arduino-esp32 の `libbt.a` は `OI_CODEC_SBC_*` を global シンボルとして持つが、ヘッダは配布されず、Bluedroid 内部シンボルへの直接依存になる。§5 の「Bluedroid 型/シンボルへ直接依存しない」に反する |

### 11.3 シンボル衝突への対処(必須)

arduino-esp32 の `libbt.a` は、Bluedroid 自身がリンクしている**このコーデックそのもののコピー**を含んでいる。しかも公開エントリポイントだけでなく、内部ヘルパ(`OI_SBC_ReadHeader`、`crc8_narrow`、`dct2_8`、`shift_buffer` 等)も static ではないため global で出ている。A2DP を使うスケッチは必ず `libbt.a` をリンクするので、vendor したコードを上流の名前のままビルドすると **duplicate symbol でリンクが失敗する**。

**arduino-esp32 3.3.11 / esp32 ターゲットでの実測: vendor ツリーが定義する global シンボルは 80 個、うち 77 個が `libbt.a` にも定義されている。** したがって全シンボルに `pcmflowbt_` プレフィックスを付ける。

プレフィックスは **vendor したファイルを 1 行も改変せずに**適用できる。vendor 側の `.c` / `.h` はすべて `#include "common/bt_target.h"` を先頭付近に持つので、この 1 ファイルを本ライブラリ側の shim に差し替え、そこにリネームマクロを置く。

```c
/* src/external/sbc_config.h — bt_target.h の代替 */
#define SBC_DEC_INCLUDED  TRUE
#define SBC_ENC_INCLUDED  FALSE   /* 初期リリースはデコードのみ */
#define PLC_INCLUDED      FALSE

#ifndef PCMFLOWBT_SBC_NO_RENAME
/* BEGIN generated by tools/gen_sbc_symbol_renames.py */
#define OI_CODEC_SBC_DecodeFrame  pcmflowbt_OI_CODEC_SBC_DecodeFrame
#define OI_SBC_ReadHeader         pcmflowbt_OI_SBC_ReadHeader
/* … 80 エントリ … */
/* END generated by tools/gen_sbc_symbol_renames.py */
#endif
```

このリストは**手書きしない**。`tools/gen_sbc_symbol_renames.py` が `PCMFLOWBT_SBC_NO_RENAME` 付きでツリーをコンパイルし、`nm` の出力を正としてブロックを書き換える。手書きだと、上流スナップショットでヘルパが 1 つ非 static になっただけで、警告なしにリンクが壊れる。`sync_sbc.py` を実行したら必ず再生成する。`--libbt <path>` を渡すと、実際に衝突しているものを一覧できる。

同じヘッダが、Bluedroid の `bt_target.h` が推移的に提供していたものも供給する — vendor 側が自前でインクルードせずに使う標準ヘッダ(`stddef.h`、`string.h`、`stdbool.h` 等)と、エンコーダヘッダが `stack/bt_types.h` から取っている `UINT8` / `UINT16` / `UINT32` の typedef である。エンコーダは struct 宣言を `SBC_ENC_INCLUDED` ガードの外に置いているため、デコード専用ビルドでもこの typedef が必要になる。

`src/external/sbc_port.c` が `OI_FatalError()` / `OI_LogError()` / `OI_InitDebugCodeHandler()` を実装する(Bluedroid の OS インターフェイスに依存しないため)。冒頭で `sbc_config.h` をインクルードしており、呼び出し側がコンパイルされたのと同じリネーム後のシンボル名で定義される。

### 11.4 Arduino のインクルードパス問題

Arduino のライブラリ形式ではインクルードパスに `<library>/src` しか追加されないが、vendor した SBC ツリーは `#include "oi_codec_sbc.h"` のようなディレクトリを跨ぐ裸のインクルードを持つ。

対処は **PCMFlowOpus と同じ方式**を採る — `tools/sync_sbc.py` が、vendor ツリーの各サブディレクトリに対し、そこから参照される他ディレクトリのヘッダへの 1 行 shim ヘッダを生成する。GCC の `#include "X"` は「その `#include` を書いたファイルのあるディレクトリ」を最初に探すため、shim が必ず見つかる。shim は verbatim ファイルと一緒に `src/external/sbc/` 配下で管理し、`--apply` のたびに全消し・再生成する。

1 箇所だけ別扱いが要る。デコーダの一部が **角括弧**インクルード(`#include <oi_codec_sbc_private.h>`)でディレクトリを跨いでおり、角括弧はインクルード元自身のディレクトリを見ないため、隣に置いた shim では捕まえられない。これらは `src/` 直下に shim を置く — Arduino がインクルードパスに入れる唯一のディレクトリだからである。`sync_sbc.py` が生成・マーキングし、参照されなくなれば削除する。現時点では `src/oi_codec_sbc_private.h` の 1 個だけ。

### 11.5 上流追従方針

**L0 — 自動追従なし。** OI SBC デコーダは 2006 年由来で AOSP / ESP-IDF のいずれでも実質的に凍結している。`tools/sync_sbc.py` は保守者ツールであり、CI からは呼ばない。`src/external/UPSTREAM.lock` に ESP-IDF のコミット SHA と取得日を記録する。更新はライセンス、ABI、品質、実機回帰を伴う明示的な作業とする。

## 12. リリースフロー

親 PCMFlow と同一。`tools/bump_version.py` が `library.properties` / `library.json` / `src/pcmflowbluetooth_version.h` / `CHANGELOG.md` の `Unreleased` セクションを同時に動かし、[`.github/workflows/release.yml`](.github/workflows/release.yml) が `workflow_dispatch` で駆動する。

## 13. テスト

姉妹ライブラリと同じ規約:

- pytest-embedded + Arduino CLI バックエンド。
- プロファイル 2 本 — `lang-ship:host:host`(ロジック、CI 高速)と `esp32:esp32:esp32`(実機検証、フットプリント測定)。
- 機能ごとのディレクトリに `<feature>.ino` / `sketch.yaml` / `test_<feature>.py`。
- アサーションは `EXPECT_TRUE` / `EXPECT_EQ` / `EXPECT_NEAR` マクロと `TEST done N/M` の Serial プロトコル。
- EspBle 由来の実機 2 台テストは `tests/peer/` 配下(EspBle の `tests/peer/classic_a2dp_media` と同じ構成)。

| テストディレクトリ | プロファイル | 検証内容 |
|---|---|---|
| `smoke/` | host | ビルドとハーネス配線。バージョン表示、各クラスのインスタンス化 |
| `sbc_decoder/` | host + esp32 | 既知 SBC ベクタのデコード、mono/dual/stereo/joint stereo、16/32/44.1/48 kHz、bitpool 範囲、不正フレームの拒否と再同期、`reset()` |
| `encoded_queue/` | host | パケット単位の atomic 格納、部分パケット禁止、溢れ時の全体 drop、リングのラップ、`mediaMtu` 1 個分の下限 |
| `a2dp_sink_stream/` | host | 複数フレームパケットの反復、コーデック再設定で旧データ破棄、PCM 溢れポリシー、全カウンタ、`isEof()` が常に false |
| `external_source/` | host | `PCMFlow::setInputSource()` → `pump()` → `readFrames()` の統合 |
| `peer/a2dp_sbc_receive/` | esp32 ×2 | 実機 A2DP。接続、コーデック設定、連続 SBC 受信、PCM デコード、suspend/resume、切断、再接続 |

**SBC テストベクタ**は `tools/gen_sbc_vectors.py` が生成する(vendor した Broadcom エンコーダをホストでビルドして SBC フレームを作り、既知 PCM との対応を固定する)。この用途ではエンコーダをホストでのみビルドするので、`libbt.a` との衝突は起きない。

`tests/peer/a2dp_sbc_receive/` の peer 側は、**事前生成したread-only SBCベクタをテストイメージへ埋め込み** `EspBleClassicA2dpSource::send()` から送る。ESP32 上で実時間エンコードするより決定論的で、resumeと再接続を跨いだサンプルhashまで厳密に検証できる。

### 13.1 完了条件

- ホスト単体テストで、既知 SBC ベクタ、複数フレームパケット、部分パケット禁止、不正フレーム、キュー溢れ、リセットを確認する。
- 非 ESP32 ターゲットで `EspBleA2dpSinkAdapter` が明示的に unsupported となり、EspBle 内部型や Bluedroid シンボルへ直接依存しないことをコンパイルテストで確認する。
- 無印 ESP32 で、A2DP Source 機器からの接続、コーデック設定、連続 SBC 受信、PCM デコード、suspend/resume、切断、再接続を確認する。
- コーデック再設定と停止後に旧フレーム/コールバックが残らない。
- 長時間受信でキューが上限内、ヒープが継続低下せず、コールバックタスクをブロックしない。
- スピーカーから音を出すことは本ライブラリ単体の完了条件ではないが、デバイスライブラリを接続した end-to-end 例で実用性を別途確認する。

## 14. バージョニング

SemVer(`major.minor.patch`)を `library.properties` / `library.json` / `src/pcmflowbluetooth_version.h` で維持する。PCMFlow のバージョンとも EspBle のバージョンとも独立。vendor した SBC のスナップショットは `src/external/UPSTREAM.lock` で別途追跡する。

## 15. 将来フェーズ・未確定項目

### 後続フェーズ

1. **A2DP Source** — vendor 済み Broadcom SBC エンコーダを `PCMSink` として有効化し、EspBle の送信キュー/バックプレッシャへ接続する。`EspBleClassicA2dpSource::send()` が `Accepted` を返したときだけ timestamp を進め、`WouldBlock` ではパケットを保持して次の `update()` で同一内容を再試行する。busy loop も暗黙の drop も行わない。`TooLarge` はネゴシエート済みメディア MTU 超過、`InvalidState` は未接続またはストリーム停止を表す。
2. **HFP WBS** — mSBC の encode/decode、bad-frame 時 PLC(vendor 済み `plc/` を有効化)、16 kHz mono PCM。
3. **HFP NBS** — CVSD バックエンドの選定、8 kHz mono PCM。CVSD は vendor した SBC ツリーに含まれないため、別途ライセンス/実装を監査する。
4. **デュアルホスト** — Classic audio を安定させた後に BLE 同時利用を smoke 対象へ加える。

A2DP と HFP を 1 つの巨大クラスへ統合しない。HFP は downlink が `PCMSource`、uplink が `PCMSink` の別クラスとして追加する。

### 15.1 HFP adapter契約（EspBle実機検証反映済み）

EspBle側の公開transportは`EspBleClassicHfpClient`と`EspBleClassicHfpAudioGateway`で確定した。両roleは
同じ`EspBleClassicHfpAudioConnection`、`EspBleClassicHfpEncodedAudioView`、
`EspBleClassicHfpEncodedAudioPacket`を使い、ESP-IDFの制約によりprocess-wideで排他となる。
PCMFlowBluetoothはroleを開始・停止せず、開始済みのどちらか一方へattachする。

- `HfpDownlinkSource`は`begin(EspBleClassicHfpClient&)`または
  `begin(EspBleClassicHfpAudioGateway&)`で受信callbackを所有し、decode済みPCMを`PCMSource`として公開する。
- `HfpUplinkSink`は同じ2種類のoverloadで送信先を選び、`PCMSink`へ書かれたPCMをencodeして
  `send(EspBleClassicHfpEncodedAudioPacket)`へ渡す。Client/AGをtemplateや一つの巨大profile classへ統合しない。
- adapter停止時はEspBleの`onAudio({})` barrierを通してcallbackを解除してからqueue/codecを破棄する。
  adapter利用中にapplicationが同じroleの`onAudio()`を差し替えることはunsupportedとする。
- Bluetooth callbackではraw viewを固定容量queueへcopyするだけとし、decode、PLC、PCM callback、device I/Oを行わない。
  `view.badFrame`もpayloadと同じ順序でqueueへ保存し、bad frameはdecodeせずPLCへ渡す。

WBS/mSBCは16 kHz、mono、signed 16-bit PCM、1 frameあたり120 sample（7.5 ms）、encode済みframeは57 byteとする。
EspBle実機probeでは`preferredFrameSize=57`だった一方、57-byte送信が受信viewでは58 byteまたは60 byteになり、
controller paddingと60-byteの`badFrame=true` packetが観測された。このためdownlinkは`view.length == 57`を要求せず、
H2 sequence/headerとmSBC frame境界を検証して57 byteを取り出す。末尾を無条件に切る実装は禁止する。同期を失った
payload、短いpayload、bad frameは1 frame分のPLCへ変換し、次の妥当なH2 headerで再同期する。uplinkはvalidな
mSBC frameを1個ずつ、audio connectionの`preferredFrameSize`に一致する57 byteで送る。

NBS/CVSDは8 kHz、mono、signed 16-bit PCMとする。CVSDのencode/decode backendとlicenseが確定するまでclassは
`UnsupportedCodec`を返して開始を拒否し、mSBCへ偽装fallbackしない。CVSDのpacket長は固定値を仮定せず、
`preferredFrameSize`と受信view境界を使う。EspBleのAGは`preferredAudioCodec=Cvsd`で標準codec negotiationを
選択でき、ESP32同士の実機probeでは両roleとも`preferredFrameSize=120`、120-byte受信viewだった。SCOを切断し
同一call中に再接続した場合や、SLC切断・再接続後の新しいcallでもcodecとframe sizeを接続eventから取り直す。
120 byteは観測値であり固定定数にしない。

`send()`の`Accepted`はBluedroidへbuffer ownershipが移ったことだけを示し、controller送信完了を意味しない。
`WouldBlock`は現状local allocation failureである。uplinkは`Accepted`時だけPCM/frameを消費し、`WouldBlock`では
同一frameを保持して次の`update()`で再試行する。`InvalidState`はaudio切断としてqueueをflushし、再接続後の
新しいcodec/handle/preferred frame sizeでcodec stateをresetする。送信discardはEspBleのpacket statisticsを
adapter統計へ反映し、暗黙に成功扱いしない。

HFP phaseの実機完了条件は、公開Client/Audio Gateway間でmSBCをnegotiationし、既知PCMのencode/decode、
双方向連続転送、bad-frame PLC、sequence再同期、着信・応答・終了に伴うSCO再接続、停止callback barrier、
送信discard/queue overflow統計を確認することとする。実speaker/microphoneはdevice libraryを接続する別の
end-to-end exampleで確認し、本ライブラリの必須依存にはしない。

### 並行実装を止めない未確定項目

公開 API と責務境界は本仕様で確定しており、`A2dpSinkStream`、`EncodedPacketQueue`、`SbcDecoder`、ホスト単体テストは先行実装してよい。次の項目は実測後に**値または文書だけを更新**し、公開 API へ波及させない。

- 市販 Source または実 SBC エンコーダでの最大 payload 長・最大 SBC フレーム数と、timestamp 不連続の発生条件
- キュー既定値を決めるための、連続再生時のデコード時間、ヒープ、スタック、ジッタ
- arduino-esp32 のコアバージョンごとの §11.3 リネーム表の再検証。手書きではなく生成物なので、`tools/gen_sbc_symbol_renames.py --libbt <該当コアの libbt.a>` を再実行するだけでよい

## 16. ライセンス

PCMFlowBluetooth 自身のコード: **MIT**([LICENSE](LICENSE))。

vendor した上流(`src/external/sbc/`): **Apache-2.0** — Open Interface North America / The Android Open Source Project / Broadcom Corporation / Espressif Systems。逐語のライセンス本文とクレジットは `src/external/LICENSE_sbc.md` に置く。Apache-2.0 は MIT ライブラリからの再配布と両立するが、Apache-2.0 が要求する NOTICE と改変記録は同ファイルに保持する。

Bluetooth® のワードマークとロゴは Bluetooth SIG, Inc. の登録商標であり、本プロジェクトはその使用許諾を受けていない。SBC は Bluetooth SIG が A2DP の必須コーデックとして規定するものである。
