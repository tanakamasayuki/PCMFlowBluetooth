#ifndef PCMFLOWBLUETOOTH_ESPBLEA2DPSINKADAPTER_H
#define PCMFLOWBLUETOOTH_ESPBLEA2DPSINKADAPTER_H

// The one part of this library that depends on EspBle, and therefore the one
// part that only exists where Classic Bluetooth does. Everything below it —
// A2dpSinkStream, SbcDecoder, EncodedPacketQueue — is portable and builds
// anywhere, which is what makes the host test profile possible (SPEC.md §3.1).
//
// On any other target this header declares nothing at all. That is
// deliberate: a stub adapter that compiled but never produced audio would be
// worse than a compile error, because it would look like a working A2DP sink
// on a chip that has no Classic radio.

// Arduino.h first: CONFIG_IDF_TARGET_ESP32 comes from sdkconfig.h through it.
// A .ino gets that include for free, but a library .cpp does not — without
// this the guard below would silently evaluate to 0 in this library's own
// translation units and the adapter would compile to nothing.
#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32) && \
    __has_include(<EspBleClassic.h>)
#define PCMFLOWBLUETOOTH_ESPBLE_ADAPTER_AVAILABLE 1
#else
#define PCMFLOWBLUETOOTH_ESPBLE_ADAPTER_AVAILABLE 0
#endif

#if PCMFLOWBLUETOOTH_ESPBLE_ADAPTER_AVAILABLE

#include <EspBleClassic.h>

#include "A2dpSinkStream.h"

// Wires an EspBle A2DP Sink to an A2dpSinkStream.
//
// This class does no audio work of its own: it registers EspBle's callbacks,
// copies each payload into the stream, and forwards the control events that
// change what the stream should be doing. Everything it delegates to is
// verified by the host test suite; what is left here is the wiring.
//
// It does NOT own the Bluetooth stack. The sketch calls
// EspBleClassic::begin() and EspBleClassicA2dpSink::begin() itself, and
// end() here leaves both running — this adapter is a consumer of the
// transport, not its lifecycle owner (SPEC.md §4.3).
class EspBleA2dpSinkAdapter : public PCMSource
{
public:
    EspBleA2dpSinkAdapter() = default;
    ~EspBleA2dpSinkAdapter() override { end(); }

    EspBleA2dpSinkAdapter(const EspBleA2dpSinkAdapter &) = delete;
    EspBleA2dpSinkAdapter &operator=(const EspBleA2dpSinkAdapter &) = delete;

    // Attach to a transport and register its callbacks.
    //
    // The transport must already have been started. Returns false without
    // registering anything if the stream's buffers could not be allocated.
    //
    // If the transport is already connected and configured, the existing
    // codec configuration is picked up immediately: EspBle may have
    // delivered it before this adapter existed.
    bool begin(EspBleClassicA2dpSink &transport,
               const A2dpSinkStream::Config &config);
    bool begin(EspBleClassicA2dpSink &transport);

    // Unregister the callbacks and release the stream.
    //
    // Unregistering the media callback waits for any in-flight callback to
    // return before it does, so no callback can be running against the
    // stream by the time it is torn down. Never call this from inside a
    // callback — EspBle documents that as unsupported, and it would
    // deadlock on its own barrier.
    void end();

    // Decode queued packets. Call it from the sketch loop or a dedicated
    // task, alongside EspBleClassic::update() which dispatches the control
    // events. The two are independent: this one does not need the other to
    // have run.
    void update();

    // Counters, error state and the overflow policy live on the stream.
    A2dpSinkStream &stream() { return stream_; }
    const A2dpSinkStream &stream() const { return stream_; }

    // PCMSource — delegated to the stream.
    const PCMFormat &format() const override { return stream_.format(); }
    size_t readFrames(void *out, size_t frameCount) override
    {
        return stream_.readFrames(out, frameCount);
    }
    bool isEof() const override { return false; }  // SPEC.md §4.5
    bool isReady() const override { return stream_.isReady(); }

    size_t availableFrames() const { return stream_.availableFrames(); }

    // Transport state, as this adapter last observed it.
    bool connected() const { return connected_; }
    bool streaming() const { return streaming_; }

    // Media payloads that arrived from a connection other than the accepted
    // one. The initial implementation serves a single connection; a second
    // one's audio is dropped rather than mixed into the first (SPEC.md §4.3).
    uint32_t foreignConnectionPacketCount() const { return foreignPackets_; }

    // Translate a negotiated EspBle codec configuration into the transport-
    // independent form the core uses. Exposed because it is worth testing
    // and worth reusing from a sketch that drives the stream directly.
    static EncodedAudioFormat toEncodedAudioFormat(
        const EspBleClassicA2dpCodecConfig &config);

private:
    void onCodecConfigured(const EspBleClassicA2dpCodecConfig &config);
    void onStreamStateChanged(const EspBleClassicA2dpStreamEvent &event);
    void onConnected(const EspBleClassicA2dpConnection &connection);
    void onDisconnected(const EspBleClassicA2dpConnection &connection);
    void onMedia(const EspBleClassicEncodedAudioView &view);

    A2dpSinkStream stream_;
    EspBleClassicA2dpSink *transport_ = nullptr;

    EspBleClassicA2dpConnectionId connectionId_ = 0;
    bool haveConnection_ = false;
    volatile bool connected_ = false;
    volatile bool streaming_ = false;
    uint32_t foreignPackets_ = 0;
};

#endif // PCMFLOWBLUETOOTH_ESPBLE_ADAPTER_AVAILABLE

#endif // PCMFLOWBLUETOOTH_ESPBLEA2DPSINKADAPTER_H
