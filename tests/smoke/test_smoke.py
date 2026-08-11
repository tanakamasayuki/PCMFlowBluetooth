"""Smoke test — verifies the build and harness wiring.

If this builds, the umbrella header and the portable core have been picked
up correctly by the Arduino library loader on the selected profile.

It also pins SPEC §13.1's requirement that `EspBleA2dpSinkAdapter` be
explicitly unsupported where it cannot work: the sketch prints the two build
inputs of the guard, and the adapter must be absent unless both hold. Neither
profile here links EspBle, so this covers the =0 side of the implication;
examples/ and peer/ cover the =1 side.
"""

import re


def read_flag(dut, name):
    match = dut.expect(re.compile(rb"%s=([01])" % name.encode()), timeout=10)
    return match.group(1) == b"1"


def test_smoke(dut):
    dut.expect("fmt.isValid=1", timeout=10)

    classic_arch = read_flag(dut, "classic_arch")
    espble_header = read_flag(dut, "espble_classic_header")
    adapter = read_flag(dut, "adapter")

    assert adapter == (classic_arch and espble_header)
    assert not adapter, (
        "no profile in this suite links EspBle, so the adapter must not be "
        "declared; a 1 here means the guard stopped being the thing that "
        "decides"
    )

    dut.expect("SMOKE ready", timeout=10)
