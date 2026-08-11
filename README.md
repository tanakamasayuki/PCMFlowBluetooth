# PCMFlowBluetooth

> 日本語版: [README.ja.md](README.ja.md)

Bluetooth Classic audio add-on for [PCMFlow](https://github.com/tanakamasayuki/PCMFlow). It connects the encoded audio payloads handled by [EspBle](https://github.com/tanakamasayuki/EspBle) to PCMFlow's `PCMSource` / `PCMSink` boundary.

It reimplements neither Bluetooth profiles nor device I/O. It owns exactly three things: **the codec**, **the packet queue**, and **the PCM boundary**.

The initial release decodes **A2DP Sink SBC** into interleaved signed 16-bit PCM on the plain ESP32. A2DP Source and HFP follow on the same design.

PCMFlowBluetooth's own code is **MIT**. The SBC codec is vendored from the **Apache-2.0** AOSP / ESP-IDF Bluedroid sources — see [`src/external/LICENSE_sbc.md`](src/external/LICENSE_sbc.md).

See [SPEC.md](SPEC.md) for the full specification.

> **Status:** early development. The specification and the test harness are in place; the implementation is in progress.

---

## Where it sits

```text
EspBle A2DP/HFP transport
        ↓ encoded payload / codec configuration
PCMFlowBluetooth
        ↓ PCMSource / PCMSink (signed 16-bit PCM)
PCMFlow
        ↓ PCMSource / PCMSink
PCMFlowDevice, EspUsbHost, I2S, speakers, microphones, …
```

| Layer | Owns |
|---|---|
| EspBle | Profiles, connection, codec negotiation, encoded payload |
| **PCMFlowBluetooth** | **SBC decode, packet queue, `PCMSource` / `PCMSink` adapters** |
| PCMFlow | PCM buffering, format conversion, resampling, gain |
| Device libraries | I2S, DAC, USB Audio, on-board speakers/microphones |

## What's inside

| Class | Direction | Interface | Target |
|---|---|---|---|
| `SbcDecoder` | SBC frame → PCM | — | any |
| `A2dpSinkStream` | encoded packets → PCM | `PCMSource` | any |
| `EspBleA2dpSinkAdapter` | EspBle A2DP Sink → PCM | `PCMSource` | ESP32 only |

Only the adapter needs a Bluetooth stack. Everything below it is EspBle-independent, so the queue and decoder logic build and unit-test on a host as well as on hardware.

## Usage

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

PCMFlowBluetooth never starts or stops the Bluetooth stack — the sketch owns that, through EspBle.

## Dependencies

- [PCMFlow](https://github.com/tanakamasayuki/PCMFlow) ≥ 0.2.1 — required.
- [EspBle](https://github.com/tanakamasayuki/EspBle) — required for `EspBleA2dpSinkAdapter` (ESP32 only). The rest of the library builds without it. EspBle publishes no `library.json`, so it is declared in `library.properties` only; PlatformIO users install it from its repository.

> **EspBle's Classic Bluetooth support is not in a published release yet.** `EspBleA2dpSinkAdapter` is declared only when `<EspBleClassic.h>` is present, so with a stock EspBle install it does not exist and `PCMFLOWBLUETOOTH_HAS_ESPBLE_ADAPTER` is 0. Until a release ships it, clone [EspBle](https://github.com/tanakamasayuki/EspBle) next to this repository — that is what the examples' `sketch.yaml` expects.

## Target platforms

Espressif SoCs **with Classic Bluetooth** — the plain ESP32 is the reference target. Classic Bluetooth does not exist on the ESP32-S3, -C3, -C6 or -H2, and this library does not pretend otherwise: the adapter is simply not declared there.

The portable core (`SbcDecoder`, `A2dpSinkStream`) builds anywhere, which is what makes the host test profile possible.

## PCMFlow family

| Library | Role |
|---|---|
| [PCMFlow](https://github.com/tanakamasayuki/PCMFlow) | Parent: PCM pipeline, ring buffer, WAV/MP3/FLAC |
| [PCMFlowG711](https://github.com/tanakamasayuki/PCMFlowG711) / [PCMFlowG722](https://github.com/tanakamasayuki/PCMFlowG722) / [PCMFlowOpus](https://github.com/tanakamasayuki/PCMFlowOpus) | VoIP codecs |
| [PCMFlowUDP](https://github.com/tanakamasayuki/PCMFlowUDP) | Network transport (RAW / VBAN / RTP) |
| **PCMFlowBluetooth** | **Bluetooth Classic audio (A2DP / HFP)** |
| [PCMFlowDevice](https://github.com/tanakamasayuki/PCMFlowDevice) | Device output (I2S, DAC, board speakers) |

## License

PCMFlowBluetooth's own code: **MIT** ([LICENSE](LICENSE)).

Vendored SBC codec (`src/external/sbc/`): **Apache-2.0** — Open Interface North America / The Android Open Source Project / Broadcom Corporation / Espressif Systems. See [`src/external/LICENSE_sbc.md`](src/external/LICENSE_sbc.md).

The Bluetooth® word mark and logos are registered trademarks owned by Bluetooth SIG, Inc.; this project is not licensed to use them.

## Tests

See [tests/README.md](tests/README.md).

## Reports

- [docs/A2DP_VALIDATION_REPORT.md](docs/A2DP_VALIDATION_REPORT.md) — A2DP Sink validation on two ESP32 boards: the decoder-reset fix, host and hardware results, observed transport values.
