// Contract tests for SbcDecoder.
//
// Vectors in input/sbc_vectors.h are produced by tools/gen_sbc_vectors.py
// from the vendored Broadcom encoder, one per configuration the A2DP
// negotiation can select. Each carries the frame count and layout the
// decoder must arrive at independently, from the bitstream alone.
//
// SBC is lossy and the synthesis filter bank has a startup transient, so the
// signal checks are statistical: energy present, dominant frequency right.
// Sample-exact comparison would only re-assert what the vendored codec
// already guarantees.
//
// See SPEC.md §4.1.

#include <PCMFlowBluetooth.h>

#include "input/sbc_vectors.h"

static int g_pass = 0;
static int g_total = 0;

#define EXPECT_TRUE(name, cond) do { \
    ++g_total; \
    if (cond) { ++g_pass; Serial.print("PASS "); Serial.println(name); } \
    else { Serial.print("FAIL "); Serial.print(name); Serial.println(" cond"); } \
} while (0)

#define EXPECT_EQ(name, expected, actual) do { \
    ++g_total; \
    const long _e = (long)(expected); \
    const long _a = (long)(actual); \
    if (_e == _a) { ++g_pass; Serial.print("PASS "); Serial.println(name); } \
    else { \
        Serial.print("FAIL "); Serial.print(name); \
        Serial.print(" expected="); Serial.print(_e); \
        Serial.print(" actual=");   Serial.println(_a); \
    } \
} while (0)

// One SBC frame decodes to at most 128 PCM frames; two channels of those.
static int16_t g_pcm[SbcDecoder::maxPcmFramesPerSbcFrame() * 2];

// Decode a whole vector, accumulating simple signal statistics.
struct DecodeResult
{
    size_t framesDecoded = 0;   // SBC frames
    size_t pcmFrames = 0;
    size_t bytesConsumed = 0;
    bool sawFailure = false;
    long peak = 0;
    long zeroCrossings = 0;     // on the left channel
};

static DecodeResult decode_all(SbcDecoder &dec, const SbcVector &v)
{
    DecodeResult r;
    int16_t previous = 0;
    bool havePrevious = false;

    while (r.bytesConsumed < v.length) {
        size_t consumed = 0;
        size_t written = 0;
        const bool ok = dec.decodeFrame(
            v.data + r.bytesConsumed, v.length - r.bytesConsumed, consumed,
            g_pcm, SbcDecoder::maxPcmFramesPerSbcFrame(), written);

        if (!ok) {
            r.sawFailure = true;
            if (consumed == 0) break;   // incomplete tail
            r.bytesConsumed += consumed;
            continue;
        }

        ++r.framesDecoded;
        r.pcmFrames += written;
        r.bytesConsumed += consumed;

        const uint8_t channels = dec.format().channels;
        for (size_t i = 0; i < written; ++i) {
            const int16_t left = g_pcm[i * channels];
            const long magnitude = left < 0 ? -(long)left : (long)left;
            if (magnitude > r.peak) r.peak = magnitude;
            if (havePrevious && ((previous < 0) != (left < 0))) ++r.zeroCrossings;
            previous = left;
            havePrevious = true;
        }
    }
    return r;
}

static void test_lifecycle()
{
    SbcDecoder dec;
    EXPECT_TRUE("not-ready-before-begin", !dec.isReady());

    size_t consumed = 0, written = 0;
    EXPECT_TRUE("decode-before-begin-fails",
                !dec.decodeFrame(kSbc_stereo_48k, sizeof(kSbc_stereo_48k),
                                 consumed, g_pcm, 128, written));

    EXPECT_TRUE("begin", dec.begin());
    EXPECT_TRUE("ready", dec.isReady());
    EXPECT_TRUE("no-format-before-decode", !dec.hasFormat());
    EXPECT_TRUE("format-invalid-before-decode", !dec.format().isValid());
    EXPECT_EQ("stream-channels-zero", 0, dec.streamChannels());

    dec.end();
    EXPECT_TRUE("not-ready-after-end", !dec.isReady());
    EXPECT_TRUE("begin-again", dec.begin());
}

