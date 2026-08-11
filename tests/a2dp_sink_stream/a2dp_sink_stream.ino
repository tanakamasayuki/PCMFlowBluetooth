// Contract tests for A2dpSinkStream.
//
// This is the class that sits between the Bluetooth callback and the PCMFlow
// pipeline, so the properties under test are the ones that would corrupt a
// live stream: data surviving a codec change, a slow consumer growing an
// unbounded backlog, a malformed packet stalling the queue, two tasks
// decoding at once.
//
// The encoded packets come from the same generated vectors the decoder tests
// use, so a failure here is a failure of the queueing and lifecycle logic
// rather than of the codec.
//
// See SPEC.md §4.2, §6, §7.

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

static const SbcVector &stereo48k() { return kSbcVectors[0]; }

static EncodedAudioFormat sbc_format(uint32_t rate, uint8_t channels)
{
    EncodedAudioFormat f;
    f.codec = EncodedAudioCodec::Sbc;
    f.sampleRate = rate;
    f.channels = channels;
    f.minimumBitpool = 2;
    f.maximumBitpool = 53;
    return f;
}

// Bytes occupied by one SBC frame of the given vector.
static size_t frame_bytes(const SbcVector &v) { return v.length / v.frameCount; }

// Push the vector one SBC frame per packet, the way a Source sending single
// frames would.
static size_t push_frames(A2dpSinkStream &s, const SbcVector &v,
                          size_t count, uint32_t startTimestamp)
{
    const size_t stride = frame_bytes(v);
    size_t pushed = 0;
    for (size_t i = 0; i < count && i < v.frameCount; ++i) {
        if (!s.pushEncoded(v.data + i * stride, stride, 1,
                           startTimestamp + (uint32_t)i * 128)) break;
        ++pushed;
    }
    return pushed;
}

static void test_begin_validation()
{
    A2dpSinkStream s;
    EXPECT_TRUE("not-ready-before-begin", !s.isReady());
    EXPECT_TRUE("eof-false-before-begin", !s.isEof());

    A2dpSinkStream::Config bad;
    bad.maximumPacketBytes = 1024;
    bad.encodedQueueBytes = 1024;  // no room for the record header
    EXPECT_TRUE("rejects-queue-below-one-packet", !s.begin(bad));
    EXPECT_TRUE("reports-invalid-config",
                s.lastError() == PCMFlowBluetoothError::InvalidConfiguration);

    bad.maximumPacketBytes = 0;
    EXPECT_TRUE("rejects-zero-packet-size", !s.begin(bad));

    A2dpSinkStream::Config zeroPcm;
    zeroPcm.pcmQueueFrames = 0;
    EXPECT_TRUE("rejects-zero-pcm-frames", !s.begin(zeroPcm));

    A2dpSinkStream::Config ok;
    ok.maximumPacketBytes = 1024;
    ok.encodedQueueBytes = 1024 + EncodedPacketQueue::overheadPerPacket();
    EXPECT_TRUE("accepts-exact-minimum", s.begin(ok));
    EXPECT_TRUE("no-error-after-begin",
                s.lastError() == PCMFlowBluetoothError::None);

    // Still not ready: nothing has been negotiated yet.
    EXPECT_TRUE("not-ready-without-codec", !s.isReady());
    EXPECT_TRUE("format-invalid-without-codec", !s.format().isValid());
}

static void test_push_requires_begin()
{
    A2dpSinkStream s;
    const SbcVector &v = stereo48k();
    EXPECT_TRUE("push-before-begin-fails",
                !s.pushEncoded(v.data, frame_bytes(v), 1, 0));
    EXPECT_TRUE("push-before-begin-reports",
                s.lastError() == PCMFlowBluetoothError::NotInitialized);
    EXPECT_EQ("no-packets-counted", 0, s.receivedPacketCount());
}

