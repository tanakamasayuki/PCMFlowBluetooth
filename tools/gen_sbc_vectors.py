#!/usr/bin/env python3
"""Generate SBC test vectors for tests/sbc_decoder/.

Maintainer tool. Regenerate when the vector set should change:

    python tools/gen_sbc_vectors.py --apply

It builds the **vendored Broadcom SBC encoder** for the host, encodes a
deterministic PCM signal in each configuration the decoder must handle, and
writes tests/sbc_decoder/input/sbc_vectors.h containing the encoded frames
alongside the PCM that produced them.

Why encode rather than ship captured frames
-------------------------------------------
The decoder has to be checked against every channel mode, sample rate and
bitpool the A2DP negotiation can select. Capturing that many streams from
real hardware is neither reproducible nor reviewable; encoding them from a
known signal is both, and it exercises the same vendored code the A2DP
Source phase will eventually ship.

Building the encoder for the **host only** is what makes this safe: on an
ESP32 it would collide with the SBC encoder inside libbt.a (see SPEC.md
§11.3). The generated header contains nothing but data.

The comparison the test makes is statistical, not sample-exact: SBC is lossy
and has filter-bank delay, so the test asserts frame counts, output layout,
and that the decoded signal keeps the energy and dominant frequency of the
input. Bit-exactness against the ITU/SIG reference is not re-asserted here —
the vendored codec is the same one Bluedroid ships.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import struct
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "src"
ENCODER_DIR = SRC_DIR / "external" / "sbc" / "encoder" / "srce"
OUT_PATH = REPO_ROOT / "tests" / "sbc_decoder" / "input" / "sbc_vectors.h"

# Encoder constants, from src/external/sbc/encoder/include/sbc_encoder.h.
SBC_MONO, SBC_DUAL, SBC_STEREO, SBC_JOINT_STEREO = 0, 1, 2, 3
SBC_LOUDNESS, SBC_SNR = 0, 1
FREQ_INDEX = {16000: 0, 32000: 1, 44100: 2, 48000: 3}

CHANNEL_MODE_NAMES = {
    SBC_MONO: "Mono",
    SBC_DUAL: "DualChannel",
    SBC_STEREO: "Stereo",
    SBC_JOINT_STEREO: "JointStereo",
}

# One vector per row. Chosen to cover each channel mode, every A2DP sample
# rate, both allocation methods, both subband counts, and the ends of the
# bitpool range measured against a real Source (2..53, SPEC.md §5.1).
#
# name, rate, channel mode, subbands, blocks, allocation, bitpool
VECTORS = [
    ("stereo_48k",       48000, SBC_STEREO,       8, 16, SBC_LOUDNESS, 53),
    ("joint_44k1",       44100, SBC_JOINT_STEREO, 8, 16, SBC_LOUDNESS, 53),
    ("mono_48k",         48000, SBC_MONO,         8, 16, SBC_LOUDNESS, 26),
    ("dual_48k",         48000, SBC_DUAL,         8, 16, SBC_LOUDNESS, 26),
    ("stereo_16k",       16000, SBC_STEREO,       8, 16, SBC_LOUDNESS, 32),
    ("stereo_32k",       32000, SBC_STEREO,       8, 16, SBC_LOUDNESS, 32),
    ("stereo_48k_sb4",   48000, SBC_STEREO,       4, 16, SBC_LOUDNESS, 26),
    ("stereo_48k_blk4",  48000, SBC_STEREO,       8,  4, SBC_LOUDNESS, 32),
    ("stereo_48k_snr",   48000, SBC_STEREO,       8, 16, SBC_SNR,      32),
    ("stereo_48k_bp2",   48000, SBC_STEREO,       8, 16, SBC_LOUDNESS,  2),
]

# PCM frames encoded per vector. Enough for several SBC frames in every
# configuration (the largest frame consumes 16 blocks x 8 subbands = 128).
PCM_FRAMES = 1024

# Test signal: a tone well inside every rate's passband, plus a quieter
# second tone so joint stereo and dual channel have something to differ on.
TONE_HZ = 1000.0
TONE2_HZ = 2500.0
AMPLITUDE = 12000.0

DRIVER_C = r"""
/* Vector generator driver — host only, not part of the library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sbc_encoder.h"

/* Bluedroid's logging macro resolves to this. Nothing here logs. */
void APPL_TRACE_EVENT(const char *fmt, ...) { (void)fmt; }

