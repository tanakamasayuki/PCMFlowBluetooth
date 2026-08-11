#include <EspBleClassic.h>
#include <PCMFlowBluetooth.h>
#include <esp_mac.h>

EspBleClassic classic;
EspBleA2dpSinkAdapter a2dp;

static constexpr uint32_t PacketsPerBurst = 8;
static constexpr uint32_t ExpectedPcmFrames = PacketsPerBurst * 8 * 128;
static uint32_t totalPcmFrames = 0;
static uint32_t pcmHash = 2166136261UL;
static uint16_t pcmPeak = 0;
static bool resultPrinted = false;
static bool wasConnected = false;
static bool wasStreaming = false;
static uint32_t burstNumber = 0;

static String classicAddress()
{
    uint8_t address[6] = {};
    esp_read_mac(address, ESP_MAC_BT);
    char value[18];
    snprintf(value, sizeof(value), "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0], address[1], address[2], address[3],
             address[4], address[5]);
    return String(value);
}

static void consumePcm()
{
    static int16_t pcm[256 * 2];
    size_t frames = 0;
    while ((frames = a2dp.readFrames(pcm, 256)) > 0)
    {
        const size_t samples = frames * a2dp.format().channels;
        for (size_t i = 0; i < samples; ++i)
        {
            const int32_t magnitude = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
            if (magnitude > pcmPeak) pcmPeak = static_cast<uint16_t>(magnitude);

            const uint16_t value = static_cast<uint16_t>(pcm[i]);
            pcmHash ^= static_cast<uint8_t>(value);
            pcmHash *= 16777619UL;
            pcmHash ^= static_cast<uint8_t>(value >> 8);
            pcmHash *= 16777619UL;
        }
        totalPcmFrames += frames;
    }
}

static void printResult()
{
    if (resultPrinted || totalPcmFrames < ExpectedPcmFrames) return;
    resultPrinted = true;

    const A2dpSinkStream &stream = a2dp.stream();
    const PCMFormat &format = a2dp.format();
    Serial.printf(
        "PCM_A2DP_RESULT burst=%lu rate=%lu channels=%u bits=%u frames=%lu "
        "peak=%u hash=%08lx packets=%lu decoded=%lu dropped=%lu "
        "invalid=%lu failures=%lu overflow=%lu foreign=%lu error=%s\n",
        static_cast<unsigned long>(burstNumber),
        static_cast<unsigned long>(format.sampleRate), format.channels,
        format.bitsPerSample, static_cast<unsigned long>(totalPcmFrames),
        pcmPeak, static_cast<unsigned long>(pcmHash),
        static_cast<unsigned long>(stream.receivedPacketCount()),
        static_cast<unsigned long>(stream.decodedFrameCount()),
        static_cast<unsigned long>(stream.droppedPacketCount()),
        static_cast<unsigned long>(stream.invalidFrameCount()),
        static_cast<unsigned long>(stream.decodeFailureCount()),
        static_cast<unsigned long>(stream.pcmOverflowFrameCount()),
        static_cast<unsigned long>(a2dp.foreignConnectionPacketCount()),
        stream.lastErrorName());
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    EspBleClassicConfig stackConfig;
    stackConfig.deviceName = "PCMFlowBluetooth A2DP Sink";
    if (!classic.begin(stackConfig))
    {
        Serial.printf("PCM_A2DP_STACK_FAILED %s:%s\n",
                      classic.lastErrorName(), classic.lastErrorDetail().c_str());
        return;
    }
    if (!classic.a2dpSink().begin())
    {
        Serial.printf("PCM_A2DP_PROFILE_FAILED %s:%s\n",
                      classic.lastErrorName(), classic.lastErrorDetail().c_str());
        return;
    }
    if (!a2dp.begin(classic.a2dpSink()))
    {
        Serial.printf("PCM_A2DP_ADAPTER_FAILED %s\n",
                      a2dp.stream().lastErrorName());
        return;
    }

    Serial.printf("PCM_A2DP_READY address=%s\n", classicAddress().c_str());
}

void loop()
{
    classic.update();
    a2dp.update();
    consumePcm();

    if (a2dp.connected() != wasConnected)
    {
        wasConnected = a2dp.connected();
        Serial.printf("PCM_A2DP_CONNECTED value=%u\n", wasConnected ? 1 : 0);
    }
    if (a2dp.streaming() != wasStreaming)
    {
        wasStreaming = a2dp.streaming();
        if (wasStreaming)
        {
            ++burstNumber;
            totalPcmFrames = 0;
            pcmHash = 2166136261UL;
            pcmPeak = 0;
            resultPrinted = false;
        }
        Serial.printf("PCM_A2DP_STREAMING value=%u\n", wasStreaming ? 1 : 0);
    }

    printResult();
    delay(1);
}