static void test_decode_pipeline()
{
    const SbcVector &v = stereo48k();

    A2dpSinkStream s;
    EXPECT_TRUE("begin", s.begin());
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));

    // The format settles at negotiation, not at the first decoded frame.
    EXPECT_TRUE("ready-after-codec-config", s.isReady());
    EXPECT_EQ("rate", v.sampleRate, s.format().sampleRate);
    EXPECT_EQ("channels", v.streamChannels, s.format().channels);
    EXPECT_EQ("bits", 16, s.format().bitsPerSample);
    EXPECT_TRUE("eof-always-false", !s.isEof());

    // Nothing decodes until update() runs: the callback path only queues.
    EXPECT_EQ("pushed", v.frameCount, push_frames(s, v, v.frameCount, 1000));
    EXPECT_EQ("received", v.frameCount, s.receivedPacketCount());
    EXPECT_EQ("no-pcm-before-update", 0, s.availableFrames());
    EXPECT_EQ("no-frames-decoded-before-update", 0, s.decodedFrameCount());

    s.update();
    EXPECT_EQ("decoded-every-frame", v.frameCount, s.decodedFrameCount());
    EXPECT_EQ("pcm-available",
              (long)v.frameCount * v.pcmFramesPerSbcFrame, s.availableFrames());
    EXPECT_EQ("no-drops", 0, s.droppedPacketCount());
    EXPECT_EQ("no-invalid", 0, s.invalidFrameCount());
    EXPECT_EQ("no-overflow", 0, s.pcmOverflowFrameCount());

    // readFrames() hands over what is there and never blocks.
    static int16_t pcm[256 * 2];
    const size_t got = s.readFrames(pcm, 256);
    EXPECT_EQ("read-256", 256, got);
    EXPECT_EQ("available-drops-by-read",
              (long)v.frameCount * v.pcmFramesPerSbcFrame - 256,
              s.availableFrames());

    long peak = 0;
    for (size_t i = 0; i < got * 2; ++i) {
        const long m = pcm[i] < 0 ? -(long)pcm[i] : pcm[i];
        if (m > peak) peak = m;
    }
    EXPECT_TRUE("decoded-audio-not-silence", peak > 1000);

    // Draining to empty returns 0 rather than stalling or repeating.
    while (s.availableFrames() > 0) s.readFrames(pcm, 256);
    EXPECT_EQ("empty-reads-zero", 0, s.readFrames(pcm, 256));
}

// A packet holding several SBC frames must be iterated, not treated as one.
static void test_multi_frame_packet()
{
    const SbcVector &v = stereo48k();

    A2dpSinkStream s;
    s.begin();
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));

    EXPECT_TRUE("push-whole-vector-as-one-packet",
                s.pushEncoded(v.data, v.length, v.frameCount, 5000));
    EXPECT_EQ("one-packet-received", 1, s.receivedPacketCount());

    s.update();
    EXPECT_EQ("all-frames-from-one-packet", v.frameCount, s.decodedFrameCount());
    EXPECT_EQ("all-pcm-from-one-packet",
              (long)v.frameCount * v.pcmFramesPerSbcFrame, s.availableFrames());
}

static void test_queue_overflow_drops_whole_packets()
{
    const SbcVector &v = stereo48k();
    const size_t stride = frame_bytes(v);

    A2dpSinkStream::Config config;
    config.maximumPacketBytes = stride;
    config.encodedQueueBytes =
        (stride + EncodedPacketQueue::overheadPerPacket()) * 3;

    A2dpSinkStream s;
    EXPECT_TRUE("begin-small-queue", s.begin(config));
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));

    // Push more than fits without draining.
    const size_t pushed = push_frames(s, v, v.frameCount, 1000);
    EXPECT_EQ("only-three-fit", 3, pushed);
    EXPECT_TRUE("overflow-reported",
                s.lastError() == PCMFlowBluetoothError::QueueOverflow);
    EXPECT_EQ("one-drop-counted", 1, s.droppedPacketCount());

    // What did fit is intact and decodes normally: an overflowing packet is
    // dropped whole, never truncated into the queue.
    s.update();
    EXPECT_EQ("three-frames-decoded", 3, s.decodedFrameCount());
    EXPECT_EQ("three-frames-of-pcm", 3 * v.pcmFramesPerSbcFrame, s.availableFrames());
    EXPECT_EQ("no-invalid-frames", 0, s.invalidFrameCount());

    // Draining frees the queue again.
    s.update();
    EXPECT_TRUE("queue-drained", push_frames(s, v, 3, 9000) == 3);
}

