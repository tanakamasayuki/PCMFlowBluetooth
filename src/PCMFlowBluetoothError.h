#ifndef PCMFLOWBLUETOOTH_ERROR_H
#define PCMFLOWBLUETOOTH_ERROR_H

#include <stdint.h>

// Reason the most recent operation failed.
//
// The counters on A2dpSinkStream are the primary observation surface — they
// say how often something went wrong. This says what it was, for the most
// recent occurrence only.
enum class PCMFlowBluetoothError : uint8_t
{
    None = 0,
    NotInitialized,       // used before begin(), or after end()
    InvalidConfiguration, // begin() rejected the Config
    AllocationFailed,     // begin() could not obtain its buffers
    UnsupportedCodec,     // codec configuration this build cannot decode
    QueueOverflow,        // an encoded packet did not fit and was dropped whole
    InvalidFrame,         // frame the decoder could not parse; resynchronized
    DecodeFailure,        // decoder rejected an otherwise well-framed packet
    PcmOverflow,          // PCM was discarded because the consumer fell behind
    ConcurrentUpdate,     // update() called while another call was running
};

// Stable, printable name. Useful in tests and in Serial diagnostics, where
// the numeric value alone is unhelpful.
inline const char *toString(PCMFlowBluetoothError error)
{
    switch (error)
    {
    case PCMFlowBluetoothError::None:                 return "None";
    case PCMFlowBluetoothError::NotInitialized:       return "NotInitialized";
    case PCMFlowBluetoothError::InvalidConfiguration: return "InvalidConfiguration";
    case PCMFlowBluetoothError::AllocationFailed:     return "AllocationFailed";
    case PCMFlowBluetoothError::UnsupportedCodec:     return "UnsupportedCodec";
    case PCMFlowBluetoothError::QueueOverflow:        return "QueueOverflow";
    case PCMFlowBluetoothError::InvalidFrame:         return "InvalidFrame";
    case PCMFlowBluetoothError::DecodeFailure:        return "DecodeFailure";
    case PCMFlowBluetoothError::PcmOverflow:          return "PcmOverflow";
    case PCMFlowBluetoothError::ConcurrentUpdate:     return "ConcurrentUpdate";
    }
    return "Unknown";
}

#endif // PCMFLOWBLUETOOTH_ERROR_H
