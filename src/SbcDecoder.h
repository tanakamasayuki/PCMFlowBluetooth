#ifndef PCMFLOWBLUETOOTH_SBCDECODER_H
#define PCMFLOWBLUETOOTH_SBCDECODER_H

#include <stdint.h>
#include <stddef.h>

#include <PCMFormat.h>

// SBC frame decoder.
//
// Wraps the vendored OI codec (src/external/sbc/) and exposes nothing of it:
// no OI type appears in this header, so a later phase can swap the backend
// without touching anything above (SPEC.md §4.1).
//
// This class knows nothing about EspBle or the PCMFlow pipeline. It decodes
// one frame per call and leaves iteration over a multi-frame packet to
// A2dpSinkStream, which is also what keeps it testable on the host profile.
//
// The output format comes from the SBC bitstream header, not from the A2DP
// negotiation: a Source is free to change bitpool per frame, and the frame
// header is what actually describes the samples being decoded.
class SbcDecoder
{
public:
    // Largest PCM frame count a single SBC frame can decode to
    // (16 blocks x 8 subbands). Callers can size a decode buffer from this
    // without knowing the negotiated configuration.
    static constexpr size_t maxPcmFramesPerSbcFrame() { return 128; }

    SbcDecoder() = default;
    ~SbcDecoder() { end(); }

    SbcDecoder(const SbcDecoder &) = delete;
    SbcDecoder &operator=(const SbcDecoder &) = delete;

    // Allocate the decoder state. This is the only call that touches the
    // heap; decodeFrame() never does.
    bool begin();

    // Free the decoder state.
    void end();

    // Reinitialize the decoder without reallocating. Call it when the codec
    // configuration changes or a stream restarts, so filter history from the
    // previous stream cannot leak into the next one.
    //
    // `pcmChannels` (1 or 2) fixes the interleave of the PCM this decoder
    // writes, and is what format().channels reports. Pass the negotiated
    // channel count. It matters for mono streams: asked for 2 the backend
    // duplicates each mono sample into both slots, which is valid output but
    // twice the data. Anything other than 1 or 2 is treated as 2.
    void reset(uint8_t pcmChannels = 2);

    // Decode the single SBC frame at the head of `in`.
    //
    // On success returns true, sets `consumed` to the bytes the frame
    // occupied and `written` to the PCM frames produced.
    //
    // On failure returns false and sets `consumed` to the number of bytes to
    // skip before retrying:
    //   - a malformed frame or a checksum mismatch skips past the bad
    //     syncword so the caller resynchronizes on the next one;
    //   - an incomplete frame sets `consumed` to 0, meaning "no progress is
    //     possible with this much data" — the caller must wait for more
    //     rather than discarding what it has.
    // `written` is 0 on any failure.
    //
    // `outFrameCapacity` is counted in PCM frames. Passing less than
    // maxPcmFramesPerSbcFrame() risks a BufferTooSmall failure on a frame
    // that is larger than expected.
    bool decodeFrame(const uint8_t *in, size_t inLength, size_t &consumed,
                     int16_t *out, size_t outFrameCapacity, size_t &written);

    // Layout of the PCM that decodeFrame() writes. The channel count is the
    // one given to reset(); the sample rate comes from the decoded frame
    // header, so format().isValid() only becomes true once a frame has been
    // decoded.
    const PCMFormat &format() const { return format_; }

    // Channels the encoded stream itself carries, from the last decoded
    // frame header. Differs from format().channels when a mono stream is
    // being written into a two-channel interleave. 0 before the first
    // successful decode.
    uint8_t streamChannels() const { return streamChannels_; }

    bool isReady() const { return decoderData_ != nullptr; }

    // True once a frame has been decoded and format() describes it.
    bool hasFormat() const { return format_.isValid(); }

private:
    // Opaque storage for the backend's context, so no OI type leaks into
    // this header. Sized generously and checked against the real type in
    // the .cpp with a static_assert.
    struct ContextStorage
    {
        alignas(8) uint8_t bytes[192];
    };

    ContextStorage context_;
    // The backend's scratch area. Held as void* because its element type is
    // the backend's own word type, which must not appear in this header.
    void *decoderData_ = nullptr;
    size_t decoderDataBytes_ = 0;
    uint8_t pcmStride_ = 2;
    uint8_t streamChannels_ = 0;
    PCMFormat format_;
};

#endif // PCMFLOWBLUETOOTH_SBCDECODER_H