static void test_pcm_overflow_policies()
{
    const SbcVector &v = stereo48k();

    // Room for two SBC frames of PCM, then push four.
    A2dpSinkStream::Config config;
    config.pcmQueueFrames = v.pcmFramesPerSbcFrame * 2;

    {
        A2dpSinkStream s;
        config.pcmOverflowPolicy = PcmOverflowPolicy::DropOldest;
        s.begin(config);
        s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));
        push_frames(s, v, 4, 1000);
        s.update();

        EXPECT_EQ("oldest-decoded-all", 4, s.decodedFrameCount());
        EXPECT_EQ("oldest-queue-stays-bounded",
                  config.pcmQueueFrames, s.availableFrames());
        EXPECT_EQ("oldest-overflow-counted",
                  2 * v.pcmFramesPerSbcFrame, s.pcmOverflowFrameCount());
        EXPECT_TRUE("oldest-error-reported",
                    s.lastError() == PCMFlowBluetoothError::PcmOverflow);
    }
    {
        A2dpSinkStream s;
        config.pcmOverflowPolicy = PcmOverflowPolicy::DropNewest;
        s.begin(config);
        s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));
        push_frames(s, v, 4, 1000);
        s.update();

        EXPECT_EQ("newest-decoded-all", 4, s.decodedFrameCount());
        EXPECT_EQ("newest-queue-stays-bounded",
                  config.pcmQueueFrames, s.availableFrames());
        EXPECT_EQ("newest-overflow-counted",
                  2 * v.pcmFramesPerSbcFrame, s.pcmOverflowFrameCount());
    }
}

// Anything buffered when the codec changes belongs to the old stream.
static void test_codec_reconfiguration_discards()
{
    const SbcVector &stereo = stereo48k();
    const SbcVector *mono = nullptr;
    for (size_t i = 0; i < kSbcVectorCount; ++i) {
        if (kSbcVectors[i].streamChannels == 1) { mono = &kSbcVectors[i]; break; }
    }
    EXPECT_TRUE("mono-vector-present", mono != nullptr);
    if (mono == nullptr) return;

    A2dpSinkStream s;
    s.begin();
    s.setCodecConfig(sbc_format(stereo.sampleRate, stereo.streamChannels));

    push_frames(s, stereo, 4, 1000);
    s.update();
    EXPECT_TRUE("has-pcm-before-reconfig", s.availableFrames() > 0);

    // Undecoded packets too: push without an update in between.
    push_frames(s, stereo, 2, 2000);

    s.setCodecConfig(sbc_format(mono->sampleRate, mono->streamChannels));
    EXPECT_EQ("pcm-discarded", 0, s.availableFrames());
    EXPECT_EQ("new-channels", 1, s.format().channels);

    // The queued stereo packets must be gone, not decoded as mono.
    const uint32_t decodedBefore = s.decodedFrameCount();
    s.update();
    EXPECT_EQ("queued-packets-discarded", decodedBefore, s.decodedFrameCount());

    // The new configuration works.
    push_frames(s, *mono, 4, 3000);
    s.update();
    EXPECT_EQ("mono-decoded", decodedBefore + 4, s.decodedFrameCount());
    EXPECT_EQ("mono-pcm", 4 * mono->pcmFramesPerSbcFrame, s.availableFrames());

    // The same configuration applied twice must not disturb the stream.
    const size_t available = s.availableFrames();
    s.setCodecConfig(sbc_format(mono->sampleRate, mono->streamChannels));
    EXPECT_EQ("identical-config-is-a-no-op", available, s.availableFrames());
}

