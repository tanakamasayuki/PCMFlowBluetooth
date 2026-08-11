"""A2dpSinkStream contract tests."""

import re


def test_a2dp_sink_stream(dut):
    dut.expect("TEST start a2dp_sink_stream", timeout=10)
    match = dut.expect(re.compile(rb"TEST done (\d+)/(\d+)"), timeout=60)
    passed, total = int(match.group(1)), int(match.group(2))
    assert passed == total, f"{total - passed} of {total} assertions failed"
    assert total > 0, "no assertions ran"
