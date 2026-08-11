#ifndef PCMFLOWBLUETOOTH_ENCODEDPACKETQUEUE_H
#define PCMFLOWBLUETOOTH_ENCODEDPACKETQUEUE_H

#include <stdint.h>
#include <stddef.h>

#include <atomic>

// Fixed-capacity SPSC queue of encoded audio packets.
//
// The producer is the Bluetooth media callback, which may run in a Bluedroid
// task context: push() only copies and advances an index, and never blocks,
// allocates, or reallocates. The consumer is A2dpSinkStream::update().
//
// Packets are stored as length-prefixed records in one byte ring, so a
// packet is either stored whole or not at all — a truncated packet would
// desynchronize the SBC frame stream, which is exactly what the decoder
// cannot recover from cheaply. push() therefore rejects a packet that does
// not fit rather than storing part of it (SPEC.md §7).
//
// Records may straddle the end of the buffer; there is no padding and no
// wasted tail. The header is written and read through the same wrap-aware
// copy as the payload.
class EncodedPacketQueue
{
public:
    // Per-packet metadata, carried alongside the payload.
    struct PacketHeader
    {
        uint16_t length = 0;      // payload bytes
        uint16_t frameCount = 0;  // codec frames the payload contains
        uint32_t timestamp = 0;   // opaque transport timestamp (SPEC.md §5)
    };

    // Bytes of bookkeeping each stored packet costs on top of its payload.
    static constexpr size_t overheadPerPacket() { return kHeaderBytes; }

    // Largest payload the record format can express.
    static constexpr size_t maxPacketBytes() { return UINT16_MAX; }

    EncodedPacketQueue() = default;
    ~EncodedPacketQueue() { end(); }

    EncodedPacketQueue(const EncodedPacketQueue &) = delete;
    EncodedPacketQueue &operator=(const EncodedPacketQueue &) = delete;

    // Allocate `capacityBytes` of storage. Returns false if the capacity is
    // too small to hold a single one-byte packet, or if allocation fails.
    bool begin(size_t capacityBytes);

    // Free the storage.
    void end();

    // Drop every stored packet without freeing memory.
    //
    // Consumer-side only. Calling it concurrently with push() would race,
    // so callers must have stopped the producer first — which is what
    // A2dpSinkStream::reset() does when a stream stops, a peer disconnects,
    // or the codec is reconfigured.
    void clear();

    // --- Producer side -------------------------------------------------

    // Copy one packet in. Returns false, storing nothing, when the payload
    // is empty, larger than maxPacketBytes(), or does not fit in the space
    // currently free.
    bool push(const uint8_t *data, size_t length,
              uint16_t frameCount, uint32_t timestamp);

    // --- Consumer side -------------------------------------------------

    // Read the header of the oldest packet without removing it. Returns
    // false when the queue is empty.
    bool peek(PacketHeader &header) const;

    // Remove the oldest packet, copying its payload into `out`.
    //
    // Returns the payload byte count, or 0 when the queue is empty. When
    // `outCapacity` is too small the packet is left in place and 0 is
    // returned, so a caller with a fixed buffer can peek() first and grow
    // rather than losing data silently.
    size_t pop(PacketHeader &header, uint8_t *out, size_t outCapacity);

    // Discard the oldest packet without copying it. Returns false when the
    // queue is empty.
    bool drop();

    // --- Observation ---------------------------------------------------
    //
    // Safe from either side, though the value is a snapshot: the other side
    // may move it before the caller acts on it.

    size_t capacityBytes() const { return capacity_; }
    size_t usedBytes() const;
    size_t freeBytes() const { return capacity_ - usedBytes(); }
    size_t packetCount() const { return packets_.load(std::memory_order_acquire); }
    bool isEmpty() const { return packetCount() == 0; }
    bool isReady() const { return buffer_ != nullptr; }

private:
    static constexpr size_t kHeaderBytes = 8;  // length + frameCount + timestamp

    void writeAt(size_t offset, const void *src, size_t length);
    void readAt(size_t offset, void *dst, size_t length) const;

    uint8_t *buffer_ = nullptr;
    size_t capacity_ = 0;

    // Monotonically increasing byte counters, indexed modulo capacity_.
    // Using counters rather than raw indices keeps "full" and "empty"
    // distinguishable without wasting a slot.
    std::atomic<size_t> writePos_{0};
    std::atomic<size_t> readPos_{0};
    std::atomic<size_t> packets_{0};
};

#endif // PCMFLOWBLUETOOTH_ENCODEDPACKETQUEUE_H
