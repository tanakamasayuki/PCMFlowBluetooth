// A2dpSinkWithPCMFlow — feed received A2DP audio into the PCMFlow pipeline.
//
// The adapter is a PCMSource, so it plugs into PCMFlow exactly like any
// bundled decoder. PCMFlow then owns the parts it is meant to own: format
// conversion, resampling, gain, and its output ring buffer.
//
// There is still no device output here. Where the frames go — I2S, an
// internal DAC, USB Audio, a board speaker — is the sketch's decision, and
// PCMFlowDevice is where those live. Keeping it out means this example stays
// runnable on any original ESP32 board regardless of what is wired to it.
//
// Requires an original ESP32 (Classic Bluetooth).

#include <EspBleClassic.h>
#include <PCMFlow.h>
#include <PCMFlowBluetooth.h>

EspBleClassic classic;
EspBleA2dpSinkAdapter a2dp;
PCMFlow audio;

// Fixed output format. Pick what the eventual output device wants; PCMFlow
// converts from whatever the A2DP peer negotiated, so this does not have to
// match the incoming stream.
static constexpr uint32_t kOutputRate = 44100;
static constexpr uint8_t kOutputChannels = 2;

static uint32_t lastReport = 0;
static uint64_t totalFrames = 0;
static long peak = 0;

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("A2dpSinkWithPCMFlow");

    if (!classic.begin() || !classic.a2dpSink().begin()) {
        Serial.println("Bluetooth start failed");
        return;
    }
    if (!a2dp.begin(classic.a2dpSink())) {
        Serial.println("a2dp adapter begin() failed");
        return;
    }

    // PCMFlow has no default output format and latches the failure if the
    // first pump() finds one missing, so set it before anything runs.
    PCMFormat out;
    out.sampleRate = kOutputRate;
    out.channels = kOutputChannels;
    out.bitsPerSample = 16;
    audio.setOutputFormat(out);

    // PCMFlow does not call begin() on an external source — the adapter is
    // already started above.
    audio.setInputSource(a2dp);

    Serial.println("Ready — pair with this device and play audio.");
}

void loop()
{
    classic.update();   // EspBle control events
    a2dp.update();      // SBC decode: queue -> PCM
    audio.pump();       // PCMFlow: convert / resample into its ring buffer

    // This is where an output device would be fed. Replace the counting
    // below with i2s.write(), a PCMFlowDevice sink, or whatever the board
    // has; the shape of the loop does not change.
    static int16_t pcm[256 * 2];
    size_t got;
    while ((got = audio.readFrames(pcm, 256)) > 0) {
        totalFrames += got;
        for (size_t i = 0; i < got * kOutputChannels; ++i) {
            const long magnitude = pcm[i] < 0 ? -(long)pcm[i] : pcm[i];
            if (magnitude > peak) peak = magnitude;
        }
    }

    const uint32_t now = millis();
    if (now - lastReport >= 1000) {
        lastReport = now;
        const A2dpSinkStream &s = a2dp.stream();
        Serial.print("connected="); Serial.print(a2dp.connected());
        Serial.print(" streaming="); Serial.print(a2dp.streaming());
        Serial.print(" in="); Serial.print(a2dp.format().sampleRate);
        Serial.print("/"); Serial.print(a2dp.format().channels);
        Serial.print(" out="); Serial.print(audio.outputFormat().sampleRate);
        Serial.print("/"); Serial.print(audio.outputFormat().channels);
        Serial.print(" frames="); Serial.print((unsigned long)totalFrames);
        Serial.print(" peak="); Serial.print(peak);
        Serial.print(" dropped="); Serial.print(s.droppedPacketCount());
        Serial.print(" overflow="); Serial.print(s.pcmOverflowFrameCount());
        Serial.print(" heap="); Serial.println(ESP.getFreeHeap());
        peak = 0;
    }
}