int main(int argc, char **argv)
{
    if (argc != 8) {
        fprintf(stderr, "usage: %s rate mode subbands blocks alloc bitpool pcmframes\n", argv[0]);
        return 2;
    }
    const int freqIndex   = atoi(argv[1]);
    const int channelMode = atoi(argv[2]);
    const int subbands    = atoi(argv[3]);
    const int blocks      = atoi(argv[4]);
    const int alloc       = atoi(argv[5]);
    const int bitpool     = atoi(argv[6]);
    const int pcmFrames   = atoi(argv[7]);

    const int channels = (channelMode == 0) ? 1 : 2;

    /* PCM arrives on stdin as interleaved little-endian int16. */
    SINT16 *pcm = (SINT16 *)malloc((size_t)pcmFrames * channels * sizeof(SINT16));
    if (!pcm) return 1;
    if (fread(pcm, sizeof(SINT16), (size_t)pcmFrames * channels, stdin)
        != (size_t)pcmFrames * channels) {
        fprintf(stderr, "short PCM read\n");
        return 1;
    }

    static SBC_ENC_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.s16SamplingFreq     = (SINT16)freqIndex;
    params.s16ChannelMode      = (SINT16)channelMode;
    params.s16NumOfSubBands    = (SINT16)subbands;
    params.s16NumOfChannels    = (SINT16)channels;
    params.s16NumOfBlocks      = (SINT16)blocks;
    params.s16AllocationMethod = (SINT16)alloc;
    params.s16BitPool          = (SINT16)bitpool;
    params.sbc_mode            = SBC_MODE_STD;
    params.u8NumPacketToEncode = 1;

    SBC_Encoder_Init(&params);

    /* Init derives s16BitPool from u16BitRate and overwrites whatever was
     * set beforehand. We want to pin the bitpool directly — it is what A2DP
     * negotiates and what the decoder has to cope with — so restore it
     * afterwards, exactly as Bluedroid's A2DP layer does. */
    params.s16BitPool = (SINT16)bitpool;

    /* One SBC frame consumes blocks * subbands PCM frames. */
    const int framePcm = blocks * subbands;
    static UINT8 packet[1024];

    for (int offset = 0; offset + framePcm <= pcmFrames; offset += framePcm) {
        /* SBC_Encoder() resets ps16NextPcmBuffer to its internal
         * as16PcmBuffer on entry, so the samples have to be copied in
         * rather than pointed at. This is what Bluedroid's A2DP layer does
         * as well. */
        memcpy(params.as16PcmBuffer, pcm + (size_t)offset * channels,
               (size_t)framePcm * channels * sizeof(SINT16));
        params.pu8Packet = packet;
        SBC_Encoder(&params);
        fwrite(packet, 1, params.u16PacketLength, stdout);
    }
    free(pcm);
    return 0;
}
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--apply", action="store_true",
                        help="Write the header. Without it, only report.")
    parser.add_argument("--cc", default="gcc", help="Host compiler (default: gcc).")
    return parser.parse_args()


