// Contract tests for EncodedPacketQueue.
//
// The queue sits between the Bluetooth media callback and update(), so the
// properties that matter are the ones a truncated or partially stored packet
// would violate:
//
//   - a packet is stored whole or not at all
//   - a rejected packet leaves the queue exactly as it was
//   - records straddling the end of the ring survive intact
//   - metadata (frameCount, timestamp) travels with its payload
//   - the queue holds one mediaMtu-sized packet at the documented minimum
//
// See SPEC.md §7.

#include <PCMFlowBluetooth.h>

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

// Deterministic filler so a payload can be verified byte by byte.
static void fill_pattern(uint8_t *out, size_t length, uint8_t seed)
{
    for (size_t i = 0; i < length; ++i) {
        out[i] = (uint8_t)(seed + i * 7u);
    }
}

static bool check_pattern(const uint8_t *in, size_t length, uint8_t seed)
{
    for (size_t i = 0; i < length; ++i) {
        if (in[i] != (uint8_t)(seed + i * 7u)) return false;
    }
    return true;
}

static void test_begin_validation()
{
    EncodedPacketQueue q;
    EXPECT_TRUE("begin-rejects-zero", !q.begin(0));
    EXPECT_TRUE("begin-rejects-header-only",
                !q.begin(EncodedPacketQueue::overheadPerPacket()));
    EXPECT_TRUE("begin-accepts-header-plus-one",
                q.begin(EncodedPacketQueue::overheadPerPacket() + 1));
    EXPECT_TRUE("ready", q.isReady());
    EXPECT_TRUE("starts-empty", q.isEmpty());
    EXPECT_EQ("free-equals-capacity", q.capacityBytes(), q.freeBytes());
}

static void test_roundtrip()
{
    EncodedPacketQueue q;
    q.begin(1024);

    uint8_t payload[64];
    fill_pattern(payload, sizeof(payload), 0x11);
    EXPECT_TRUE("push", q.push(payload, sizeof(payload), 3, 0xDEADBEEF));
    EXPECT_EQ("count-after-push", 1, q.packetCount());
    EXPECT_EQ("used-after-push",
              sizeof(payload) + EncodedPacketQueue::overheadPerPacket(),
              q.usedBytes());

    EncodedPacketQueue::PacketHeader peeked;
    EXPECT_TRUE("peek", q.peek(peeked));
    EXPECT_EQ("peek-length", sizeof(payload), peeked.length);
    EXPECT_EQ("peek-frames", 3, peeked.frameCount);
    EXPECT_TRUE("peek-timestamp", peeked.timestamp == 0xDEADBEEF);
    EXPECT_EQ("peek-does-not-consume", 1, q.packetCount());

    EncodedPacketQueue::PacketHeader header;
    uint8_t out[64];
    memset(out, 0, sizeof(out));
    EXPECT_EQ("pop-returns-length", sizeof(payload),
              q.pop(header, out, sizeof(out)));
    EXPECT_TRUE("pop-payload-intact", check_pattern(out, sizeof(out), 0x11));
    EXPECT_EQ("pop-frames", 3, header.frameCount);
    EXPECT_TRUE("pop-timestamp", header.timestamp == 0xDEADBEEF);
    EXPECT_TRUE("empty-after-pop", q.isEmpty());
    EXPECT_EQ("pop-empty-returns-zero", 0, q.pop(header, out, sizeof(out)));
}

static void test_rejects_invalid_push()
{
    EncodedPacketQueue q;
    q.begin(256);

    uint8_t payload[8];
    fill_pattern(payload, sizeof(payload), 0x22);
    EXPECT_TRUE("rejects-null", !q.push(nullptr, sizeof(payload), 1, 0));
    EXPECT_TRUE("rejects-zero-length", !q.push(payload, 0, 1, 0));
    EXPECT_TRUE("rejects-over-max",
                !q.push(payload, EncodedPacketQueue::maxPacketBytes() + 1, 1, 0));
    EXPECT_TRUE("still-empty", q.isEmpty());
}

