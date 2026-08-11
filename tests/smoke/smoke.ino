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

    // SPEC §13.1 requires that the adapter be *explicitly* unsupported where
    // it cannot work, rather than silently compiling to something inert. The
    // two build inputs are printed separately so the test can assert the
    // implication rather than a bare 0: without <EspBleClassic.h>, or off the
    // plain ESP32, the adapter must not be declared. The =1 side of that
    // implication is what examples/ and tests/peer/ build.
    Serial.print("classic_arch=");
#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32)
    Serial.println(1);
#else
    Serial.println(0);
#endif

    Serial.print("espble_classic_header=");
#if __has_include(<EspBleClassic.h>)
    Serial.println(1);
#else
    Serial.println(0);
#endif

    Serial.print("adapter=");
    Serial.println(PCMFLOWBLUETOOTH_HAS_ESPBLE_ADAPTER);

    Serial.println("SMOKE ready");
}

void loop()
{
    delay(1);
}
