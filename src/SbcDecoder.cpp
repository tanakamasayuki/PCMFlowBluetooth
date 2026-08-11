#include "SbcDecoder.h"

#include <stdlib.h>
#include <string.h>

#include "external/sbc_config.h"
#include "external/sbc/decoder/include/oi_codec_sbc.h"

namespace
{
constexpr uint8_t kMaxChannels = SBC_MAX_CHANNELS;

// Number of OI_UINT32 words the codec needs for a stereo stream using the
// fast filter buffers. The mono case needs strictly less, and allocating for
// the maximum keeps begin() free of any dependency on the negotiation, which
// may not have happened yet.
constexpr size_t kDecoderDataWords =
    CODEC_DATA_WORDS(SBC_MAX_CHANNELS, SBC_CODEC_FAST_FILTER_BUFFERS);

OI_CODEC_SBC_DECODER_CONTEXT *contextOf(void *storage)
{
    return static_cast<OI_CODEC_SBC_DECODER_CONTEXT *>(storage);
}

const OI_CODEC_SBC_DECODER_CONTEXT *contextOf(const void *storage)
{
    return static_cast<const OI_CODEC_SBC_DECODER_CONTEXT *>(storage);
}
} // namespace

bool SbcDecoder::begin()
{
    // If these ever fail the opaque storage in the header is out of step
    // with the backend and must be resized.
    static_assert(sizeof(SbcDecoder::ContextStorage) >=
                      sizeof(OI_CODEC_SBC_DECODER_CONTEXT),
                  "ContextStorage too small for the SBC backend context");
    static_assert(alignof(SbcDecoder::ContextStorage) >=
                      alignof(OI_CODEC_SBC_DECODER_CONTEXT),
                  "ContextStorage underaligned for the SBC backend context");

    end();

    decoderDataBytes_ = kDecoderDataWords * sizeof(OI_UINT32);
    decoderData_ = malloc(decoderDataBytes_);
    if (decoderData_ == nullptr)
    {
        decoderDataBytes_ = 0;
        return false;
    }

    reset();
    return isReady();
}

void SbcDecoder::end()
{
    free(decoderData_);
    decoderData_ = nullptr;
    decoderDataBytes_ = 0;
    format_ = PCMFormat();
    memset(&context_, 0, sizeof(context_));
}

void SbcDecoder::reset(uint8_t pcmChannels)
{
    if (decoderData_ == nullptr) return;

    pcmStride_ = (pcmChannels == 1) ? 1 : 2;

    memset(&context_, 0, sizeof(context_));
    format_ = PCMFormat();
    streamChannels_ = 0;

    const OI_STATUS status = OI_CODEC_SBC_DecoderReset(
        contextOf(&context_),
        static_cast<OI_UINT32 *>(decoderData_),
        static_cast<OI_UINT32>(decoderDataBytes_),
        kMaxChannels,
        pcmStride_,
        FALSE,   // enhanced SBC: not used by A2DP
        FALSE);  // mSBC: HFP WBS phase, not this one

    if (status != OI_STATUS_SUCCESS)
    {
        // The only documented failures are a too-small buffer or a bad
        // channel/stride combination, both fixed at compile time here. Drop
        // the allocation so isReady() reports the truth rather than letting
        // decodeFrame() run against an uninitialized context.
        free(decoderData_);
        decoderData_ = nullptr;
        decoderDataBytes_ = 0;
    }
}

bool SbcDecoder::decodeFrame(const uint8_t *in, size_t inLength, size_t &consumed,
                             int16_t *out, size_t outFrameCapacity, size_t &written)
{
    consumed = 0;
    written = 0;

    if (!isReady() || in == nullptr || out == nullptr) return false;
    if (inLength == 0) return false;

    const OI_BYTE *frameData = reinterpret_cast<const OI_BYTE *>(in);
    OI_UINT32 frameBytes = static_cast<OI_UINT32>(inLength);
    OI_UINT32 pcmBytes =
        static_cast<OI_UINT32>(outFrameCapacity * pcmStride_ * sizeof(int16_t));

    const OI_STATUS status = OI_CODEC_SBC_DecodeFrame(
        contextOf(&context_), &frameData, &frameBytes, out, &pcmBytes);

    if (status == OI_STATUS_SUCCESS)
    {
        // frameData/frameBytes were advanced past the frame just decoded.
        consumed = inLength - frameBytes;

        const OI_CODEC_SBC_FRAME_INFO &info = contextOf(&context_)->common.frameInfo;
        streamChannels_ = info.nrof_channels;

        // The interleave is the stride the decoder was reset with, not the
        // stream's own channel count: a mono stream decoded at stride 2 is
        // duplicated into both slots by the backend, and the PCM really is
        // two-channel. Reporting nrof_channels here would describe a layout
        // the buffer does not have.
        format_.sampleRate = info.frequency;
        format_.channels = pcmStride_;
        format_.bitsPerSample = 16;

        written = pcmBytes / (pcmStride_ * sizeof(int16_t));
        return true;
    }

    if (status == OI_CODEC_SBC_NOT_ENOUGH_HEADER_DATA ||
        status == OI_CODEC_SBC_NOT_ENOUGH_BODY_DATA)
    {
        // A partial frame at the tail of the buffer. Consuming anything here
        // would destroy a frame that is merely incomplete, so report no
        // progress and let the caller bring more data.
        consumed = 0;
        return false;
    }

    // Malformed frame: bad syncword, checksum mismatch, or a configuration
    // the decoder was limited away from. Skip one byte so the caller
    // resynchronizes on the next syncword rather than retrying the same
    // bad one forever.
    consumed = 1;
    return false;
}
