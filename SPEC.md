# PCMFlowBluetooth Specification

> 日本語版: [SPEC.ja.md](SPEC.ja.md)

## 1. Scope

**PCMFlowBluetooth** connects the encoded Bluetooth Classic audio payloads handled by [EspBle](https://github.com/tanakamasayuki/EspBle) to the `PCMSource` / `PCMSink` boundary of [PCMFlow](https://github.com/tanakamasayuki/PCMFlow).

It reimplements neither Bluetooth profiles nor device I/O. It owns exactly three things:

- **Codec** — SBC decode (later: SBC encode, mSBC, CVSD).
- **Packet queue** — a fixed-capacity queue separating the Bluetooth callback context from the user task.
- **PCM boundary** — decoded audio exposed as a `PCMSource`, encoder input accepted as a `PCMSink`.

The initial release targets **A2DP Sink reception on the plain ESP32**. A2DP Source and HFP Hands-Free / Audio Gateway are added later on the same design. Functionality is never faked on SoCs without Classic Bluetooth (ESP32-S3 and friends).

> **Sibling libraries in the same family:** [PCMFlowG711](https://github.com/tanakamasayuki/PCMFlowG711) / [PCMFlowG722](https://github.com/tanakamasayuki/PCMFlowG722) / [PCMFlowOpus](https://github.com/tanakamasayuki/PCMFlowOpus) cover VoIP codecs, [PCMFlowUDP](https://github.com/tanakamasayuki/PCMFlowUDP) covers network transport. Like them, PCMFlowBluetooth plugs into the pipeline through `PCMSource` / `PCMSink`.

## 2. Non-goals

- **Owning the Bluetooth stack or profiles** — connection, pairing, AVDTP negotiation and SCO setup all belong to EspBle. This library neither starts the stack in `begin()` nor stops it in `end()`.
- **Device output** — I2S, DAC, USB Audio, on-board speakers/microphones. Owned by PCMFlowDevice or the user sketch.
- **General PCM processing** — resampling, gain, mixing and format conversion are owned by PCMFlow.
- **AAC and other optional A2DP codecs** — SBC is A2DP's mandatory codec and is the only one the initial release handles.
- **AVRCP / metadata / volume-control UI** — EspBle's territory.
- **Dual-host-specific optimization** — revisited once Classic audio is stable (§15).
- **Non-Espressif SoCs** — only Espressif SoCs with Classic Bluetooth.

## 3. Dependency direction and layering

```text
EspBle A2DP/HFP transport
        ↓ encoded payload / codec configuration
PCMFlowBluetooth
        ↓ PCMSource / PCMSink (signed 16-bit PCM)
PCMFlow
        ↓ PCMSource / PCMSink
PCMFlowDevice, EspUsbHost, I2S, speakers, microphones, …
```

| Layer | Responsibility |
|---|---|
| EspBle | Profiles, connection, codec negotiation, encoded payload, timestamp, frame count, SCO quality information |
| PCMFlowBluetooth | SBC/mSBC/CVSD encode/decode, packet/frame splitting, queueing, PLC, `PCMSource`/`PCMSink` adapters |
| PCMFlow | PCM buffering, format conversion, resampling, gain and other general PCM work |
| Device libraries | I2S, DAC, USB Audio, board speakers/microphones |

PCMFlowBluetooth contains no device driver and adds no device dependency to EspBle or to PCMFlow core.

### 3.1 Two layers inside the library

The library itself is split into an **EspBle-independent core** and a **thin adapter that only wires EspBle up**.

```text
EspBleA2dpSinkAdapter   ← depends on EspBle. ESP32 only. Callback wiring only.
        ↓ pushEncoded() / setCodecConfig() / reset()
A2dpSinkStream          ← EspBle-independent. Bounded queue + decoder + PCM ring. Implements PCMSource.
        ↓
SbcDecoder              ← EspBle-independent. SBC frames → PCM.
```

This split is mandatory. EspBle cannot be built for the host profile (`lang-ship:host`), so a core that depended on EspBle types would make the family's "test all logic on the host" arrangement impossible. With this layering, queue overflow, reset, invalid frames and codec reconfiguration are all verifiable on the host profile, and the ESP32 hardware tests can focus on transport wiring.

## 4. Public API

The umbrella header is `PCMFlowBluetooth.h`. Backend codec types (`OI_CODEC_SBC_*` and friends) are never exposed in public headers.

### 4.1 `SbcDecoder`

Turns a sequence of SBC frames into interleaved signed 16-bit PCM. Knows nothing about EspBle or the PCMFlow pipeline.

```cpp
class SbcDecoder {
public:
  bool begin();
  void end();
  void reset();                       // reinitialize decoder state (on codec reconfiguration)

  // Decode the single frame at the head of the input buffer.
  // `consumed` receives the input bytes consumed, `written` the PCM frames produced.
  // Returns false when the frame boundary cannot be established or the frame is
  // invalid; `consumed` then holds the number of bytes to skip to resynchronize.
  bool decodeFrame(const uint8_t *in, size_t inLength, size_t &consumed,
                   int16_t *out, size_t outFrameCapacity, size_t &written);

  const PCMFormat &format() const;    // settled once the first frame has been decoded
  bool isReady() const;
};
```

- The output format is determined by **the SBC bitstream header**. The negotiated `EspBleClassicA2dpCodecConfig` is used for validation and to report `format()` early, but the frame header is authoritative for decoding.
- One call handles exactly one frame. Iterating over a multi-frame packet is `A2dpSinkStream`'s job.
- Heap allocation happens only in `begin()`. `decodeFrame()` never touches the heap.
- `reset()` must leave the decoder in a **bit-identical** starting state, every time. The backend's own reset rebuilds pointers into the scratch area but does not clear the synthesis filter buffers held there, so `reset()` zeroes the whole area itself. Without that, a resumed stream inherits the previous one's filter history — and the very first stream inherits whatever `malloc()` returned, which is how the same SBC input decodes to different PCM on different runs. The contract is that a fresh `begin()`, a `reset()` after a suspend, and a `reset()` after a reconnect all produce the same samples from the same input; `tests/sbc_decoder/` asserts it by hash, and `tests/peer/a2dp_sbc_receive/` asserts it end-to-end on hardware.

### 4.2 `A2dpSinkStream` — implements `PCMSource`

The core of the library. Takes encoded packets in, produces PCM out.

```cpp
enum class PcmOverflowPolicy : uint8_t {
  DropOldest,   // default: keep latency low, discard the oldest PCM
  DropNewest,   // prefer continuity, discard the newest PCM
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

  // --- Producer side (safe to call from the Bluetooth callback context) ---
  // Copies the payload into the encoded queue. No decoding, no heap allocation.
  // Returns false when the whole payload does not fit; never copies partially.
  bool pushEncoded(const uint8_t *data, size_t length,
                   uint16_t frameCount, uint32_t timestamp);

  // --- Control side ---
  void setCodecConfig(const EncodedAudioFormat &format);  // resets internally when changed
  void reset();                                            // discard all queues and decoder state

  // --- Consumer side ---
  size_t availableFrames() const;
  void update();                       // single consumer: decode and advance the PCM queue

  // PCMSource
  const PCMFormat &format() const override;
  size_t readFrames(void *out, size_t frameCount) override;
  bool isEof() const override;         // always false (§4.5)
  bool isReady() const override;

  // --- Observability ---
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

`EncodedAudioFormat` is an EspBle-independent POD holding only the fields copied out of `EspBleClassicA2dpCodecConfig`. It is the boundary that keeps the core free of EspBle types, and it can be constructed directly in host tests.

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

### 4.3 `EspBleA2dpSinkAdapter` — EspBle wiring

A thin layer whose only purpose is to spare the user from wiring EspBle's callbacks by hand. Compiled only on ESP32 targets with Classic Bluetooth.

```cpp
class EspBleA2dpSinkAdapter : public PCMSource {
public:
  bool begin(EspBleClassicA2dpSink &transport,
             const A2dpSinkStream::Config &config = A2dpSinkStream::Config());
  void end();
  void update();

  A2dpSinkStream &stream();            // counters and detailed settings live here

  // PCMSource — delegated to stream()
  const PCMFormat &format() const override;
  size_t readFrames(void *out, size_t frameCount) override;
  bool isEof() const override;
  bool isReady() const override;

  bool connected() const;
  bool streaming() const;
};
```

**Availability.** The adapter is declared only when `ARDUINO_ARCH_ESP32` and `CONFIG_IDF_TARGET_ESP32` hold *and* `<EspBleClassic.h>` is present. EspBle's Classic support is not in a published release yet, so on a stock EspBle install the adapter simply does not exist — which is the same outcome as building for an SoC without a Classic radio, and is preferable to a stub that compiles and produces silence. `PCMFLOWBLUETOOTH_HAS_ESPBLE_ADAPTER` reports which case applies. The guard tests `CONFIG_IDF_TARGET_ESP32`, which reaches a library `.cpp` only through `<Arduino.h>`; the header includes it for that reason.

- `begin()` registers `onMedia` / `onCodecConfigured` / `onStreamStateChanged` / `onDisconnected`. It **does not own the Bluetooth stack or profile startup** — the user calls `EspBleClassic` and `EspBleClassicA2dpSink::begin()` first.
- `end()` first calls `onMedia({})` to unregister the media callback, waits for any in-flight callback to finish, then releases internal resources. It does not stop the EspBle stack.
- `update()` advances decoding, independently of EspBle's control-event dispatch (`EspBleClassic::update()`). Call it from the user loop or a dedicated task.
- The initial implementation accepts a **single A2DP connection**. Payloads from a second connection are dropped and counted in `foreignConnectionPacketCount()`.
- Control events are not replayed by EspBle, so `begin()` queries `connected()` / `codecConfig()` directly. An adapter attached after the peer connected must not wait for an event that has already fired.
- The negotiated media MTU can exceed the configured `maximumPacketBytes`, in which case every full-size packet would be dropped on arrival. `onConnected` grows the stream to fit instead — a control-path reallocation.

### 4.4 PCMFlow pipeline integration

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
  classic.update();   // EspBle control-event dispatch
  a2dp.update();      // SBC decode
  audio.pump();       // PCMFlow pipeline
  // audio.readFrames(...) → I2S / DAC / any output
}
```

### 4.5 The `isEof()` contract

`isEof()` **always returns `false`**. A2DP is an unbounded live stream; a disconnect or a pause is not the end of the stream. Returning `true` here would stop the `PCMFlow` pipeline and it would not resume on reconnection. Stream state is observed through `isReady()` / `connected()` / `streaming()` instead.

## 5. EspBle transport contract

PCMFlowBluetooth depends only on the following public EspBle types, and exposes no ESP-IDF / Bluedroid type in its own public headers.

| EspBle type | Use |
|---|---|
| `EspBleClassicA2dpSink` | the transport itself |
| `EspBleClassicA2dpConnection` | `mediaMtu` (the basis for the queue's lower bound), connection id |
| `EspBleClassicA2dpCodecConfig` | negotiation result, copied into `EncodedAudioFormat` |
| `EspBleClassicEncodedAudioView` | media payload, copied by `pushEncoded()` |
| `EspBleClassicA2dpStreamEvent` | Started / Suspended |
| `EspBleClassicA2dpConnectionId` | the connection-id type (never spelled as a bare `uint16_t`) |

The contract (to be kept identical in EspBle's own SPEC):

- `EspBleClassicEncodedAudioView::data` is a **read-only view valid only for the duration of the callback**. Anyone retaining it must copy during the callback. The EspBle backend frees the underlying buffer exactly once after the callback returns.
- The media callback may run in a Bluedroid task context. **Blocking, decoding, heap allocation and device I/O are forbidden there.**
- Control callbacks (codec, connection, stream) are dispatched to the user task from `EspBleClassic::update()`.
- After the media callback is unregistered, `end()` returns only once any in-flight callback has completed, and no callback fires afterwards. `onMedia({})` waits for a running media callback to finish before unregistering. Neither `onMedia()` nor the transport's `end()` may be called from the media callback itself.
- The A2DP payload is the encoded codec-frame sequence with the A2DP media header removed, containing `frameCount` SBC frames.
- `timestamp` is an **opaque 32-bit value** the transport does not modify. It is never used for synchronization or time conversion — only for detecting ordering, discontinuity and wrap. No per-source unit is assumed.
- **Control-event ordering is not assumed.** The codec-configured notification can arrive before the connected notification (observed in practice).

### 5.1 Measurements (2026-08-11, EspBle `tests/peer/classic_a2dp_media`)

Two ESP32-D0WD-V3 devices, Arduino-ESP32 3.3.11, custom ESP-IDF v5.5.5 Bluedroid archive.

| Item | Value |
|---|---|
| Negotiated | SBC 48,000 Hz / 2 ch / raw CIE 4 byte / bitpool 2..53 |
| Media MTU | 995 byte |
| Transfer | 100 packets of a known 13-byte payload as 1 frame each; Sink received 100 packets / 1,300 byte with no loss |
| Backpressure | `WouldBlock` observed on Source burst send; retrying the same packet delivered all of them |
| Timestamp | Source values 1000, 1128, 1256… observed unchanged at the Sink |
| Event ordering | codec-configured observed arriving before connected |
| Lifecycle | Started → media → Suspended → Disconnected all confirmed |
| Real SBC E2E | one 944-byte packet carries 8 SBC frames; 8 packets per burst decode to 8,192 stereo PCM frames |
| Restart repeatability | the identical burst produced PCM hash `e511d892` before suspend, after resume, and after disconnect/reconnect |
| Decode health | 3 bursts / 24 packets completed with no packet drop, invalid frame, decode failure, PCM overflow or foreign-connection packet |

13 byte / 1 frame is a transport-boundary probe value, not an operational limit. The PCMFlowBluetooth two-board fixture additionally verifies a real 944-byte, 8-frame SBC payload against the 995-byte MTU. The queue's lower bound remains that it **must hold one `mediaMtu` worth of payload atomically**.

## 6. Threads and callbacks

| Role | Context | Constraints |
|---|---|---|
| `pushEncoded()` | Bluetooth host callback (producer) | copy and counter updates only; no heap allocation, no blocking |
| `update()` | user loop or dedicated task (single consumer) | concurrent calls forbidden; detected calls fail with `ConcurrentUpdate` |
| `readFrames()` | PCM consumer | protected by an SPSC queue so it can run in a different task from `update()` |

- The encoded queue is **SPSC** (producer = BT callback, consumer = `update()`).
- The PCM queue is **SPSC** (producer = `update()`, consumer = `readFrames()`), reusing PCMFlow's [`PCMRingBuffer`](https://github.com/tanakamasayuki/PCMFlow/blob/main/src/PCMRingBuffer.h).
- Counters are monotonically increasing `uint32_t` values that are allowed to wrap; `resetCounters()` is provided separately.
- Callback removal, `end()` and the destructor carry a lifetime barrier that rules out use-after-free.
- **User callbacks are never invoked while an internal lock is held.**
- `ConcurrentUpdate` is a defensive path: it is reachable only by a caller that already violates the single-consumer rule. No automated test asserts the refusal, because a race that has to be lost to be observed makes for a flaky test. The tests assert instead that the guard is always released.

## 7. Buffering and load control

- Both the encoded queue and the PCM queue have a **fixed capacity** and never grow without bound after connection. Memory is allocated in `begin()`.
- Inside the media callback, data is copied into the existing buffer; the queue storage is never reallocated.
- The encoded queue is a **sequence of length-prefixed records**, and **a packet that does not fit entirely is dropped entirely**. No partial packet is ever left behind (`droppedPacketCount()` advances).
- When the PCM consumer is slow, old PCM is not replayed with unbounded latency. The default is `DropOldest` (latency first); discarded frames accumulate in `pcmOverflowFrameCount()`.
- All queues and decoder state are fully reset on: stream stop (Suspended), disconnection, and codec reconfiguration.
- The lower bound for `encodedQueueBytes` is one `mediaMtu` plus the record header. `begin()` rejects a configuration below that.

## 8. PCM I/O format

| Item | Value |
|---|---|
| Sample format | signed 16-bit little-endian, interleaved |
| Channels | 1 (mono) or 2 (stereo) |
| Sample rate | as negotiated; for A2DP SBC one of 16 / 32 / 44.1 / 48 kHz |
| When it settles | after codec negotiation completes; before that `format().isValid() == false` and `isReady() == false` |

SBC joint stereo and dual channel both decode to 2-channel interleaved PCM. Neither resampling nor gain happens here — that is PCMFlow's job.

## 9. Memory & footprint targets

| Item | Target |
|---|---|
| SBC decoder state | 136 byte context + 2,912 byte decoder data for 2 channels with `SBC_CODEC_FAST_FILTER_BUFFERS` (measured on the host build; both are 32-bit targets) |
| Encoded queue default | 8,192 byte (about 8 packets at the measured 995-byte `mediaMtu`) |
| PCM queue default | 4,096 frames (about 85 ms / 16 KB at 48 kHz stereo) |
| Heap allocation | `begin()` only; zero steady-state allocation |
| Decode time | to be measured and recorded; never executed on the Bluetooth callback task |

Measured on the `esp32` test profile (arduino-esp32 3.3.11, plain ESP32, 2026-08-12), as build deltas against the `smoke` image (269,296 byte flash / 22,116 byte RAM), which is the Arduino core plus this library's headers:

| Test image | Flash delta | RAM delta | Contents |
|---|---|---|---|
| `sbc_decoder` | +23,116 byte | +1,656 byte | SBC decoder, the vendored codec, and the embedded test vectors |
| `a2dp_sink_stream` | +35,924 byte | +1,792 byte | the above plus the queue, the stream and PCMFlow's ring buffer |

These are test images, not a library footprint: they carry the vectors and the assertion scaffolding. They bound the real cost from above.

The defaults are finalized after the measurements listed in §15. Only the numbers change — never the public API.

## 10. Repository layout

```
PCMFlowBluetooth/
├─ README.md / README.ja.md
├─ SPEC.md   / SPEC.ja.md
├─ CHANGELOG.md
├─ LICENSE                            # MIT (this library)
├─ library.properties                 # Arduino IDE
├─ library.json                       # PlatformIO
├─ keywords.txt
├─ src/
│  ├─ PCMFlowBluetooth.h              # umbrella header
│  ├─ EncodedAudioFormat.h            # EspBle-independent POD boundary
│  ├─ EncodedPacketQueue.h/.cpp       # SPSC byte ring of length-prefixed records
│  ├─ SbcDecoder.h/.cpp               # SBC frame → PCM
│  ├─ A2dpSinkStream.h/.cpp           # the core; implements PCMSource
│  ├─ EspBleA2dpSinkAdapter.h/.cpp    # EspBle wiring (ESP32 only)
│  ├─ pcmflowbluetooth_version.h      # generated by tools/bump_version.py
│  ├─ oi_codec_sbc_private.h          # generated shim (angle include, §11.4)
│  └─ external/
│     ├─ LICENSE_sbc.md               # upstream license + NOTICE + credits
│     ├─ UPSTREAM.lock                # pinned upstream commit
│     ├─ sbc_config.h                 # replacement for bt_target.h (§11.3)
│     ├─ sbc_port.c                   # OI_FatalError / OI_LogError implementations
│     └─ sbc/                          # verbatim subset of ESP-IDF Bluedroid external/sbc
├─ examples/
│  ├─ A2dpSinkToPcm/                  # print decode statistics over Serial
│  └─ A2dpSinkWithPCMFlow/            # plug into PCMFlow::setInputSource()
├─ tests/
│  ├─ README.md / README.ja.md
│  ├─ conftest.py / pyproject.toml
│  ├─ smoke/
│  ├─ sbc_decoder/                    # known vectors, invalid frames, reset
│  ├─ encoded_queue/                  # overflow, no partial packets, wrap
│  ├─ a2dp_sink_stream/               # codec reconfiguration, PCM overflow, counters
│  ├─ external_source/                # PCMFlow::setInputSource() integration
│  └─ peer/
│     └─ a2dp_sbc_receive/            # DUT + peer, two real devices
├─ doc/
│  └─ sibling_library_brief.md
├─ tools/
│  ├─ bump_version.py
│  ├─ sync_sbc.py                     # maintainer tool: refresh src/external/sbc/
│  ├─ gen_sbc_symbol_renames.py       # maintainer tool: regenerate the §11.3 block
│  └─ gen_sbc_vectors.py              # generate SBC test vectors
└─ .github/
   └─ workflows/
      ├─ release.yml
      └─ compile-examples.yml
```

## 11. Vendored upstream — the SBC codec

### 11.1 Selection

SBC is **vendored as Apache-2.0 source**. That keeps host-profile unit testing viable, exactly as in the sibling libraries, and keeps `architectures=*` compiling.

**Upstream:** ESP-IDF's [`components/bt/host/bluedroid/external/sbc/`](https://github.com/espressif/esp-idf/tree/master/components/bt/host/bluedroid/external/sbc), which is itself AOSP-derived.

| Part | Origin | License |
|---|---|---|
| `decoder/` | Open Interface North America (2006) + The Android Open Source Project (2014) | Apache-2.0 |
| `encoder/` | Broadcom Corporation (1999–2012) | Apache-2.0 |
| `plc/` | Espressif Systems (2015–2021) | Apache-2.0 |

Apache-2.0 is compatible with MIT. The library itself ships under MIT with the vendored portion identified in `src/external/LICENSE_sbc.md`.

The initial release actually builds only `decoder/`. `encoder/` and `plc/` are fetched from the same snapshot for the later phases (§15) but are excluded from compilation.

**One upstream patch is required, applied by `tools/sync_sbc.py`.** The codec's fixed-point arithmetic assumes `long` is 32 bits — true on every target it was written for, including the ESP32 (ILP32), but false on a 64-bit host, where `OI_INT32` / `OI_UINT32` / `SINT32` silently become 64-bit. The frame headers still parse correctly and the frame lengths still come out right, so the decoder looks like it is working while emitting noise. Three typedefs are changed to fixed-width types, which is a no-op on 32-bit targets and is what makes the host test suite able to verify the codec at all. Each patched file carries the notice Apache-2.0 §4(b) requires; see `src/external/LICENSE_sbc.md`. A substitution that stops matching upstream is a hard error in `sync_sbc.py` rather than a silent skip.

### 11.2 Rejected alternatives

| Candidate | Reason for rejection |
|---|---|
| Espressif `esp_audio_codec` v2.4.1 | ESP32 binary distribution under `LicenseRef-Espressif-Modified-MIT` (Espressif products only). It cannot link on the host profile, which breaks the unit-test arrangement, and `architectures=*` cannot be kept |
| BlueZ `sbc` | LGPL-2.1-or-later — too heavy a condition to vendor into an MIT library |
| Calling the existing `libbt.a` symbols | arduino-esp32's `libbt.a` does export `OI_CODEC_SBC_*` globally, but the headers are not distributed and it would be a direct dependency on internal Bluedroid symbols, contradicting §5 |

### 11.3 Symbol collision handling (mandatory)

arduino-esp32's prebuilt `libbt.a` links Bluedroid's own copy of this exact codec. It exports far more than the documented entry points — the internal helpers (`OI_SBC_ReadHeader`, `crc8_narrow`, `dct2_8`, `shift_buffer`, …) are non-static too. Any sketch using A2DP links `libbt.a`, so building the vendored code under the upstream names fails with duplicate symbols.

**Measured against arduino-esp32 3.3.11, esp32 target: the vendored tree defines 80 global symbols, and 77 of them are also defined in `libbt.a`.** Every one of them therefore gets a `pcmflowbt_` prefix.

The prefix is applied **without modifying a single line of the vendored files**. Every vendored `.c` / `.h` includes `"common/bt_target.h"` near the top, so that one file is replaced by a shim of ours that carries the rename macros.

```c
/* src/external/sbc_config.h — replacement for bt_target.h */
#define SBC_DEC_INCLUDED  TRUE
#define SBC_ENC_INCLUDED  FALSE   /* decode only in the initial release */
#define PLC_INCLUDED      FALSE

#ifndef PCMFLOWBT_SBC_NO_RENAME
/* BEGIN generated by tools/gen_sbc_symbol_renames.py */
#define OI_CODEC_SBC_DecodeFrame  pcmflowbt_OI_CODEC_SBC_DecodeFrame
#define OI_SBC_ReadHeader         pcmflowbt_OI_SBC_ReadHeader
/* … 80 entries … */
/* END generated by tools/gen_sbc_symbol_renames.py */
#endif
```

The list is **not hand-maintained**. `tools/gen_sbc_symbol_renames.py` compiles the tree with `PCMFLOWBT_SBC_NO_RENAME`, asks `nm` what came out global, and rewrites the generated block. Hand-maintaining it would not survive an upstream snapshot that makes one more helper non-static: the link would break with no warning. Regenerate after every `sync_sbc.py` run; `--libbt <path>` additionally reports which entries genuinely collide.

The same header supplies the handful of things Bluedroid's `bt_target.h` provided transitively: the standard headers the vendored sources use without including (`stddef.h`, `string.h`, `stdbool.h`, …) and the `UINT8` / `UINT16` / `UINT32` typedefs the encoder headers take from `stack/bt_types.h`. The encoder declares its structs outside the `SBC_ENC_INCLUDED` guard, so those typedefs are needed even in a decode-only build.

`src/external/sbc_port.c` implements `OI_FatalError()` / `OI_LogError()` / `OI_InitDebugCodeHandler()` so nothing depends on Bluedroid's OS interface. It includes `sbc_config.h` first, so its definitions land under the same renamed symbols its callers were compiled against.

### 11.4 The Arduino include-path problem

The Arduino library format only adds `<library>/src` to the include path, but the vendored SBC tree uses cross-directory bare includes such as `#include "oi_codec_sbc.h"`.

The fix is **the same approach PCMFlowOpus uses**: `tools/sync_sbc.py` generates, for every subdirectory of the vendored tree, a one-line shim header for each cross-directory include referenced from that directory. GCC resolves `#include "X"` starting from the directory of the file containing the `#include`, so the shim is always found. Shims live alongside the verbatim files under `src/external/sbc/` and are wiped and regenerated on every `--apply` run.

One case needs different handling. A few decoder sources reach across directories with an **angle** include (`#include <oi_codec_sbc_private.h>`), and an angle include never consults the including file's own directory, so a neighbouring shim cannot catch it. Those get a shim at `src/` instead — the one directory Arduino does place on the include path. `sync_sbc.py` generates it, marks it, and removes it again when it is no longer referenced. Currently there is exactly one: `src/oi_codec_sbc_private.h`.

### 11.5 Upstream-tracking posture

**L0 — no automatic tracking.** The OI SBC decoder dates from 2006 and is effectively frozen in both AOSP and ESP-IDF. `tools/sync_sbc.py` is a maintainer tool and is never invoked from CI. `src/external/UPSTREAM.lock` records the ESP-IDF commit SHA and the sync date. Updates are explicit work accompanied by license, ABI, quality and on-hardware regression checks.

## 12. Release workflow

Identical to parent PCMFlow. `tools/bump_version.py` moves `library.properties`, `library.json`, `src/pcmflowbluetooth_version.h` and the `Unreleased` CHANGELOG section together, driven by [`.github/workflows/release.yml`](.github/workflows/release.yml) via `workflow_dispatch`.

## 13. Testing

Same conventions as the sibling libraries:

- pytest-embedded + Arduino CLI backend.
- Two profiles: `lang-ship:host:host` (logic, fast CI) and `esp32:esp32:esp32` (hardware verification, footprint).
- Per-feature directory with `<feature>.ino`, `sketch.yaml`, `test_<feature>.py`.
- Assertions use the `EXPECT_TRUE` / `EXPECT_EQ` / `EXPECT_NEAR` macros and the `TEST done N/M` Serial protocol.
- Two-device hardware tests live under `tests/peer/`, mirroring EspBle's `tests/peer/classic_a2dp_media`.

| Test dir | Profile | Subject |
|---|---|---|
| `smoke/` | host | build and harness wiring; print version, instantiate each class |
| `sbc_decoder/` | host + esp32 | known SBC vectors; mono/dual/stereo/joint stereo; 16/32/44.1/48 kHz; bitpool range; invalid-frame rejection and resynchronization; `reset()` |
| `encoded_queue/` | host | atomic per-packet storage, no partial packets, whole-packet drop on overflow, ring wrap, the `mediaMtu` lower bound |
| `a2dp_sink_stream/` | host | multi-frame packet iteration, old data discarded on codec reconfiguration, PCM overflow policies, every counter, `isEof()` always false |
| `external_source/` | host | `PCMFlow::setInputSource()` → `pump()` → `readFrames()` integration |
| `peer/a2dp_sbc_receive/` | esp32 ×2 | real A2DP: connect, codec configuration, sustained SBC reception, PCM decode, suspend/resume, disconnect, reconnect |

**SBC test vectors** are produced by `tools/gen_sbc_vectors.py`, which builds the vendored Broadcom encoder on the host to generate SBC frames paired with known PCM. Building the encoder for the host only means no collision with `libbt.a`.

The peer side of `tests/peer/a2dp_sbc_receive/` sends a **pre-generated read-only SBC vector embedded in the test image** through `EspBleClassicA2dpSource::send()`. That is more deterministic than encoding in real time on the ESP32 and lets the DUT-side PCM be checked strictly, including sample hashing across resume and reconnect.

### 13.1 Definition of done

- Host unit tests cover known SBC vectors, multi-frame packets, the no-partial-packet rule, invalid frames, queue overflow and reset.
- A compile test confirms that `EspBleA2dpSinkAdapter` is explicitly unsupported on non-ESP32 targets and that nothing depends directly on EspBle internal types or Bluedroid symbols.
- On a plain ESP32, connection from an A2DP Source device, codec configuration, sustained SBC reception, PCM decode, suspend/resume, disconnect and reconnect all work.
- No stale frames or callbacks survive a codec reconfiguration or a stop.
- Under sustained reception the queues stay within their limits, the heap does not trend downwards, and the callback task is never blocked.
- Producing sound from a speaker is not part of this library's own definition of done, but practicality is confirmed separately in an end-to-end example with a device library attached.

## 14. Versioning

SemVer (`major.minor.patch`) maintained in `library.properties`, `library.json` and `src/pcmflowbluetooth_version.h`. Independent of both the PCMFlow and the EspBle version. The vendored SBC snapshot is tracked separately in `src/external/UPSTREAM.lock`.

## 15. Later phases and open items

### Later phases

1. **A2DP Source** — enable the already-vendored Broadcom SBC encoder as a `PCMSink` and connect it to EspBle's send queue and backpressure. The timestamp advances only when `EspBleClassicA2dpSource::send()` returns `Accepted`; on `WouldBlock` the packet is retained and the identical content is retried from the next `update()`. No busy loop and no implicit drop. `TooLarge` means the negotiated media MTU was exceeded; `InvalidState` means not connected or the stream is stopped.
2. **HFP WBS** — mSBC encode/decode, PLC on bad frames (enabling the vendored `plc/`), 16 kHz mono PCM.
3. **HFP NBS** — select a CVSD backend, 8 kHz mono PCM. CVSD is not part of the vendored SBC tree, so its license and implementation are audited separately.
4. **Dual host** — add concurrent BLE usage to the smoke coverage once Classic audio is stable.

A2DP and HFP are never merged into one large class. HFP is added as separate classes: downlink as a `PCMSource`, uplink as a `PCMSink`.

### 15.1 HFP adapter contract (based on EspBle hardware validation)

EspBle's public transports are now fixed as `EspBleClassicHfpClient` and
`EspBleClassicHfpAudioGateway`. Both roles use the same
`EspBleClassicHfpAudioConnection`, `EspBleClassicHfpEncodedAudioView`, and
`EspBleClassicHfpEncodedAudioPacket` value types, and are process-wide mutually
exclusive because of the ESP-IDF backend constraint. PCMFlowBluetooth attaches
to one already-started role; it never starts or stops that role.

- `HfpDownlinkSource` accepts either role through `begin()` overloads, owns its
  receive callback while running, and exposes decoded PCM as a `PCMSource`.
- `HfpUplinkSink` has the same two overloads, encodes PCM written through its
  `PCMSink`, and passes packets to the selected role's `send()`. Do not merge the
  roles through templates or a large profile-owning class.
- Stop unregisters the callback through EspBle's `onAudio({})` barrier before
  destroying queues or codec state. Replacing that role's callback from the
  application while the adapter runs is unsupported.
- The Bluetooth callback only copies the raw view and `badFrame` marker into a
  fixed-capacity queue. Decode, PLC, PCM callbacks, and device I/O run elsewhere.

WBS/mSBC is 16-kHz mono signed 16-bit PCM, 120 samples (7.5 ms) and 57 encoded
bytes per frame. EspBle hardware negotiated `preferredFrameSize=57`, but a
57-byte send arrived in 58- or 60-byte receive views, and 60-byte bad frames were
also observed. Downlink therefore must not require `view.length == 57` or blindly
truncate the tail. It validates the H2 sequence/header, extracts a 57-byte mSBC
frame, turns invalid/short/bad input into one PLC frame, and resynchronizes at the
next valid H2 header. Uplink sends one valid 57-byte mSBC frame matching the
negotiated preferred frame size.

NBS/CVSD is 8-kHz mono signed 16-bit PCM. Until a CVSD backend and its license are
settled, the classes reject it with `UnsupportedCodec`; they never disguise mSBC
as a fallback. CVSD packet size follows the negotiated preferred frame size and
received view boundaries instead of a hard-coded length. EspBle's AG selects the
standard negotiation with `preferredAudioCodec=Cvsd`; an ESP32-to-ESP32 probe
reported `preferredFrameSize=120` and 120-byte receive views on both roles, also
after reconnecting SCO within the same call and after a full SLC reconnect into
a new call. Adapters reacquire codec and frame size from each connection event.
That size is an observation, not a constant.

`send()` returning `Accepted` means only that Bluedroid took buffer ownership,
not that the controller transmitted it. `WouldBlock` currently means local
allocation failure. Uplink consumes a PCM/frame only on `Accepted`, retains and
retries the identical frame from the next `update()` on `WouldBlock`, and flushes
the queue on `InvalidState`. A new audio connection resets codec state from its
codec, handle, and preferred frame size. Controller discards from EspBle packet
statistics are reflected in adapter statistics rather than treated as success.

The HFP phase definition of done is public Client/Audio Gateway mSBC negotiation,
known-PCM encode/decode, sustained bidirectional transfer, bad-frame PLC,
sequence resynchronization, SCO reconnection across incoming/answer/end, callback
barrier shutdown, and discard/overflow statistics. Real speaker and microphone
I/O remain a separate end-to-end example using a device library, not a required
dependency.

### Open items that do not block parallel implementation

The public API and the responsibility boundaries are settled by this specification. `A2dpSinkStream`, `EncodedPacketQueue`, `SbcDecoder` and the host unit tests may all be implemented now. The following are measured later and update **only values or documentation** — never the public API.

- Maximum payload length, maximum SBC frame count and the conditions producing timestamp discontinuity, against a commercial Source or a real SBC encoder.
- Decode time, heap, stack and jitter under sustained playback, which fix the queue defaults.
- The rename table of §11.3 re-verified per arduino-esp32 core version. It is generated rather than hand-written, so this is a re-run of `tools/gen_sbc_symbol_renames.py --libbt <core's libbt.a>`, not new analysis.

## 16. License

PCMFlowBluetooth's own code: **MIT** ([LICENSE](LICENSE)).

Vendored upstream (`src/external/sbc/`): **Apache-2.0** — Open Interface North America / The Android Open Source Project / Broadcom Corporation / Espressif Systems. The verbatim license text and credits live in `src/external/LICENSE_sbc.md`. Apache-2.0 redistribution from an MIT library is fine, but the NOTICE and the record of modifications that Apache-2.0 requires are kept in that same file.

The Bluetooth® word mark and logos are registered trademarks owned by Bluetooth SIG, Inc.; this project is not licensed to use them. SBC is the mandatory A2DP codec specified by the Bluetooth SIG.
