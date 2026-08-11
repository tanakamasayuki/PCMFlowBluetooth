#ifndef PCMFLOWBLUETOOTH_H
#define PCMFLOWBLUETOOTH_H

// PCMFlowBluetooth — Bluetooth Classic audio add-on for PCMFlow.
//
// Connects the encoded audio payloads handled by EspBle to PCMFlow's
// PCMSource / PCMSink boundary. This library owns only three things:
// the codec, the packet queue, and the PCM boundary. Bluetooth profiles
// stay in EspBle; device output stays in PCMFlow / PCMFlowDevice.
//
// See SPEC.md (English) / SPEC.ja.md (日本語) for the full specification.
//
// Layering (SPEC.md §3.1):
//
//   EspBleA2dpSinkAdapter   depends on EspBle. ESP32 only. Callback wiring.
//           |
//   A2dpSinkStream          EspBle-independent. Queue + decoder. PCMSource.
//           |
//   SbcDecoder              EspBle-independent. SBC frames -> PCM.
//
// Only the adapter needs a Bluetooth stack, so everything below it builds
// and unit-tests on a host as well as on hardware.

#include "pcmflowbluetooth_version.h"

#include "PCMFlowBluetoothError.h"
#include "EncodedAudioFormat.h"
#include "EncodedPacketQueue.h"
#include "SbcDecoder.h"
#include "A2dpSinkStream.h"

// The EspBle adapter is compiled only where Classic Bluetooth exists, and
// only when EspBle is actually installed. Including this umbrella header on
// any other target is fine: the core (queue, decoder, PCMSource) is
// portable, and the adapter simply is not declared. See SPEC.md §13.1.
#include "EspBleA2dpSinkAdapter.h"

#define PCMFLOWBLUETOOTH_HAS_ESPBLE_ADAPTER PCMFLOWBLUETOOTH_ESPBLE_ADAPTER_AVAILABLE

#endif // PCMFLOWBLUETOOTH_H
