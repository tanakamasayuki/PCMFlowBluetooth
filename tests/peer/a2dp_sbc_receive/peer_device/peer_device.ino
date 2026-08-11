#include <EspBleClassic.h>

#include "../../../sbc_decoder/input/sbc_vectors.h"

EspBleClassic classic;

static bool connected = false;
static uint32_t streamingSince = 0;
static uint32_t nextSendAt = 0;
static uint32_t wouldBlockCount = 0;
static uint32_t sentPacketsInBurst = 0;
static uint32_t totalSentPackets = 0;
static uint32_t nextTimestamp = 1000;
static constexpr uint32_t PacketsPerBurst = 8;

static const SbcVector &testVector()
{
    return kSbcVectors[0];
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    classic.a2dpSource().onConnected(
        [](const EspBleClassicA2dpConnection &connection) {
            connected = true;
            Serial.printf("SBC_SOURCE_CONNECTED mtu=%u\n", connection.mediaMtu);
        });
    classic.a2dpSource().onCodecConfigured(
        [](const EspBleClassicA2dpCodecConfig &codec) {
            Serial.printf(
                "SBC_SOURCE_CODEC rate=%lu channels=%u mode=%u blocks=%u "
                "subbands=%u bitpool=%u-%u\n",
                static_cast<unsigned long>(codec.sampleRate), codec.channels,
                static_cast<unsigned>(codec.sbcChannelMode), codec.sbcBlockLength,
                codec.sbcSubbands, codec.minimumBitpool, codec.maximumBitpool);
        });
    classic.a2dpSource().onStreamStateChanged(
        [](const EspBleClassicA2dpStreamEvent &event) {
            Serial.printf("SBC_SOURCE_STREAM state=%u\n",
                          static_cast<unsigned>(event.state));
            if (event.state == EspBleClassicA2dpStreamState::Started)
            {
                streamingSince = millis();
                nextSendAt = streamingSince + 300;
                sentPacketsInBurst = 0;
            }
        });
    classic.a2dpSource().onDisconnected(
        [](const EspBleClassicA2dpConnection &) {
            connected = false;
            Serial.printf("SBC_SOURCE_DISCONNECTED sent=%lu would_block=%lu\n",
                          static_cast<unsigned long>(totalSentPackets),
                          static_cast<unsigned long>(wouldBlockCount));
        });

    EspBleClassicConfig stackConfig;
    stackConfig.deviceName = "PCMFlowBluetooth SBC Source";
    if (!classic.begin(stackConfig))
    {
        Serial.printf("SBC_SOURCE_STACK_FAILED %s:%s\n",
                      classic.lastErrorName(), classic.lastErrorDetail().c_str());
        return;
    }

    EspBleClassicA2dpSourceConfig sourceConfig;
    sourceConfig.sampleRate = 48000;
    sourceConfig.channelMode = EspBleClassicSbcChannelMode::Stereo;
    sourceConfig.blockLength = 16;
    sourceConfig.subbands = 8;
    sourceConfig.allocationMethod = EspBleClassicSbcAllocationMethod::Loudness;
    sourceConfig.minimumBitpool = 2;
    sourceConfig.maximumBitpool = 53;
    const bool started = classic.a2dpSource().begin(sourceConfig);
    Serial.printf("SBC_SOURCE_READY started=%u vector_bytes=%u vector_frames=%u\n",
                  started ? 1 : 0,
                  static_cast<unsigned>(testVector().length),
                  testVector().frameCount);
}

static void sendVector()
{
    const SbcVector &vector = testVector();
    EspBleClassicEncodedAudioPacket packet;
    packet.data = vector.data;
    packet.length = vector.length;
    packet.frameCount = vector.frameCount;
    packet.timestamp = nextTimestamp;

    const EspBleClassicAudioSendResult result = classic.a2dpSource().send(packet);
    if (result == EspBleClassicAudioSendResult::Accepted)
    {
        ++sentPacketsInBurst;
        ++totalSentPackets;
        nextTimestamp += vector.frameCount * vector.pcmFramesPerSbcFrame;
        nextSendAt = millis() + 25;
        if (sentPacketsInBurst == PacketsPerBurst)
            Serial.printf(
                "SBC_SOURCE_BURST packets=%lu bytes=%lu frames=%lu last_timestamp=%lu\n",
                static_cast<unsigned long>(sentPacketsInBurst),
                static_cast<unsigned long>(sentPacketsInBurst * vector.length),
                static_cast<unsigned long>(sentPacketsInBurst * vector.frameCount),
                static_cast<unsigned long>(packet.timestamp));
    }
    else if (result == EspBleClassicAudioSendResult::WouldBlock)
    {
        ++wouldBlockCount;
    }
    else
    {
        Serial.printf("SBC_SOURCE_SEND_FAILED result=%u\n",
                      static_cast<unsigned>(result));
    }
}

void loop()
{
    classic.update();

    if (Serial.available())
    {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command.startsWith("c"))
            Serial.printf("SBC_SOURCE_CONNECT requested=%u\n",
                          classic.a2dpSource().connect(command.c_str() + 1) ? 1 : 0);
        else if (command == "s" && connected)
            Serial.printf("SBC_SOURCE_START requested=%u\n",
                          classic.a2dpSource().start() ? 1 : 0);
        else if (command == "p" && connected)
            Serial.printf("SBC_SOURCE_SUSPEND requested=%u\n",
                          classic.a2dpSource().suspend() ? 1 : 0);
        else if (command == "d" && connected)
            Serial.printf("SBC_SOURCE_DISCONNECT requested=%u\n",
                          classic.a2dpSource().disconnect() ? 1 : 0);
    }

    if (classic.a2dpSource().streaming() &&
        sentPacketsInBurst < PacketsPerBurst && streamingSince != 0 &&
        static_cast<int32_t>(millis() - nextSendAt) >= 0)
        sendVector();
    delay(1);
}
