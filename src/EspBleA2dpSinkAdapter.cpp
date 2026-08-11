#include "EspBleA2dpSinkAdapter.h"

#if PCMFLOWBLUETOOTH_ESPBLE_ADAPTER_AVAILABLE

EncodedAudioFormat EspBleA2dpSinkAdapter::toEncodedAudioFormat(
    const EspBleClassicA2dpCodecConfig &config)
{
    EncodedAudioFormat out;
    switch (config.codec)
    {
    case EspBleClassicAudioCodec::Sbc:  out.codec = EncodedAudioCodec::Sbc;  break;
    case EspBleClassicAudioCodec::Msbc: out.codec = EncodedAudioCodec::Msbc; break;
    case EspBleClassicAudioCodec::Cvsd: out.codec = EncodedAudioCodec::Cvsd; break;
    default:                            out.codec = EncodedAudioCodec::Unknown; break;
    }
    out.sampleRate = config.sampleRate;
    out.channels = config.channels;
    out.minimumBitpool = config.minimumBitpool;
    out.maximumBitpool = config.maximumBitpool;
    return out;
}

bool EspBleA2dpSinkAdapter::begin(EspBleClassicA2dpSink &transport)
{
    return begin(transport, A2dpSinkStream::Config());
}

bool EspBleA2dpSinkAdapter::begin(EspBleClassicA2dpSink &transport,
                                  const A2dpSinkStream::Config &config)
{
    end();

    if (!stream_.begin(config)) return false;

    transport_ = &transport;

    transport.onCodecConfigured([this](const EspBleClassicA2dpCodecConfig &c) {
        onCodecConfigured(c);
    });
    transport.onStreamStateChanged([this](const EspBleClassicA2dpStreamEvent &e) {
        onStreamStateChanged(e);
    });
    transport.onConnected([this](const EspBleClassicA2dpConnection &c) {
        onConnected(c);
    });
    transport.onDisconnected([this](const EspBleClassicA2dpConnection &c) {
        onDisconnected(c);
    });

    // Registered last: once this is in place the Bluetooth task can call
    // into the stream, so everything it touches has to be ready first.
    transport.onMedia([this](const EspBleClassicEncodedAudioView &v) {
        onMedia(v);
    });

    // EspBle may have connected and negotiated before this adapter existed —
    // control events are not replayed, so ask for the current state rather
    // than waiting for an event that has already been and gone.
    if (transport.connected())
    {
        onConnected(transport.connection());
        const EspBleClassicA2dpCodecConfig current = transport.codecConfig();
        if (current.codec != EspBleClassicAudioCodec::Unknown)
        {
            onCodecConfigured(current);
        }
    }
    streaming_ = transport.streaming();

    return true;
}

void EspBleA2dpSinkAdapter::end()
{
    if (transport_ != nullptr)
    {
        // Unregister the media callback first. This waits for an in-flight
        // callback to return, so nothing can be inside pushEncoded() by the
        // time the stream is released below.
        transport_->onMedia({});
        transport_->onCodecConfigured({});
        transport_->onStreamStateChanged({});
        transport_->onConnected({});
        transport_->onDisconnected({});
        transport_ = nullptr;
    }

    stream_.end();
    connectionId_ = 0;
    haveConnection_ = false;
    connected_ = false;
    streaming_ = false;
    foreignPackets_ = 0;
}

void EspBleA2dpSinkAdapter::update()
{
    stream_.update();
}

void EspBleA2dpSinkAdapter::onConnected(const EspBleClassicA2dpConnection &connection)
{
    if (haveConnection_ && connection.id != connectionId_)
    {
        // A second peer. The initial implementation serves one connection;
        // ignore this one rather than interleaving two streams into one
        // decoder (SPEC.md §4.3).
        return;
    }

    connectionId_ = connection.id;
    haveConnection_ = true;
    connected_ = true;

    // The negotiated media MTU is the largest payload the transport can
    // deliver. If it exceeds what the stream was configured to accept, every
    // full-size packet would be dropped on arrival, so grow the stream to
    // fit. Control path only, and only when the configuration was too small.
    const size_t mtu = connection.mediaMtu;
    if (mtu > 0)
    {
        A2dpSinkStream::Config config = stream_.config();
        if (mtu > config.maximumPacketBytes)
        {
            config.maximumPacketBytes = mtu;
            const size_t minimumQueue =
                mtu + EncodedPacketQueue::overheadPerPacket();
            if (config.encodedQueueBytes < minimumQueue)
            {
                config.encodedQueueBytes = minimumQueue;
            }
            const EncodedAudioFormat codec = stream_.codecConfig();
            if (stream_.begin(config) && codec.codec != EncodedAudioCodec::Unknown)
            {
                stream_.setCodecConfig(codec);
            }
        }
    }
}

void EspBleA2dpSinkAdapter::onDisconnected(const EspBleClassicA2dpConnection &connection)
{
    if (haveConnection_ && connection.id != connectionId_) return;

    connected_ = false;
    streaming_ = false;
    haveConnection_ = false;
    connectionId_ = 0;

    // Nothing buffered belongs to the next session.
    stream_.reset();
}

void EspBleA2dpSinkAdapter::onCodecConfigured(const EspBleClassicA2dpCodecConfig &config)
{
    // Codec configuration can arrive before the connection event, so this
    // adopts the connection id rather than requiring one already (SPEC.md §5).
    if (!haveConnection_)
    {
        connectionId_ = config.connectionId;
        haveConnection_ = true;
    }
    else if (config.connectionId != connectionId_)
    {
        return;
    }

    stream_.setCodecConfig(toEncodedAudioFormat(config));
}

void EspBleA2dpSinkAdapter::onStreamStateChanged(const EspBleClassicA2dpStreamEvent &event)
{
    if (haveConnection_ && event.connectionId != connectionId_) return;

    const bool started = (event.state == EspBleClassicA2dpStreamState::Started);
    streaming_ = started;

    if (!started)
    {
        // Suspended. Whatever is buffered is stale by the time the stream
        // resumes, and playing it out then would sound like an echo of the
        // moment before the pause (SPEC.md §7).
        stream_.reset();
    }
}

void EspBleA2dpSinkAdapter::onMedia(const EspBleClassicEncodedAudioView &view)
{
    // Bluetooth host callback context. Copy and return: no decoding, no
    // allocation, no blocking (SPEC.md §5).
    if (haveConnection_ && view.connectionId != connectionId_)
    {
        ++foreignPackets_;
        return;
    }
    if (view.data == nullptr || view.length == 0) return;

    stream_.pushEncoded(view.data, view.length, view.frameCount, view.timestamp);
}

#endif // PCMFLOWBLUETOOTH_ESPBLE_ADAPTER_AVAILABLE
