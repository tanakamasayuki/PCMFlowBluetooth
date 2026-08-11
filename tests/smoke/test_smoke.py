"""Smoke test — verifies the build and harness wiring.

If this builds, the umbrella header and the portable core have been picked
up correctly by the Arduino library loader on the selected profile.
"""


def test_smoke(dut):
    dut.expect("fmt.isValid=1", timeout=10)
    dut.expect("SMOKE ready", timeout=10)
