#ifndef PCMFLOWBLUETOOTH_ENCODEDAUDIOFORMAT_H
#define PCMFLOWBLUETOOTH_ENCODEDAUDIOFORMAT_H

#include <stdint.h>
#include <stddef.h>

// EspBle-independent description of a negotiated Bluetooth Classic audio
// codec. This is the boundary that keeps the core of this library free of
// EspBle types: `EspBleA2dpSinkAdapter` copies the fields it needs out of
// `EspBleClassicA2dpCodecConfig` into this POD, and everything below the
// adapter only ever sees this.
//
// Keeping it a POD also means host unit tests can construct any codec
// configuration directly, without a Bluetooth stack in the picture.
//
// See SPEC.md §4.2.

enum class EncodedAudioCodec : uint8_t
{
    Unknown = 0,
    Sbc,
    Msbc,
    Cvsd,
};

struct EncodedAudioFormat
{
    EncodedAudioCodec codec = EncodedAudioCodec::Unknown;
    uint32_t sampleRate = 0;
    uint8_t channels = 0;
    uint8_t minimumBitpool = 0;
    uint8_t maximumBitpool = 0;

    bool isValid() const
    {
        return codec != EncodedAudioCodec::Unknown && sampleRate > 0 &&
               (channels == 1 || channels == 2);
    }

    // Two configurations describing the same stream. A change means the
    // decoder state and every queue must be discarded (SPEC.md §7).
    bool operator==(const EncodedAudioFormat &other) const
    {
        return codec == other.codec && sampleRate == other.sampleRate &&
               channels == other.channels &&
               minimumBitpool == other.minimumBitpool &&
               maximumBitpool == other.maximumBitpool;
    }

    bool operator!=(const EncodedAudioFormat &other) const
    {
        return !(*this == other);
    }
};

#endif // PCMFLOWBLUETOOTH_ENCODEDAUDIOFORMAT_H