static void test_reset_discards_everything()
{
    const SbcVector &v = stereo48k();

    A2dpSinkStream s;
    s.begin();
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));

    push_frames(s, v, 4, 1000);
    s.update();
    push_frames(s, v, 2, 2000);
    EXPECT_TRUE("buffered-before-reset", s.availableFrames() > 0);

    s.reset();
    EXPECT_EQ("pcm-cleared", 0, s.availableFrames());
    EXPECT_TRUE("still-ready-after-reset", s.isReady());
    EXPECT_EQ("codec-config-survives", v.sampleRate, s.format().sampleRate);

    const uint32_t decodedBefore = s.decodedFrameCount();
    s.update();
    EXPECT_EQ("queued-packets-cleared", decodedBefore, s.decodedFrameCount());

    // Usable immediately afterwards, as it must be on stream restart.
    push_frames(s, v, 3, 3000);
    s.update();
    EXPECT_EQ("decodes-after-reset", decodedBefore + 3, s.decodedFrameCount());

    // Counters are diagnostics and survive reset(); resetCounters() clears
    // them separately.
    EXPECT_TRUE("counters-survive-reset", s.receivedPacketCount() > 0);
    s.resetCounters();
    EXPECT_EQ("counters-cleared", 0, s.receivedPacketCount());
    EXPECT_EQ("decoded-cleared", 0, s.decodedFrameCount());
    EXPECT_TRUE("pcm-survives-counter-reset", s.availableFrames() > 0);
}

static void test_malformed_packet_does_not_stall()
{
    const SbcVector &v = stereo48k();

    A2dpSinkStream s;
    s.begin();
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));

    // Pure noise: no syncword anywhere.
    static uint8_t noise[120];
    for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = (uint8_t)(i * 5 + 3);
    EXPECT_TRUE("noise-queued", s.pushEncoded(noise, sizeof(noise), 1, 100));

    s.update();
    EXPECT_EQ("noise-decodes-nothing", 0, s.decodedFrameCount());
    EXPECT_TRUE("invalid-counted", s.invalidFrameCount() > 0);
    EXPECT_EQ("no-pcm-from-noise", 0, s.availableFrames());

    // A truncated frame must not hold the queue: A2DP packets carry whole
    // frames, so the next packet starts a new one.
    EXPECT_TRUE("half-frame-queued",
                s.pushEncoded(v.data, frame_bytes(v) / 2, 1, 200));
    s.update();
    EXPECT_TRUE("truncation-counted",
                s.decodeFailureCount() > 0 || s.invalidFrameCount() > 0);

    // Good packets after the bad ones decode normally.
    push_frames(s, v, 4, 300);
    s.update();
    EXPECT_EQ("recovers-after-bad-packets", 4, s.decodedFrameCount());
    EXPECT_EQ("recovered-pcm", 4 * v.pcmFramesPerSbcFrame, s.availableFrames());
}

static void test_unsupported_codec()
{
    A2dpSinkStream s;
    s.begin();

    EncodedAudioFormat cvsd = sbc_format(8000, 1);
    cvsd.codec = EncodedAudioCodec::Cvsd;
    s.setCodecConfig(cvsd);

    EXPECT_TRUE("unsupported-reported",
                s.lastError() == PCMFlowBluetoothError::UnsupportedCodec);
    EXPECT_TRUE("not-ready-for-unsupported", !s.isReady());
    EXPECT_TRUE("format-invalid-for-unsupported", !s.format().isValid());

    // Packets still queue, but nothing is decoded as if it were SBC.
    const SbcVector &v = stereo48k();
    s.pushEncoded(v.data, frame_bytes(v), 1, 1);
    s.update();
    EXPECT_EQ("nothing-decoded-for-unsupported", 0, s.decodedFrameCount());
}