def build_encoder(cc: str, workdir: pathlib.Path) -> pathlib.Path:
    driver = workdir / "gen_sbc_vectors_driver.c"
    driver.write_text(DRIVER_C, encoding="utf-8")

    binary = workdir / "gen_sbc_vectors"
    sources = sorted(ENCODER_DIR.glob("*.c"))
    if not sources:
        raise SystemExit(f"error: no encoder sources in {ENCODER_DIR} — run tools/sync_sbc.py")

    cmd = [
        cc, "-O2", "-w",
        "-DSBC_ENC_INCLUDED=TRUE",
        "-DPCMFLOWBT_SBC_NO_RENAME=1",
        f"-I{SRC_DIR}", f"-I{SRC_DIR / 'external'}",
        f"-I{SRC_DIR / 'external' / 'sbc' / 'encoder' / 'include'}",
        "-o", str(binary), str(driver), *[str(s) for s in sources],
        "-lm",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("error: failed to build the encoder\n" + result.stderr)
    return binary


def make_pcm(rate: int, channels: int, frames: int) -> tuple[bytes, list[int]]:
    """Deterministic interleaved int16 PCM. Returns (bytes, flat samples)."""
    import math

    samples: list[int] = []
    for n in range(frames):
        t = n / rate
        left = AMPLITUDE * math.sin(2.0 * math.pi * TONE_HZ * t)
        if channels == 1:
            samples.append(int(round(left)))
            continue
        right = 0.5 * AMPLITUDE * math.sin(2.0 * math.pi * TONE2_HZ * t)
        samples.append(int(round(left)))
        samples.append(int(round(right)))
    data = struct.pack(f"<{len(samples)}h", *samples)
    return data, samples


def encode(binary: pathlib.Path, spec, pcm: bytes) -> bytes:
    _name, rate, mode, subbands, blocks, alloc, bitpool = spec
    channels = 1 if mode == SBC_MONO else 2
    result = subprocess.run(
        [str(binary), str(FREQ_INDEX[rate]), str(mode), str(subbands),
         str(blocks), str(alloc), str(bitpool), str(PCM_FRAMES)],
        input=pcm, capture_output=True,
    )
    if result.returncode != 0:
        raise SystemExit(f"error: encoding {_name} failed\n{result.stderr.decode()}")
    if not result.stdout:
        raise SystemExit(f"error: encoding {_name} produced nothing")
    del channels
    return result.stdout


def format_bytes(data: bytes, indent: str = "    ") -> str:
    lines = []
    for start in range(0, len(data), 12):
        chunk = data[start:start + 12]
        lines.append(indent + " ".join(f"0x{b:02x}," for b in chunk))
    return "\n".join(lines)


def render(vectors: list[dict]) -> str:
    out = [
        "// Generated by tools/gen_sbc_vectors.py — do not edit.",
        "//",
        "// SBC frames produced by the vendored Broadcom encoder from a known",
        "// tone, one entry per configuration the decoder must handle.",
        "//",
        f"// Signal: {TONE_HZ:.0f} Hz at amplitude {AMPLITUDE:.0f}, plus a",
        f"// {TONE2_HZ:.0f} Hz tone at half amplitude on the right channel.",
        "",
        "#ifndef PCMFLOWBLUETOOTH_TEST_SBC_VECTORS_H",
        "#define PCMFLOWBLUETOOTH_TEST_SBC_VECTORS_H",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        "struct SbcVector {",
        "    const char *name;",
        "    const uint8_t *data;",
        "    size_t length;",
        "    uint32_t sampleRate;",
        "    uint8_t streamChannels;   // channels the SBC stream carries",
        "    uint8_t subbands;",
        "    uint8_t blocks;",
        "    uint8_t bitpool;",
        "    uint16_t frameCount;      // SBC frames in data",
        "    uint16_t pcmFramesPerSbcFrame;",
        "    uint16_t toneHz;          // dominant frequency of the encoded signal",
        "};",
        "",
    ]

    for v in vectors:
        out.append(f"// {v['name']}: {v['modeName']}, {v['rate']} Hz, "
                   f"{v['subbands']} subbands, {v['blocks']} blocks, "
                   f"bitpool {v['bitpool']}, {v['frameCount']} frames")
        out.append(f"static const uint8_t kSbc_{v['name']}[] = {{")
        out.append(format_bytes(v["data"]))
        out.append("};")
        out.append("")

    out.append("static const SbcVector kSbcVectors[] = {")
    for v in vectors:
        out.append(
            f"    {{ \"{v['name']}\", kSbc_{v['name']}, sizeof(kSbc_{v['name']}), "
            f"{v['rate']}, {v['channels']}, {v['subbands']}, {v['blocks']}, "
            f"{v['bitpool']}, {v['frameCount']}, {v['pcmPerFrame']}, "
            f"{int(TONE_HZ)} }},"
        )
    out.append("};")
    out.append("")
    out.append("static const size_t kSbcVectorCount = "
               "sizeof(kSbcVectors) / sizeof(kSbcVectors[0]);")
    out.append("")
    out.append("#endif // PCMFLOWBLUETOOTH_TEST_SBC_VECTORS_H")
    return "\n".join(out) + "\n"


def main() -> int:
    args = parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        binary = build_encoder(args.cc, pathlib.Path(tmp))

        rendered: list[dict] = []
        for spec in VECTORS:
            name, rate, mode, subbands, blocks, alloc, bitpool = spec
            channels = 1 if mode == SBC_MONO else 2
            pcm, _ = make_pcm(rate, channels, PCM_FRAMES)
            data = encode(binary, spec, pcm)

            pcm_per_frame = blocks * subbands
            frame_count = PCM_FRAMES // pcm_per_frame
            rendered.append({
                "name": name,
                "modeName": CHANNEL_MODE_NAMES[mode],
                "rate": rate,
                "channels": channels,
                "subbands": subbands,
                "blocks": blocks,
                "bitpool": bitpool,
                "data": data,
                "frameCount": frame_count,
                "pcmPerFrame": pcm_per_frame,
            })
            print(f"{name:18} {len(data):6} bytes  {frame_count:3} frames "
                  f"({len(data)//frame_count} bytes/frame)")

    header = render(rendered)
    if not args.apply:
        print(f"\nDry run. Re-run with --apply to write {OUT_PATH}.")
        return 0

    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(header, encoding="utf-8")
    print(f"\nWrote {OUT_PATH} ({len(header)} bytes).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
