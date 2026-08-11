#include "A2dpSinkStream.h"

#include <stdlib.h>
#include <string.h>

namespace
{
// A provisional layout so the PCM ring can be sized before the codec has
// been negotiated. Stereo is the worst case, so the buffer never has to grow
// later — a mono configuration only ever shrinks it.
PCMFormat provisionalFormat()
{
    PCMFormat format;
    format.sampleRate = 48000;
    format.channels = 2;
    format.bitsPerSample = 16;
    return format;
}
} // namespace

// Enforces the single-consumer rule in update(). Taking the flag with an
// exchange means a second caller is refused rather than silently sharing the
// decoder context with the first.
class A2dpSinkStream::UpdateGuard
{
public:
    explicit UpdateGuard(std::atomic<bool> &flag) : flag_(flag)
    {
        held_ = !flag_.exchange(true, std::memory_order_acquire);
    }
    ~UpdateGuard()
    {
        if (held_) flag_.store(false, std::memory_order_release);
    }
    bool held() const { return held_; }

private:
    std::atomic<bool> &flag_;
    bool held_ = false;
};

bool A2dpSinkStream::begin()
{
    return begin(Config());
}

bool A2dpSinkStream::begin(const Config &config)
{
    end();

    // A begin() starts a new lifetime, so the counters start from zero. This
    // is deliberately different from reset(), which continues the same
    // session after a stream stop and keeps its diagnostics intact.
    resetCounters();

    if (config.maximumPacketBytes == 0 ||
        config.maximumPacketBytes > EncodedPacketQueue::maxPacketBytes() ||
        config.pcmQueueFrames == 0)
    {
        lastError_ = PCMFlowBluetoothError::InvalidConfiguration;
        return false;
    }

    // The queue must hold one maximum-size packet whole, or the transport's
    // largest packets would be dropped every time (SPEC.md §7).
    const size_t minimumQueue =
        config.maximumPacketBytes + EncodedPacketQueue::overheadPerPacket();
    if (config.encodedQueueBytes < minimumQueue)
    {
        lastError_ = PCMFlowBluetoothError::InvalidConfiguration;
        return false;
    }

    config_ = config;

    if (!encodedQueue_.begin(config_.encodedQueueBytes) ||
        !decoder_.begin() ||
        !allocatePcmQueue(provisionalFormat()))
    {
        end();
        lastError_ = PCMFlowBluetoothError::AllocationFailed;
        return false;
    }

    packetScratch_ = static_cast<uint8_t *>(malloc(config_.maximumPacketBytes));
    pcmScratch_ = static_cast<int16_t *>(
        malloc(SbcDecoder::maxPcmFramesPerSbcFrame() * 2 * sizeof(int16_t)));
    if (packetScratch_ == nullptr || pcmScratch_ == nullptr)
    {
        end();
        lastError_ = PCMFlowBluetoothError::AllocationFailed;
        return false;
    }

    started_ = true;
    lastError_ = PCMFlowBluetoothError::None;
    return true;
}

void A2dpSinkStream::end()
{
    encodedQueue_.end();
    pcmQueue_.end();
    decoder_.end();

    free(packetScratch_);
    packetScratch_ = nullptr;
    free(pcmScratch_);
    pcmScratch_ = nullptr;

    started_ = false;
    codecConfig_ = EncodedAudioFormat();
    format_ = PCMFormat();
    lastTimestamp_ = 0;
    haveTimestamp_ = false;
    updating_.store(false, std::memory_order_release);
}

bool A2dpSinkStream::allocatePcmQueue(const PCMFormat &format)
{
    return pcmQueue_.begin(format, config_.pcmQueueFrames);
}

bool A2dpSinkStream::isReady() const
{
    return started_ && format_.isValid() && pcmQueue_.isReady();
}

bool A2dpSinkStream::pushEncoded(const uint8_t *data, size_t length,
                                 uint16_t frameCount, uint32_t timestamp)
{
    if (!started_)
    {
        lastError_ = PCMFlowBluetoothError::NotInitialized;
        return false;
    }

    ++receivedPackets_;

    // Ordering only. The timestamp's unit belongs to the Source and is never
    // interpreted here (SPEC.md §5); a value that does not advance means the
    // stream jumped, restarted, or wrapped.
    if (haveTimestamp_ && timestamp <= lastTimestamp_) ++timestampDiscontinuities_;
    lastTimestamp_ = timestamp;
    haveTimestamp_ = true;

    if (!encodedQueue_.push(data, length, frameCount, timestamp))
    {
        ++droppedPackets_;
        lastError_ = PCMFlowBluetoothError::QueueOverflow;
        return false;
    }
    return true;
}