// The core invariant: an overflowing packet is dropped whole, and the
// packets already stored are untouched.
static void test_whole_packet_or_nothing()
{
    EncodedPacketQueue q;
    const size_t payloadBytes = 32;
    const size_t perPacket = payloadBytes + EncodedPacketQueue::overheadPerPacket();
    q.begin(perPacket * 2);

    uint8_t payload[32];
    fill_pattern(payload, sizeof(payload), 0x33);
    EXPECT_TRUE("fits-first", q.push(payload, sizeof(payload), 1, 100));
    EXPECT_TRUE("fits-second", q.push(payload, sizeof(payload), 1, 200));
    EXPECT_EQ("queue-full", 0, q.freeBytes());

    fill_pattern(payload, sizeof(payload), 0x44);
    EXPECT_TRUE("third-rejected", !q.push(payload, sizeof(payload), 1, 300));
    EXPECT_EQ("count-unchanged", 2, q.packetCount());
    EXPECT_EQ("used-unchanged", perPacket * 2, q.usedBytes());

    // A packet one byte too large for the free space is rejected outright,
    // not truncated.
    EncodedPacketQueue::PacketHeader header;
    uint8_t out[32];
    q.pop(header, out, sizeof(out));
    EXPECT_TRUE("freed-one-slot", q.freeBytes() == perPacket);
    EXPECT_TRUE("oversize-by-one-rejected",
                !q.push(payload, payloadBytes + 1, 1, 400));
    EXPECT_EQ("count-still-one", 1, q.packetCount());

    // The survivor is the one that was pushed second, unmodified.
    EXPECT_EQ("survivor-pops", payloadBytes, q.pop(header, out, sizeof(out)));
    EXPECT_TRUE("survivor-timestamp", header.timestamp == 200);
    EXPECT_TRUE("survivor-payload", check_pattern(out, payloadBytes, 0x33));
}

// Push and pop enough packets that records repeatedly straddle the end of
// the ring. A record split across the wrap must come back byte-identical.
static void test_ring_wrap()
{
    EncodedPacketQueue q;
    q.begin(200);

    uint8_t payload[57];  // coprime-ish with the capacity, so offsets drift
    uint8_t out[57];
    EncodedPacketQueue::PacketHeader header;

    bool allOk = true;
    for (int i = 0; i < 200; ++i) {
        const uint8_t seed = (uint8_t)(i * 13u + 1u);
        fill_pattern(payload, sizeof(payload), seed);
        if (!q.push(payload, sizeof(payload), (uint16_t)i, (uint32_t)(1000 + i))) {
            allOk = false;
            break;
        }
        memset(out, 0, sizeof(out));
        if (q.pop(header, out, sizeof(out)) != sizeof(payload)) { allOk = false; break; }
        if (!check_pattern(out, sizeof(out), seed)) { allOk = false; break; }
        if (header.frameCount != (uint16_t)i) { allOk = false; break; }
        if (header.timestamp != (uint32_t)(1000 + i)) { allOk = false; break; }
    }
    EXPECT_TRUE("wrap-roundtrip-200", allOk);
    EXPECT_TRUE("wrap-empty-at-end", q.isEmpty());

    // Same again, but with two packets in flight so a record's header and
    // payload land on opposite sides of the wrap.
    allOk = true;
    for (int i = 0; i < 200; ++i) {
        const uint8_t seed = (uint8_t)(i * 29u + 5u);
        fill_pattern(payload, sizeof(payload), seed);
        if (!q.push(payload, sizeof(payload), 1, (uint32_t)i)) { allOk = false; break; }
        if (i == 0) continue;  // keep one queued
        memset(out, 0, sizeof(out));
        if (q.pop(header, out, sizeof(out)) != sizeof(payload)) { allOk = false; break; }
        const uint8_t expectedSeed = (uint8_t)((i - 1) * 29u + 5u);
        if (!check_pattern(out, sizeof(out), expectedSeed)) { allOk = false; break; }
    }
    EXPECT_TRUE("wrap-pipelined", allOk);
}