static void test_vectors()
{
    for (size_t i = 0; i < kSbcVectorCount; ++i) {
        const SbcVector &v = kSbcVectors[i];

        SbcDecoder dec;
        dec.begin();
        dec.reset(v.streamChannels);

        const DecodeResult r = decode_all(dec, v);

        // Every byte of the vector belongs to a frame, so a correct decode
        // consumes all of it and finds exactly the frames the encoder wrote.
        EXPECT_EQ("frames", v.frameCount, r.framesDecoded);
        EXPECT_EQ("consumed-all", v.length, r.bytesConsumed);
        EXPECT_TRUE("no-failures", !r.sawFailure);
        EXPECT_EQ("pcm-frames",
                  (long)v.frameCount * v.pcmFramesPerSbcFrame, r.pcmFrames);

        // The format is read out of the bitstream, not configured.
        EXPECT_EQ("rate", v.sampleRate, dec.format().sampleRate);
        EXPECT_EQ("bits", 16, dec.format().bitsPerSample);
        EXPECT_EQ("stream-channels", v.streamChannels, dec.streamChannels());
        EXPECT_EQ("pcm-channels", v.streamChannels, dec.format().channels);
        EXPECT_TRUE("format-valid", dec.format().isValid());

        // Signal sanity. The bitpool-2 vector is deliberately at the bottom
        // of the negotiable range and carries almost no information, so only
        // the well-coded vectors are checked for tone.
        EXPECT_TRUE("has-signal", r.peak > 1000);
        if (v.bitpool >= 26) {
            // Two zero crossings per cycle of the tone. The band is wide
            // because zero-crossing counting is a crude frequency estimate:
            // the synthesis filter bank's startup transient and quantization
            // noise both add crossings near zero, more so in the small-frame
            // configurations. It is still decisive — decoding noise instead
            // of audio produced roughly twelve times the expected count.
            const long seconds_x1000 = (long)r.pcmFrames * 1000 / v.sampleRate;
            const long expected = 2L * v.toneHz * seconds_x1000 / 1000;
            EXPECT_TRUE("tone-frequency",
                        r.zeroCrossings >= expected * 6 / 10 &&
                        r.zeroCrossings <= expected * 14 / 10);
            // The encoder was fed a 12000-amplitude tone.
            EXPECT_TRUE("amplitude-preserved", r.peak > 6000 && r.peak < 24000);
        }
    }
}

// A mono stream decoded into a two-channel interleave is duplicated by the
// backend. That is valid output, and format() must describe the interleave
// that is actually in the buffer rather than the stream's own channel count.
static void test_mono_into_stereo_interleave()
{
    const SbcVector *mono = nullptr;
    for (size_t i = 0; i < kSbcVectorCount; ++i) {
        if (kSbcVectors[i].streamChannels == 1) { mono = &kSbcVectors[i]; break; }
    }
    EXPECT_TRUE("mono-vector-present", mono != nullptr);
    if (mono == nullptr) return;

    SbcDecoder dec;
    dec.begin();
    dec.reset(2);   // ask for a two-channel interleave

    size_t consumed = 0, written = 0;
    const bool ok = dec.decodeFrame(mono->data, mono->length, consumed,
                                    g_pcm, SbcDecoder::maxPcmFramesPerSbcFrame(),
                                    written);
    EXPECT_TRUE("mono-decodes", ok);
    EXPECT_EQ("mono-stream-channels", 1, dec.streamChannels());
    EXPECT_EQ("mono-pcm-channels", 2, dec.format().channels);
    EXPECT_EQ("mono-pcm-frames", mono->pcmFramesPerSbcFrame, written);

    bool duplicated = true;
    for (size_t i = 0; i < written; ++i) {
        if (g_pcm[i * 2] != g_pcm[i * 2 + 1]) { duplicated = false; break; }
    }
    EXPECT_TRUE("mono-duplicated-across-channels", duplicated);
}

static void test_incomplete_frame_makes_no_progress()
{
    SbcDecoder dec;
    dec.begin();
    dec.reset(2);

    // Half a frame: the decoder must ask for more rather than discarding it.
    const size_t half = 118 / 2;
    size_t consumed = 12345, written = 12345;
    EXPECT_TRUE("incomplete-fails",
                !dec.decodeFrame(kSbc_stereo_48k, half, consumed,
                                 g_pcm, 128, written));
    EXPECT_EQ("incomplete-consumes-nothing", 0, consumed);
    EXPECT_EQ("incomplete-writes-nothing", 0, written);

    // Given the whole frame, the same decoder decodes it.
    EXPECT_TRUE("complete-succeeds",
                dec.decodeFrame(kSbc_stereo_48k, sizeof(kSbc_stereo_48k),
                                consumed, g_pcm, 128, written));
    EXPECT_TRUE("complete-consumes", consumed > 0);
}

