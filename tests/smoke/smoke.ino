// Smoke test sketch — verifies that PCMFlowBluetooth compiles against the
// chosen profile and that the test harness wiring works.
//
// The same source builds on both profiles. The EspBle adapter is ESP32-only,
// so this sketch only touches the portable core; the adapter's build is
// covered by examples/ and by tests/peer/.

#include <PCMFlowBluetooth.h>

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.print("PCMFlowBluetooth ");
    Serial.println(PCMFLOWBLUETOOTH_VERSION_STR);

    EncodedAudioFormat fmt;
    fmt.codec = EncodedAudioCodec::Sbc;
    fmt.sampleRate = 48000;
    fmt.channels = 2;
    fmt.minimumBitpool = 2;
    fmt.maximumBitpool = 53;
    Serial.print("fmt.isValid=");
    Serial.println(fmt.isValid());

    Serial.print("adapter=");
    Serial.println(PCMFLOWBLUETOOTH_HAS_ESPBLE_ADAPTER);

    Serial.println("SMOKE ready");
}

void loop()
{
    delay(1);
}