static void test_pop_buffer_too_small()
{
    EncodedPacketQueue q;
    q.begin(256);

    uint8_t payload[40];
    fill_pattern(payload, sizeof(payload), 0x55);
    q.push(payload, sizeof(payload), 2, 77);

    EncodedPacketQueue::PacketHeader header;
    uint8_t small[39];
    EXPECT_EQ("too-small-returns-zero", 0, q.pop(header, small, sizeof(small)));
    EXPECT_EQ("too-small-keeps-packet", 1, q.packetCount());

    uint8_t exact[40];
    EXPECT_EQ("exact-size-works", sizeof(payload),
              q.pop(header, exact, sizeof(exact)));
    EXPECT_TRUE("payload-still-intact", check_pattern(exact, sizeof(exact), 0x55));
}

static void test_drop_and_clear()
{
    EncodedPacketQueue q;
    q.begin(512);

    uint8_t payload[16];
    fill_pattern(payload, sizeof(payload), 0x66);
    q.push(payload, sizeof(payload), 1, 1);
    q.push(payload, sizeof(payload), 1, 2);
    q.push(payload, sizeof(payload), 1, 3);
    EXPECT_EQ("three-queued", 3, q.packetCount());

    EXPECT_TRUE("drop", q.drop());
    EXPECT_EQ("two-left", 2, q.packetCount());

    EncodedPacketQueue::PacketHeader header;
    EXPECT_TRUE("drop-removed-oldest", q.peek(header) && header.timestamp == 2);

    q.clear();
    EXPECT_TRUE("clear-empties", q.isEmpty());
    EXPECT_EQ("clear-frees-all", q.capacityBytes(), q.freeBytes());
    EXPECT_TRUE("drop-on-empty", !q.drop());

    // Usable again after clear().
    EXPECT_TRUE("push-after-clear", q.push(payload, sizeof(payload), 1, 9));
    EXPECT_TRUE("peek-after-clear", q.peek(header) && header.timestamp == 9);
}

// SPEC.md §7: the queue must hold one mediaMtu-sized packet atomically.
// 995 bytes is the MTU measured against a real A2DP Source (SPEC.md §5.1).
static void test_media_mtu_lower_bound()
{
    const size_t mediaMtu = 995;
    const size_t minimum = mediaMtu + EncodedPacketQueue::overheadPerPacket();

    EncodedPacketQueue q;
    q.begin(minimum);

    static uint8_t payload[995];
    static uint8_t out[995];
    fill_pattern(payload, sizeof(payload), 0x77);
    EXPECT_TRUE("mtu-packet-fits", q.push(payload, mediaMtu, 5, 12345));
    EXPECT_EQ("mtu-packet-fills-queue", 0, q.freeBytes());

    EncodedPacketQueue::PacketHeader header;
    EXPECT_EQ("mtu-packet-pops", mediaMtu, q.pop(header, out, sizeof(out)));
    EXPECT_TRUE("mtu-payload-intact", check_pattern(out, mediaMtu, 0x77));

    // One byte short of the documented minimum cannot hold it.
    EncodedPacketQueue tooSmall;
    tooSmall.begin(minimum - 1);
    EXPECT_TRUE("below-minimum-rejects", !tooSmall.push(payload, mediaMtu, 5, 0));
}

static void test_end_releases()
{
    EncodedPacketQueue q;
    q.begin(128);

    uint8_t payload[8];
    fill_pattern(payload, sizeof(payload), 0x88);
    q.push(payload, sizeof(payload), 1, 1);

    q.end();
    EXPECT_TRUE("not-ready-after-end", !q.isReady());
    EXPECT_TRUE("push-after-end-fails", !q.push(payload, sizeof(payload), 1, 1));

    EncodedPacketQueue::PacketHeader header;
    EXPECT_TRUE("peek-after-end-fails", !q.peek(header));
    EXPECT_TRUE("begin-again", q.begin(128));
    EXPECT_TRUE("empty-after-rebegin", q.isEmpty());
}

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("TEST start encoded_queue");

    test_begin_validation();
    test_roundtrip();
    test_rejects_invalid_push();
    test_whole_packet_or_nothing();
    test_ring_wrap();
    test_pop_buffer_too_small();
    test_drop_and_clear();
    test_media_mtu_lower_bound();
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