void A2dpSinkStream::setCodecConfig(const EncodedAudioFormat &format)
{
    if (!started_)
    {
        lastError_ = PCMFlowBluetoothError::NotInitialized;
        return;
    }

    if (format.codec != EncodedAudioCodec::Sbc)
    {
        // mSBC and CVSD arrive with the HFP phases; anything else is not a
        // codec this build knows. Leave the stream unconfigured rather than
        // decoding a stream as something it is not.
        codecConfig_ = format;
        format_ = PCMFormat();
        reset();
        lastError_ = PCMFlowBluetoothError::UnsupportedCodec;
        return;
    }

    if (format == codecConfig_ && format_.isValid()) return;

    codecConfig_ = format;

    PCMFormat pcm;
    pcm.sampleRate = format.sampleRate;
    pcm.channels = (format.channels == 1) ? 1 : 2;
    pcm.bitsPerSample = 16;

    if (!pcm.isValid())
    {
        format_ = PCMFormat();
        reset();
        lastError_ = PCMFlowBluetoothError::InvalidConfiguration;
        return;
    }

    // The frame size changed with the channel count, so the ring has to be
    // rebuilt. Control path only — steady-state decoding allocates nothing.
    if (pcm.bytesPerFrame() != pcmQueue_.bytesPerFrame())
    {
        if (!allocatePcmQueue(pcm))
        {
            format_ = PCMFormat();
            lastError_ = PCMFlowBluetoothError::AllocationFailed;
            return;
        }
    }

    format_ = pcm;
    reset();
    lastError_ = PCMFlowBluetoothError::None;
}

void A2dpSinkStream::reset()
{
    encodedQueue_.clear();
    pcmQueue_.clear();
    decoder_.reset(format_.isValid() ? format_.channels : 2);
    haveTimestamp_ = false;
    lastTimestamp_ = 0;
}

void A2dpSinkStream::resetCounters()
{
    receivedPackets_ = 0;
    droppedPackets_ = 0;
    invalidFrames_ = 0;
    decodeFailures_ = 0;
    decodedFrames_ = 0;
    pcmOverflowFrames_ = 0;
    timestampDiscontinuities_ = 0;
    lastError_ = PCMFlowBluetoothError::None;
}

size_t A2dpSinkStream::writePcm(const int16_t *pcm, size_t frames)
{
    if (frames == 0) return 0;

    if (pcmQueue_.freeFrames() < frames)
    {
        if (config_.pcmOverflowPolicy == PcmOverflowPolicy::DropOldest)
        {
            // Make room by discarding the oldest frames. Reading them into
            // the decode scratch is the cheapest way to advance the ring
            // without another buffer; it is sized for a whole SBC frame,
            // which is the most we ever need to drop in one go.
            const size_t scratchFrames = SbcDecoder::maxPcmFramesPerSbcFrame();
            size_t needed = frames - pcmQueue_.freeFrames();
            while (needed > 0)
            {
                const size_t chunk = (needed < scratchFrames) ? needed : scratchFrames;
                const size_t dropped = pcmQueue_.readFrames(pcmScratch_, chunk);
                if (dropped == 0) break;
                pcmOverflowFrames_ += static_cast<uint32_t>(dropped);
                needed -= dropped;
            }
            lastError_ = PCMFlowBluetoothError::PcmOverflow;
        }
        else
        {
            // DropNewest: keep what is buffered and lose the tail of this
            // frame instead.
            const size_t room = pcmQueue_.freeFrames();
            pcmOverflowFrames_ += static_cast<uint32_t>(frames - room);
            lastError_ = PCMFlowBluetoothError::PcmOverflow;
            frames = room;
            if (frames == 0) return 0;
        }
    }

    return pcmQueue_.writeFrames(pcm, frames);
}

void A2dpSinkStream::update()
{
    if (!started_)
    {
        lastError_ = PCMFlowBluetoothError::NotInitialized;
        return;
    }

    UpdateGuard guard(updating_);
    if (!guard.held())
    {
        lastError_ = PCMFlowBluetoothError::ConcurrentUpdate;
        return;
    }

    if (!format_.isValid()) return;  // nothing negotiated yet

    EncodedPacketQueue::PacketHeader header;
    while (encodedQueue_.pop(header, packetScratch_, config_.maximumPacketBytes) > 0)
    {
        size_t offset = 0;
        while (offset < header.length)
        {
            size_t consumed = 0;
            size_t written = 0;
            const bool ok = decoder_.decodeFrame(
                packetScratch_ + offset, header.length - offset, consumed,
                pcmScratch_, SbcDecoder::maxPcmFramesPerSbcFrame(), written);

            if (ok)
            {
                ++decodedFrames_;
                writePcm(pcmScratch_, written);

                // The bitstream is authoritative for the sample rate: a
                // Source may reconfigure without the negotiation being
                // replayed. The channel count is fixed by reset() and is
                // not taken from the frame.
                const PCMFormat &decoded = decoder_.format();
                if (decoded.isValid() && decoded.sampleRate != format_.sampleRate)
                {
                    format_.sampleRate = decoded.sampleRate;
                }
            }
            else
            {
                ++invalidFrames_;
                lastError_ = PCMFlowBluetoothError::InvalidFrame;
                if (consumed == 0)
                {
                    // The packet ends mid-frame. A2DP packets carry whole
                    // frames, so this is a malformed packet rather than a
                    // stream that needs more data — the next packet starts
                    // a new frame and waiting would stall the queue.
                    ++decodeFailures_;
                    lastError_ = PCMFlowBluetoothError::DecodeFailure;
                    break;
                }
            }
            offset += consumed;
        }
    }
}

size_t A2dpSinkStream::readFrames(void *out, size_t frameCount)
{
    if (!started_ || out == nullptr) return 0;
    return pcmQueue_.readFrames(out, frameCount);
}
