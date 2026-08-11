# サンプル

> English version: [README.md](README.md)

| サンプル | 内容 |
|---|---|
| [A2dpSinkToPcm](A2dpSinkToPcm/) | A2DP 音声を受信し、SBC を PCM へデコードして、ネゴシエート結果と統計を Serial に表示する。 |
| [A2dpSinkWithPCMFlow](A2dpSinkWithPCMFlow/) | 同じものを `PCMFlow::setInputSource()` へ接続し、変換とリサンプルを PCMFlow に任せる。 |

どちらも **無印 ESP32** が必要。Classic Bluetooth は ESP32-S3 / -C3 / -C6 / -H2 には存在せず、本ライブラリはそれを偽装しない — これらのターゲットでは `EspBleA2dpSinkAdapter` が宣言されないだけである。

## どちらのサンプルも音は出ない

これは意図的である。PCMFlowBluetooth が担当するのはコーデック、パケットキュー、PCM 境界であって、デバイス出力ではない。フレームをどこへ送るか — I2S、内蔵 DAC、USB Audio、ボードのスピーカー — は [PCMFlowDevice](https://github.com/tanakamasayuki/PCMFlowDevice) か利用者スケッチの領分である。出力を含めないことで、何が繋がっているかに関係なくどの無印 ESP32 ボードでも動くという利点もある。

音を出すには `readFrames()` のループを出力デバイスへの書き込みに置き換える。ループの形は変わらない。

## EspBle のバージョン

これらのサンプルは EspBle の Classic Bluetooth 対応(`EspBleClassic.h`)を必要とするが、**まだ EspBle のリリースには入っていない**。そのため各 `sketch.yaml` はこのリポジトリの隣にあるローカルチェックアウトを指している。

```yaml
libraries:
  - dir: ../../
  - dir: ../../../EspBle
  - PCMFlow (0.2.1)
```

[EspBle](https://github.com/tanakamasayuki/EspBle) を PCMFlowBluetooth の隣に clone するか、パスを書き換えること。Classic 対応がリリースされたら、通常のバージョン指定に置き換える。

## 実行

```sh
cd A2dpSinkToPcm
arduino-cli compile --profile esp32
arduino-cli upload --profile esp32 -p /dev/ttyUSB0
```

その後、スマートフォンや PC からボードとペアリングして再生する。115200 baud の Serial に、ネゴシエート結果が一度、続いて毎秒の統計が出力される。
