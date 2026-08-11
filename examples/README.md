# Examples

> 日本語版: [README.ja.md](README.ja.md)

| Example | What it shows |
|---|---|
| [A2dpSinkToPcm](A2dpSinkToPcm/) | Receive A2DP audio, decode SBC to PCM, print the negotiated format and running statistics. |
| [A2dpSinkWithPCMFlow](A2dpSinkWithPCMFlow/) | The same, with the adapter plugged into `PCMFlow::setInputSource()` so PCMFlow handles conversion and resampling. |

Both require an **original ESP32**. Classic Bluetooth does not exist on the ESP32-S3, -C3, -C6 or -H2, and this library does not pretend otherwise — `EspBleA2dpSinkAdapter` is simply not declared there.

## Neither example makes sound

That is deliberate. PCMFlowBluetooth owns the codec, the packet queue and the PCM boundary; it does not own device output. Where the frames go — I2S, the internal DAC, USB Audio, a board speaker — belongs to [PCMFlowDevice](https://github.com/tanakamasayuki/PCMFlowDevice) or to your sketch. Keeping output out of these examples also means they run on any original ESP32 board, whatever is (or is not) wired to it.

To hear something, replace the `readFrames()` loop with a write to your output device. The shape of the loop does not change.

## EspBle version

These examples need EspBle's Classic Bluetooth support (`EspBleClassic.h`), which **is not in a published EspBle release yet**. Each `sketch.yaml` therefore points at a local checkout beside this repository:

```yaml
libraries:
  - dir: ../../
  - dir: ../../../EspBle
  - PCMFlow (0.2.1)
```

Clone [EspBle](https://github.com/tanakamasayuki/EspBle) next to PCMFlowBluetooth, or edit the path. Once a release ships Classic support, these become a normal version pin.

## Running

```sh
cd A2dpSinkToPcm
arduino-cli compile --profile esp32
arduino-cli upload --profile esp32 -p /dev/ttyUSB0
```

Then pair a phone or PC with the board and play something. Serial output at 115200 baud reports the negotiated format once, then statistics every second.
