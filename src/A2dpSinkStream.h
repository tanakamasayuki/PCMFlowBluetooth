#ifndef PCMFLOWBLUETOOTH_A2DPSINKSTREAM_H
#define PCMFLOWBLUETOOTH_A2DPSINKSTREAM_H

#include <stdint.h>
#include <stddef.h>

#include <atomic>

#include <PCMSource.h>
#include <PCMRingBuffer.h>

#include "EncodedAudioFormat.h"
#include "EncodedPacketQueue.h"
#include "PCMFlowBluetoothError.h"
#include "SbcDecoder.h"

// What to do with PCM that the consumer has not collected in time.
enum class PcmOverflowPolicy : uint8_t
{
    // Default. Discard the oldest PCM so playback stays close to live. A
    // consumer that stalls briefly resumes at the current position rather
    // than working through a backlog.
    DropOldest,
    // Discard the newest PCM instead, keeping what is already buffered
    // contiguous. Prefer this when an unbroken stretch matters more than
    // latency.
    DropNewest,
};

// The core of the library: encoded packets in, PCM out.
//
// Knows nothing about EspBle. Packets arrive through pushEncoded(), which is
// safe to call from a Bluetooth callback context; decoding happens later, in
// update(), on the caller's own task. That split is the whole point — the
// Bluetooth host callback must never block, and decoding is far too slow to
// run there (SPEC.md §6).
//
// Being EspBle-independent is also what makes this class testable on a host:
// everything interesting — queue overflow, codec reconfiguration, reset,
// malformed frames — is reachable without a Bluetooth stack.
class A2dpSinkStream : public PCMSource
{
public:
    struct Config
    {
        // Encoded packets waiting to be decoded. Must hold at least one
        // maximumPacketBytes packet whole; see SPEC.md §7.
        size_t encodedQueueBytes = 8192;

        // Decoded PCM waiting to be read. 4,096 frames is about 85 ms at
        // 48 kHz stereo.
        size_t pcmQueueFrames = 4096;

        // Largest single payload the transport can deliver. The A2DP media
        // MTU measured against a real Source is 995 bytes (SPEC.md §5.1);
        // the default leaves headroom. EspBleA2dpSinkAdapter sets this from
        // the connection's actual MTU.
        size_t maximumPacketBytes = 1024;

        PcmOverflowPolicy pcmOverflowPolicy = PcmOverflowPolicy::DropOldest;
    };

    A2dpSinkStream() = default;
    ~A2dpSinkStream() override { end(); }

    A2dpSinkStream(const A2dpSinkStream &) = delete;
    A2dpSinkStream &operator=(const A2dpSinkStream &) = delete;

    // Allocate every buffer this stream will use. Returns false, allocating
    // nothing, if the configuration is inconsistent or memory runs out.
    //
    // Two overloads rather than a defaulted argument: `Config()` cannot be a
    // default argument here, because Config's own member initializers are
    // not complete until the enclosing class is.
    bool begin(const Config &config);
    bool begin();

    // Release everything. Safe to call twice.
    void end();

    // --- Producer side ---------------------------------------------------

    // Copy one encoded packet into the queue. Safe to call from a Bluetooth
    // callback: it copies, updates counters, and returns. It never decodes,
    // never allocates and never blocks.
    //
    // Returns false when the packet did not fit, in which case the whole
    // packet is dropped and droppedPacketCount() advances. A partial packet
    // is never stored — the SBC frame stream could not be resynchronized
    // cheaply from one.
    bool pushEncoded(const uint8_t *data, size_t length,
                     uint16_t frameCount, uint32_t timestamp);

    // --- Control side ----------------------------------------------------

    // Apply a negotiated codec configuration. When it differs from the
    // current one, everything buffered belongs to the old stream and is
    // discarded, and the decoder is reinitialized.
    //
    // This settles format(), so isReady() becomes true here rather than
    // after the first decoded frame.
    //
    // May reallocate the PCM ring, because the frame size depends on the
    // channel count. That is a control-path event, not steady state.
    void setCodecConfig(const EncodedAudioFormat &format);

    const EncodedAudioFormat &codecConfig() const { return codecConfig_; }

    // The configuration begin() was given. Useful for adjusting one field
    // and starting over — which is what the EspBle adapter does when the
    // negotiated media MTU turns out larger than the configured maximum.
    const Config &config() const { return config_; }

    // Discard all buffered data and reset the decoder, keeping the current
    // codec configuration. Call it when a stream stops or a peer
    // disconnects, so nothing from the old session survives into the next.
    void reset();

    // --- Consumer side ---------------------------------------------------

    // Decode as much of the encoded queue as the PCM queue can take.
    //
    // Single consumer: concurrent calls are refused rather than serialized,
    // because two decoders sharing one context would corrupt it silently.
    // A refused call sets lastError() to ConcurrentUpdate.
    void update();

    size_t availableFrames() const { return pcmQueue_.availableFrames(); }

    // PCMSource
    const PCMFormat &format() const override { return format_; }
    size_t readFrames(void *out, size_t frameCount) override;
    bool isEof() const override { return false; }  // SPEC.md §4.5
    bool isReady() const override;

    // --- Observation -----------------------------------------------------
    //
    // Monotonically increasing and allowed to wrap. resetCounters() clears
    // them without disturbing the stream.

    uint32_t receivedPacketCount() const { return receivedPackets_; }
    uint32_t droppedPacketCount() const { return droppedPackets_; }
    uint32_t invalidFrameCount() const { return invalidFrames_; }
    uint32_t decodeFailureCount() const { return decodeFailures_; }
    uint32_t decodedFrameCount() const { return decodedFrames_; }
    uint32_t pcmOverflowFrameCount() const { return pcmOverflowFrames_; }
    uint32_t timestampDiscontinuityCount() const { return timestampDiscontinuities_; }
    void resetCounters();

    PCMFlowBluetoothError lastError() const { return lastError_; }
    const char *lastErrorName() const { return toString(lastError_); }

private:
    // Guard object for the single-consumer rule in update().
    class UpdateGuard;

    bool allocatePcmQueue(const PCMFormat &format);
    size_t writePcm(const int16_t *pcm, size_t frames);

    Config config_;
    bool started_ = false;

    EncodedPacketQueue encodedQueue_;
    PCMRingBuffer pcmQueue_;
    SbcDecoder decoder_;

    uint8_t *packetScratch_ = nullptr;   // one encoded packet, for update()
    int16_t *pcmScratch_ = nullptr;      // one decoded frame, for update()

    EncodedAudioFormat codecConfig_;
    PCMFormat format_;

    // Producer-side timestamp tracking. Only ordering matters; the unit is
    // the Source's business (SPEC.md §5).
    uint32_t lastTimestamp_ = 0;
    bool haveTimestamp_ = false;

    std::atomic<bool> updating_{false};

    uint32_t receivedPackets_ = 0;
    uint32_t droppedPackets_ = 0;
    uint32_t invalidFrames_ = 0;
    uint32_t decodeFailures_ = 0;
    uint32_t decodedFrames_ = 0;
    uint32_t pcmOverflowFrames_ = 0;
    uint32_t timestampDiscontinuities_ = 0;

    PCMFlowBluetoothError lastError_ = PCMFlowBluetoothError::None;
};

#endif // PCMFLOWBLUETOOTH_A2DPSINKSTREAM_H
