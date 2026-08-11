# A2DP validation and fix — completion report

> 日本語版: [A2DP_VALIDATION_REPORT.ja.md](A2DP_VALIDATION_REPORT.ja.md)

Request: `EspBle/docs/REQUEST_PCMFLOWBLUETOOTH_A2DP_VALIDATION.ja.md` (2026-08-11)
Reported: 2026-08-12

## 1. The fix, and the regression conditions added with it

### Fix

`SbcDecoder::reset()` now zeroes **the whole decoder scratch area it owns** before calling the backend reset ([src/SbcDecoder.cpp](../src/SbcDecoder.cpp)):

```cpp
memset(&context_, 0, sizeof(context_));
memset(decoderData_, 0, decoderDataBytes_);
OI_CODEC_SBC_DecoderReset(...);
```

The request's diagnosis was correct: `OI_CODEC_SBC_DecoderReset()` rebuilds the pointers into the scratch area but never clears the synthesis filter buffers held there. Clearing only the context left the previous stream's filter history in place — and left the very first stream depending on whatever `malloc()` returned.

### Regression conditions added

[tests/sbc_decoder/](../tests/sbc_decoder/) decodes the same SBC vector and compares the PCM sample stream by hash (FNV-1a), peak and frame count:

| Assertion | What it pins |
|---|---|
| `reset-same-samples` | the hash is identical before and after `reset()` |
| `begin-matches-reset-samples` / `-peak` / `-pcm` | a **freshly allocated separate instance**, right after `begin()`, matches the reset baseline |
| `reconfigure-matches-reset-samples` | changing the channel count and changing it back still matches the baseline |

**The finding that matters**: a reset-versus-reset comparison cannot detect this bug. With the fix temporarily removed, `reset-same-samples` still passes — both sides of that comparison are contaminated the same way — and only the fresh-allocation assertions fail:

```
FAIL begin-matches-reset-samples expected=1448164885 actual=1130142731
FAIL begin-matches-reset-peak    expected=12499      actual=32768
TEST done 155/157
```

The peak of 32768 that surfaces there is the same saturated value the request observed on hardware. Only a comparison against an instance whose scratch area is a fresh `malloc()` can see the uninitialized memory. The contract is now written down in [SPEC §4.1](../SPEC.md).

## 2. Host test results

`uv run pytest --ignore=peer` in `tests/`:

```
5 passed in 115.56s
```

| Suite | Result |
|---|---|
| `smoke/` | pass |
| `encoded_queue/` | pass |
| `sbc_decoder/` | pass — `TEST done 157/157` (152 before this work, 157 after) |
| `a2dp_sink_stream/` | pass |
| `external_source/` | pass |

The same five suites also pass on a real ESP32 (`--profile esp32`, `5 passed in 166.58s`), which is what pins the decoder on a 32-bit target — `sbc_decoder` reports `TEST done 157/157` there too. Footprints measured from those images are recorded in [SPEC §9](../SPEC.md).

## 3. Two-board ESP32 end-to-end results

[tests/peer/a2dp_sbc_receive/](../tests/peer/a2dp_sbc_receive/) was added. DUT = `EspBleClassicA2dpSink` + `EspBleA2dpSinkAdapter`, peer = `EspBleClassicA2dpSource`. Both build with `ESPBLE_CLASSIC_ONLY` + `ESPBLE_CLASSIC_CUSTOM_HOST` and reference EspBle and PCMFlowBluetooth as local directories.

Lifecycle: connect → start → decode → suspend → resume → decode → disconnect → reconnect → decode

```
uv run --env-file .env pytest peer/ -q
1 passed in 52.21s
```

| Burst | Starting condition | rate/ch/bits | frames | peak | hash |
|---|---|---|---|---|---|
| 1 | first connection | 48000 / 2 / 16 | 8192 | 12403 | `e511d892` |
| 2 | suspend → resume | 48000 / 2 / 16 | 8192 | 12403 | `e511d892` |
| 3 | disconnect → reconnect | 48000 / 2 / 16 | 8192 | 12403 | `e511d892` |

Identical to the `peak 12403 / hash e511d892` the request's technical probe produced. As agreed, the test asserts agreement *across starting conditions* rather than the literal value, which is build-dependent.

Callback order is not fixed. In practice `SBC_SOURCE_CODEC` arrives before `PCM_A2DP_CONNECTED`, exactly the codec-first behaviour the request warned about, and the fixture accepts it.

### Revisions used

| Component | Revision |
|---|---|
| Arduino-ESP32 | 3.3.11 |
| EspBle | local checkout `29849db` (library.properties 1.2.0; Classic support unreleased) |
| PCMFlowBluetooth | this repository (0.1.0, unreleased) |
| Vendored SBC | ESP-IDF `08e0d30a74ad0bfd5a34933142b80f45619ee410` |
| DUT | ESP32-D0WD-V3 `ec:e3:34:70:3a:86` |
| Peer | ESP32-D0WD-V3 (`0070070e9b0c`) |

## 4. PCM format, MTU, payload boundary, drop/decode/overflow

| Item | Observed |
|---|---|
| Negotiated PCM format | 48000 Hz / 2 ch / 16 bit |
| SBC config | blocks=16, subbands=8, bitpool 2-53, stereo |
| Media MTU | 995 bytes |
| Payload boundary | 944 bytes = 8 SBC frames in **one packet** (never split) |
| Per burst | 8 packets / 7552 bytes / 64 SBC frames → 8192 PCM frames |
| Totals | 3 bursts / 24 packets / 192 SBC frames, `would_block=0` |
| Dropped packets | 0 |
| Invalid frames | 0 |
| Decode failures | 0 |
| PCM overflow | 0 |
| Foreign-connection packets | 0 |
| `lastError()` | `None` |

## 5. Remaining limits, and what goes to the next phase

### Remaining limits

- `ConcurrentUpdate` (detection of a single-consumer-rule violation) is a defensive path; no automated test asserts the refusal itself, because a race that has to be lost to be observed makes for a flaky test. That the guard is always released *is* asserted ([SPEC §6](../SPEC.md)).
- EspBle's Classic support is unreleased, so the examples and the peer test reference EspBle by local directory. Switch to a version pin once it ships.
- The examples stop at pulling PCM out; they make no sound (device I/O is outside this library).
- Hardware E2E covers short bursts of a known vector only. Sustained reception is not measured.
- SBC decode only. The encoder is used solely for host-side vector generation and is not compiled into firmware.

### Next phase (the items the request lists as not blocking the initial A2DP release)

- mSBC adapter for HFP Client / Audio Gateway
- HFP CVSD adapter
- SBC encoder adapter feeding PCM to an A2DP Source
- Real speaker / microphone output via PCMFlowDevice and friends, validated outside the PCMSource / PCMSink boundary
- Heap, queue and callback-latency measurement under sustained reception

All of these match the later phases already listed in [SPEC §15](../SPEC.md).
