import re


CONNECTED = re.compile(rb"SBC_SOURCE_CONNECTED mtu=\d+")
CODEC = re.compile(
    rb"SBC_SOURCE_CODEC rate=48000 channels=2 mode=\d+ blocks=16 "
    rb"subbands=8 bitpool=2-53"
)
RESULT = re.compile(
    rb"PCM_A2DP_RESULT burst=(\d+) rate=(\d+) channels=(\d+) bits=(\d+) "
    rb"frames=(\d+) peak=(\d+) hash=([0-9a-f]{8}) packets=(\d+) "
    rb"decoded=(\d+) dropped=(\d+) invalid=(\d+) failures=(\d+) "
    rb"overflow=(\d+) foreign=(\d+) error=([^\r\n]+)"
)


def expect_connection(dut, peer):
    peer.expect([CONNECTED, CODEC], timeout=30, expect_all=True)
    dut.expect_exact("PCM_A2DP_CONNECTED value=1", timeout=30)


def run_burst(dut, peer, burst, packets, decoded):
    peer.write(b"s\n")
    peer.expect_exact("SBC_SOURCE_START requested=1", timeout=10)
    peer.expect(re.compile(rb"SBC_SOURCE_STREAM state=1"), timeout=20)
    dut.expect_exact("PCM_A2DP_STREAMING value=1", timeout=20)
    peer.expect_exact(
        "SBC_SOURCE_BURST packets=8 bytes=7552 frames=64 "
        f"last_timestamp={1000 + (burst * 8 - 1) * 1024}",
        timeout=30,
    )

    result = dut.expect(RESULT, timeout=30)
    assert result.group(1) == str(burst).encode()
    assert result.group(2) == b"48000"
    assert result.group(3) == b"2"
    assert result.group(4) == b"16"
    assert result.group(5) == b"8192"
    assert int(result.group(6)) > 1000
    assert result.group(7) == b"e511d892"
    assert result.group(8) == str(packets).encode()
    assert result.group(9) == str(decoded).encode()
    assert result.group(10) == b"0"
    assert result.group(11) == b"0"
    assert result.group(12) == b"0"
    assert result.group(13) == b"0"
    assert result.group(14) == b"0"
    assert result.group(15) == b"None"
    return result.group(7)


def suspend(dut, peer):
    peer.write(b"p\n")
    peer.expect_exact("SBC_SOURCE_SUSPEND requested=1", timeout=10)
    peer.expect(re.compile(rb"SBC_SOURCE_STREAM state=0"), timeout=20)
    dut.expect_exact("PCM_A2DP_STREAMING value=0", timeout=20)


def disconnect(dut, peer, sent_packets):
    peer.write(b"d\n")
    peer.expect_exact("SBC_SOURCE_DISCONNECT requested=1", timeout=10)
    peer.expect_exact(
        f"SBC_SOURCE_DISCONNECTED sent={sent_packets} would_block=0", timeout=30
    )
    dut.expect_exact("PCM_A2DP_CONNECTED value=0", timeout=30)


def test_real_a2dp_sbc_reaches_pcm_across_resume_and_reconnect(dut, peers):
    peer = peers["device"]

    ready = dut.expect(
        re.compile(rb"PCM_A2DP_READY address=([0-9a-f:]+)"), timeout=30
    )
    address = ready.group(1)
    peer.expect_exact(
        "SBC_SOURCE_READY started=1 vector_bytes=944 vector_frames=8", timeout=30
    )

    peer.write(b"c" + address + b"\n")
    peer.expect_exact("SBC_SOURCE_CONNECT requested=1", timeout=10)
    expect_connection(dut, peer)

    first_hash = run_burst(dut, peer, burst=1, packets=8, decoded=64)
    suspend(dut, peer)

    second_hash = run_burst(dut, peer, burst=2, packets=16, decoded=128)
    assert second_hash == first_hash
    suspend(dut, peer)
    disconnect(dut, peer, sent_packets=16)

    peer.write(b"c" + address + b"\n")
    peer.expect_exact("SBC_SOURCE_CONNECT requested=1", timeout=10)
    expect_connection(dut, peer)

    third_hash = run_burst(dut, peer, burst=3, packets=24, decoded=192)
    assert third_hash == first_hash
    suspend(dut, peer)
    disconnect(dut, peer, sent_packets=24)
