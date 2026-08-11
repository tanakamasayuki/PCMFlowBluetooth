// A2dpSinkToPcm — receive A2DP audio and report what arrives.
//
// Pair a phone or PC with this board and play something. The sketch decodes
// the SBC stream to PCM and prints the negotiated format plus running
// statistics over Serial. It produces no sound: there is no device output
// here on purpose, because PCMFlowBluetooth owns the codec and the queue,
// not the speaker. See A2dpSinkWithPCMFlow for the pipeline hookup, and
// PCMFlowDevice for output.
//
// Requires an original ESP32 — Classic Bluetooth does not exist on the S3,
// C3, C6 or H2, and this library does not pretend otherwise.

#include <EspBleClassic.h>
#include <PCMFlowBluetooth.h>

EspBleClassic classic;
EspBleA2dpSinkAdapter a2dp;

static uint32_t lastReport = 0;
static uint64_t totalFrames = 0;
static bool wasReady = false;

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("A2dpSinkToPcm");

    // The sketch owns the Bluetooth stack. PCMFlowBluetooth never starts or
    // stops it — it only consumes the transport.
    if (!classic.begin()) {
        Serial.println("classic.begin() failed");
        return;
    }
    if (!classic.a2dpSink().begin()) {
        Serial.println("a2dpSink.begin() failed");
        return;
    }

    if (!a2dp.begin(classic.a2dpSink())) {
        Serial.println("a2dp adapter begin() failed");
        return;
    }

    Serial.println("Ready — pair with this device and play audio.");
}

void loop()
{
    classic.update();   // EspBle control events (connect, codec, stream state)
    a2dp.update();      // SBC decode: queue -> PCM

    // Drain the PCM so the queue does not overflow. A real sketch hands
    // these frames to I2S, a DAC, or PCMFlow; here they are only counted.
    static int16_t pcm[256 * 2];
    size_t got;
    while ((got = a2dp.readFrames(pcm, 256)) > 0) {
        totalFrames += got;
    }

    if (a2dp.isReady() != wasReady) {
        wasReady = a2dp.isReady();
        if (wasReady) {
            Serial.print("Negotiated: ");
            Serial.print(a2dp.format().sampleRate);
            Serial.print(" Hz, ");
            Serial.print(a2dp.format().channels);
            Serial.print(" ch, ");
            Serial.print(a2dp.format().bitsPerSample);
            Serial.println(" bit");
        }
    }

    const uint32_t now = millis();
    if (now - lastReport >= 1000) {
        lastReport = now;
        const A2dpSinkStream &s = a2dp.stream();
        Serial.print("connected="); Serial.print(a2dp.connected());
        Serial.print(" streaming="); Serial.print(a2dp.streaming());
        Serial.print(" frames="); Serial.print((unsigned long)totalFrames);
        Serial.print(" packets="); Serial.print(s.receivedPacketCount());
        Serial.print(" dropped="); Serial.print(s.droppedPacketCount());
        Serial.print(" invalid="); Serial.print(s.invalidFrameCount());
        Serial.print(" overflow="); Serial.print(s.pcmOverflowFrameCount());
        Serial.print(" queued="); Serial.print((unsigned long)s.availableFrames());
        Serial.print(" lastError="); Serial.print(s.lastErrorName());
        Serial.print(" heap="); Serial.println(ESP.getFreeHeap());
    }
}
