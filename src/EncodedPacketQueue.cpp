#include "EncodedPacketQueue.h"

#include <string.h>
#include <stdlib.h>

bool EncodedPacketQueue::begin(size_t capacityBytes)
{
    end();

    // The smallest useful queue holds one header plus one payload byte.
    if (capacityBytes <= kHeaderBytes) return false;

    buffer_ = static_cast<uint8_t *>(malloc(capacityBytes));
    if (buffer_ == nullptr) return false;

    capacity_ = capacityBytes;
    writePos_.store(0, std::memory_order_relaxed);
    readPos_.store(0, std::memory_order_relaxed);
    packets_.store(0, std::memory_order_relaxed);
    return true;
}

void EncodedPacketQueue::end()
{
    free(buffer_);
    buffer_ = nullptr;
    capacity_ = 0;
    writePos_.store(0, std::memory_order_relaxed);
    readPos_.store(0, std::memory_order_relaxed);
    packets_.store(0, std::memory_order_relaxed);
}

void EncodedPacketQueue::clear()
{
    // Consumer-side: catching the read counter up to the write counter
    // discards everything currently stored. Anything the producer adds
    // afterwards survives, which is what callers want — reset() runs from
    // the control path, and a packet that arrives after it belongs to the
    // new stream.
    readPos_.store(writePos_.load(std::memory_order_acquire),
                   std::memory_order_release);
    packets_.store(0, std::memory_order_release);
}

size_t EncodedPacketQueue::usedBytes() const
{
    // Both counters increase without bound and the difference is always
    // within capacity_, so unsigned wraparound cancels out.
    return writePos_.load(std::memory_order_acquire) -
           readPos_.load(std::memory_order_acquire);
}

void EncodedPacketQueue::writeAt(size_t offset, const void *src, size_t length)
{
    const size_t index = offset % capacity_;
    const size_t firstChunk = capacity_ - index;
    if (length <= firstChunk)
    {
        memcpy(buffer_ + index, src, length);
        return;
    }
    memcpy(buffer_ + index, src, firstChunk);
    memcpy(buffer_, static_cast<const uint8_t *>(src) + firstChunk, length - firstChunk);
}

void EncodedPacketQueue::readAt(size_t offset, void *dst, size_t length) const
{
    const size_t index = offset % capacity_;
    const size_t firstChunk = capacity_ - index;
    if (length <= firstChunk)
    {
        memcpy(dst, buffer_ + index, length);
        return;
    }
    memcpy(dst, buffer_ + index, firstChunk);
    memcpy(static_cast<uint8_t *>(dst) + firstChunk, buffer_, length - firstChunk);
}

bool EncodedPacketQueue::push(const uint8_t *data, size_t length,
                              uint16_t frameCount, uint32_t timestamp)
{
    if (buffer_ == nullptr || data == nullptr) return false;
    if (length == 0 || length > maxPacketBytes()) return false;

    const size_t needed = kHeaderBytes + length;
    const size_t write = writePos_.load(std::memory_order_relaxed);
    const size_t used = write - readPos_.load(std::memory_order_acquire);
    if (needed > capacity_ - used) return false;  // whole packet or nothing

    uint8_t header[kHeaderBytes];
    const uint16_t storedLength = static_cast<uint16_t>(length);
    memcpy(header + 0, &storedLength, sizeof(storedLength));
    memcpy(header + 2, &frameCount, sizeof(frameCount));
    memcpy(header + 4, &timestamp, sizeof(timestamp));

    writeAt(write, header, kHeaderBytes);
    writeAt(write + kHeaderBytes, data, length);

    // Publishing the new write position releases both writes above, so the
    // consumer never sees a header without its payload.
    writePos_.store(write + needed, std::memory_order_release);
    packets_.fetch_add(1, std::memory_order_release);
    return true;
}

bool EncodedPacketQueue::peek(PacketHeader &header) const
{
    if (buffer_ == nullptr) return false;

    const size_t read = readPos_.load(std::memory_order_relaxed);
    if (writePos_.load(std::memory_order_acquire) - read < kHeaderBytes) return false;

    uint8_t raw[kHeaderBytes];
    readAt(read, raw, kHeaderBytes);
    memcpy(&header.length, raw + 0, sizeof(header.length));
    memcpy(&header.frameCount, raw + 2, sizeof(header.frameCount));
    memcpy(&header.timestamp, raw + 4, sizeof(header.timestamp));
    return true;
}

size_t EncodedPacketQueue::pop(PacketHeader &header, uint8_t *out, size_t outCapacity)
{
    if (out == nullptr) return 0;
    if (!peek(header)) return 0;
    if (header.length > outCapacity) return 0;  // leave it in place

    const size_t read = readPos_.load(std::memory_order_relaxed);
    readAt(read + kHeaderBytes, out, header.length);

    readPos_.store(read + kHeaderBytes + header.length, std::memory_order_release);
    packets_.fetch_sub(1, std::memory_order_release);
    return header.length;
}

bool EncodedPacketQueue::drop()
{
    PacketHeader header;
    if (!peek(header)) return false;

    const size_t read = readPos_.load(std::memory_order_relaxed);
    readPos_.store(read + kHeaderBytes + header.length, std::memory_order_release);
    packets_.fetch_sub(1, std::memory_order_release);
    return true;
}