static void test_timestamp_discontinuity()
{
    const SbcVector &v = stereo48k();
    const size_t stride = frame_bytes(v);

    A2dpSinkStream s;
    s.begin();
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));

    s.pushEncoded(v.data, stride, 1, 1000);
    s.pushEncoded(v.data, stride, 1, 1128);
    s.pushEncoded(v.data, stride, 1, 1256);
    EXPECT_EQ("monotonic-is-clean", 0, s.timestampDiscontinuityCount());

    s.pushEncoded(v.data, stride, 1, 900);   // went backwards
    EXPECT_EQ("backwards-counted", 1, s.timestampDiscontinuityCount());

    s.pushEncoded(v.data, stride, 1, 900);   // stalled
    EXPECT_EQ("repeat-counted", 2, s.timestampDiscontinuityCount());

    // A discontinuity is a diagnostic, not a reason to drop audio.
    s.update();
    EXPECT_EQ("all-packets-still-decoded", 5, s.decodedFrameCount());
}

// update() is single-consumer, guarded by an atomic flag. Genuine
// concurrency is NOT covered here: the two profiles have no common threading
// primitive, and this suite deliberately runs the same source on both. What
// is covered is that the guard is released, so repeated calls keep working —
// a guard that leaked would show up as the stream silently going deaf after
// one update(). The refusal path itself is exercised on hardware, in
// tests/peer/, where a second task is available.
static void test_update_guard_is_released()
{
    const SbcVector &v = stereo48k();

    A2dpSinkStream s;
    s.begin();
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));

    for (int round = 0; round < 4; ++round) {
        push_frames(s, v, 2, 1000 + (uint32_t)round * 1000);
        s.update();
        EXPECT_TRUE("update-not-refused",
                    s.lastError() != PCMFlowBluetoothError::ConcurrentUpdate);
        while (s.availableFrames() > 0) {
            static int16_t sink[128 * 2];
            s.readFrames(sink, 128);
        }
    }
    EXPECT_EQ("all-rounds-decoded", 8, s.decodedFrameCount());
}

static void test_end_releases()
{
    const SbcVector &v = stereo48k();

    A2dpSinkStream s;
    s.begin();
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));
    push_frames(s, v, 2, 1000);
    s.update();

    s.end();
    EXPECT_TRUE("not-ready-after-end", !s.isReady());
    EXPECT_EQ("no-pcm-after-end", 0, s.availableFrames());
    EXPECT_TRUE("push-after-end-fails",
                !s.pushEncoded(v.data, frame_bytes(v), 1, 1));

    static int16_t pcm[64];
    EXPECT_EQ("read-after-end-zero", 0, s.readFrames(pcm, 32));

    // Reusable.
    EXPECT_TRUE("begin-again", s.begin());
    s.setCodecConfig(sbc_format(v.sampleRate, v.streamChannels));
    EXPECT_TRUE("ready-again", s.isReady());
    push_frames(s, v, 2, 1000);
    s.update();
    EXPECT_EQ("decodes-again", 2, s.decodedFrameCount());
}

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("TEST start a2dp_sink_stream");

    test_begin_validation();
    test_push_requires_begin();
    test_decode_pipeline();
    test_multi_frame_packet();
    test_queue_overflow_drops_whole_packets();
    test_pcm_overflow_policies();
    test_codec_reconfiguration_discards();
    test_reset_discards_everything();
    test_malformed_packet_does_not_stall();
    test_unsupported_codec();
    test_timestamp_discontinuity();
    test_update_guard_is_released();
    test_end_releases();

    Serial.print("TEST done ");
    Serial.print(g_pass);
    Serial.print("/");
    Serial.println(g_total);
}

void loop()
{
    delay(1);
}