static void test_garbage_resynchronizes()
{
    SbcDecoder dec;
    dec.begin();
    dec.reset(2);

    // Prefix a valid stream with bytes that are not a syncword. The decoder
    // must skip forward rather than stall, and then decode every frame.
    static uint8_t buffer[64 + sizeof(kSbc_stereo_48k)];
    const size_t garbage = 7;
    for (size_t i = 0; i < garbage; ++i) buffer[i] = (uint8_t)(0x11 * (i + 1));
    memcpy(buffer + garbage, kSbc_stereo_48k, sizeof(kSbc_stereo_48k));
    const size_t total = garbage + sizeof(kSbc_stereo_48k);

    size_t offset = 0;
    size_t frames = 0;
    while (offset < total) {
        size_t consumed = 0, written = 0;
        if (dec.decodeFrame(buffer + offset, total - offset, consumed,
                            g_pcm, 128, written)) {
            ++frames;
        } else if (consumed == 0) {
            break;
        }
        offset += consumed;
    }
    // The backend hunts for the syncword itself, so the leading garbage is
    // consumed as part of the first successful decode rather than reported
    // as a failure. Either way every frame must come out and every byte must
    // be accounted for.
    EXPECT_EQ("all-frames-after-garbage", 8, frames);
    EXPECT_EQ("consumed-garbage-and-frames", total, offset);

    // A buffer that is nothing but garbage yields no frames and always makes
    // progress, so a caller draining a queue cannot spin.
    static uint8_t noise[128];
    for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = (uint8_t)(i * 3 + 1);
    offset = 0;
    bool progressed = true;
    while (offset < sizeof(noise)) {
        size_t consumed = 0, written = 0;
        const bool ok = dec.decodeFrame(noise + offset, sizeof(noise) - offset,
                                        consumed, g_pcm, 128, written);
        if (ok) { progressed = false; break; }     // must not "decode" noise
        if (consumed == 0) break;                  // ran out of data: fine
        offset += consumed;
    }
    EXPECT_TRUE("noise-never-decodes", progressed);
}

// reset() must clear the filter history, so decoding the same stream twice
// from a reset decoder gives the same output both times.
static void test_reset_restores_state()
{
    SbcDecoder dec;
    dec.begin();

    dec.reset(2);
    const DecodeResult first = decode_all(dec, kSbcVectors[0]);

    dec.reset(2);
    const DecodeResult second = decode_all(dec, kSbcVectors[0]);

    EXPECT_EQ("reset-same-frames", first.framesDecoded, second.framesDecoded);
    EXPECT_EQ("reset-same-pcm", first.pcmFrames, second.pcmFrames);
    EXPECT_EQ("reset-same-peak", first.peak, second.peak);
    EXPECT_EQ("reset-same-crossings", first.zeroCrossings, second.zeroCrossings);

    // Without a reset in between, the filter history carries over, so the
    // decoder is not expected to reproduce the startup transient. It must
    // still decode every frame.
    const DecodeResult third = decode_all(dec, kSbcVectors[0]);
    EXPECT_EQ("continues-without-reset", first.framesDecoded, third.framesDecoded);
}

static void test_rejects_bad_arguments()
{
    SbcDecoder dec;
    dec.begin();
    dec.reset(2);

    size_t consumed = 1, written = 1;
    EXPECT_TRUE("null-input", !dec.decodeFrame(nullptr, 100, consumed, g_pcm, 128, written));
    EXPECT_TRUE("null-output",
                !dec.decodeFrame(kSbc_stereo_48k, sizeof(kSbc_stereo_48k),
                                 consumed, nullptr, 128, written));
    EXPECT_TRUE("zero-length",
                !dec.decodeFrame(kSbc_stereo_48k, 0, consumed, g_pcm, 128, written));
    EXPECT_EQ("bad-args-consume-nothing", 0, consumed);
    EXPECT_EQ("bad-args-write-nothing", 0, written);

    // Too small an output buffer must fail rather than overrun it.
    EXPECT_TRUE("tiny-output-buffer",
                !dec.decodeFrame(kSbc_stereo_48k, sizeof(kSbc_stereo_48k),
                                 consumed, g_pcm, 1, written));
}

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("TEST start sbc_decoder");

    test_lifecycle();
    test_vectors();
    test_mono_into_stereo_interleave();
    test_incomplete_frame_makes_no_progress();
    test_garbage_resynchronizes();
    test_reset_restores_state();
    test_rejects_bad_arguments();

    Serial.print("TEST done ");
    Serial.print(g_pass);
    Serial.print("/");
    Serial.println(g_total);
}

void loop()
{
    delay(1);
}
