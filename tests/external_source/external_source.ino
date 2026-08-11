// Integration test: A2dpSinkStream plugged into PCMFlow as a PCMSource.
//
// This is the wiring an application actually uses:
//
//   Bluetooth callback -> pushEncoded()
//   user task          -> A2dpSinkStream::update()   (SBC decode)
//   user task          -> PCMFlow::pump()            (pulls via readFrames())
//   user task          -> PCMFlow::readFrames()      (to I2S / DAC / ...)
//
// The stream stands in for the Bluetooth side by pushing generated SBC
// packets directly, so the whole path is exercised without a radio.
//
// It also pins down the contract that matters most at this boundary:
// isEof() is always false, so a pause or a disconnect must not stop
// PCMFlow's pipeline (SPEC.md §4.5).

#include <PCMFlow.h>
#include <PCMFlowBluetooth.h>

#include "../sbc_decoder/input/sbc_vectors.h"

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

static const SbcVector &vector() { return kSbcVectors[0]; }  // stereo 48 kHz

static EncodedAudioFormat sbc_format(const SbcVector &v)
{
    EncodedAudioFormat f;
    f.codec = EncodedAudioCodec::Sbc;
    f.sampleRate = v.sampleRate;
    f.channels = v.streamChannels;
    f.minimumBitpool = 2;
    f.maximumBitpool = 53;
    return f;
}

static size_t frame_bytes(const SbcVector &v) { return v.length / v.frameCount; }

// Feed packets in, run both pumps, and collect whatever comes out the far
// end — the same loop a sketch would run.
static size_t run_pipeline(PCMFlow &audio, A2dpSinkStream &stream,
                           const SbcVector &v,
                           int16_t *out, size_t outCapacityFrames)
{
    const size_t stride = frame_bytes(v);
    size_t pushed = 0;
    size_t produced = 0;
    int idle = 0;

    while (produced < outCapacityFrames && idle < 64) {
        if (pushed < v.frameCount) {
            if (stream.pushEncoded(v.data + pushed * stride, stride, 1,
                                   1000 + (uint32_t)pushed * 128)) {
                ++pushed;
            }
        }
        stream.update();
        audio.pump();

        const size_t got = audio.readFrames(out + produced * 2,
                                            outCapacityFrames - produced);
        if (got == 0) {
            ++idle;
        } else {
            idle = 0;
            produced += got;
        }
    }
    return produced;
}

static int16_t g_out[2048 * 2];

static void test_pipeline()
{
    const SbcVector &v = vector();

    A2dpSinkStream stream;
    EXPECT_TRUE("stream-begin", stream.begin());
    stream.setCodecConfig(sbc_format(v));
    EXPECT_TRUE("stream-ready", stream.isReady());

    PCMFlow audio;
    audio.setInputSource(stream);

    // Ask for the same layout the stream produces, so the comparison below
    // is about the plumbing rather than about PCMFlow's conversion.
    PCMFormat out;
    out.sampleRate = v.sampleRate;
    out.channels = 2;
    out.bitsPerSample = 16;
    audio.setOutputFormat(out);

    const size_t want = (size_t)v.frameCount * v.pcmFramesPerSbcFrame;
    const size_t produced = run_pipeline(audio, stream, v, g_out, want);

    EXPECT_EQ("frames-through-pipeline", want, produced);
    EXPECT_EQ("stream-decoded-every-packet", v.frameCount, stream.decodedFrameCount());
    EXPECT_EQ("nothing-dropped", 0, stream.droppedPacketCount());
    EXPECT_EQ("nothing-invalid", 0, stream.invalidFrameCount());

    EXPECT_EQ("output-rate", v.sampleRate, audio.outputFormat().sampleRate);
    EXPECT_EQ("output-channels", 2, audio.outputFormat().channels);

    long peak = 0;
    for (size_t i = 0; i < produced * 2; ++i) {
        const long m = g_out[i] < 0 ? -(long)g_out[i] : g_out[i];
        if (m > peak) peak = m;
    }
    EXPECT_TRUE("audio-arrives-at-the-far-end", peak > 1000);
}

// A stream with nothing queued must look like a live source with no data
// right now, not like a finished one — otherwise a pause would end the
// pipeline and a reconnect would never resume it.
static void test_idle_stream_does_not_end_the_pipeline()
{
    const SbcVector &v = vector();

    A2dpSinkStream stream;
    stream.begin();
    stream.setCodecConfig(sbc_format(v));

    PCMFlow audio;
    audio.setInputSource(stream);

    // PCMFlow has no default output format and latches the failure, so this
    // has to be set before the first pump().
    PCMFormat out;
    out.sampleRate = v.sampleRate;
    out.channels = 2;
    out.bitsPerSample = 16;
    audio.setOutputFormat(out);

    // Pump with an empty queue: no data, no end of stream.
    for (int i = 0; i < 8; ++i) {
        stream.update();
        audio.pump();
    }
    EXPECT_EQ("no-frames-while-idle", 0, audio.readFrames(g_out, 128));
    EXPECT_TRUE("source-never-reports-eof", !stream.isEof());
    EXPECT_TRUE("source-still-ready", stream.isReady());

    // Data arriving later still flows.
    const size_t produced = run_pipeline(audio, stream, v, g_out, 256);
    EXPECT_TRUE("pipeline-not-failed", audio.lastError() == PCMFlow::Error::None);
    EXPECT_EQ("resumes-after-idle", 256, produced);
}

// The same PCMFlow instance has to survive the codec being renegotiated
// underneath it, which is what happens when a peer reconnects with a
// different configuration.
static void test_survives_codec_change()
{
    const SbcVector &stereo = vector();
    const SbcVector *mono = nullptr;
    for (size_t i = 0; i < kSbcVectorCount; ++i) {
        if (kSbcVectors[i].streamChannels == 1) { mono = &kSbcVectors[i]; break; }
    }
    EXPECT_TRUE("mono-vector-present", mono != nullptr);
    if (mono == nullptr) return;

    A2dpSinkStream stream;
    stream.begin();
    stream.setCodecConfig(sbc_format(stereo));

    PCMFlow audio;
    audio.setInputSource(stream);

    PCMFormat out;
    out.sampleRate = stereo.sampleRate;
    out.channels = 2;
    out.bitsPerSample = 16;
    audio.setOutputFormat(out);

    EXPECT_TRUE("stereo-flows", run_pipeline(audio, stream, stereo, g_out, 256) == 256);

    stream.setCodecConfig(sbc_format(*mono));
    EXPECT_EQ("source-now-mono", 1, stream.format().channels);
    EXPECT_TRUE("source-still-ready-after-change", stream.isReady());

    // PCMFlow upmixes the mono source to the stereo output it was asked for.
    const size_t produced = run_pipeline(audio, stream, *mono, g_out, 256);
    EXPECT_EQ("mono-flows-after-change", 256, produced);

    long peak = 0;
    for (size_t i = 0; i < produced * 2; ++i) {
        const long m = g_out[i] < 0 ? -(long)g_out[i] : g_out[i];
        if (m > peak) peak = m;
    }
    EXPECT_TRUE("mono-audio-arrives", peak > 1000);
}

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("TEST start external_source");

    test_pipeline();
    test_idle_stream_does_not_end_the_pipeline();
    test_survives_codec_change();

    Serial.print("TEST done ");
    Serial.print(g_pass);
    Serial.print("/");
    Serial.println(g_total);
}

void loop()
{
    delay(1);
}
