import pytest

import main


@pytest.mark.parametrize("riff,data", [(True, True), (True, False), (False, True), (False, False)])
def test_settle_only_unknown_lengths(wav, riff, data):
    streamed = bytearray(wav)
    if riff:
        streamed[4:8] = b"\xff" * 4
    if data:
        streamed[40:44] = b"\xff" * 4
    assert main.settle_wav_lengths(streamed) is (riff or data)
    assert streamed == wav
    assert main.settle_wav_lengths(streamed) is False


def test_extra_odd_chunk(wav):
    streamed = bytearray(wav[:36] + b"JUNK\x03\x00\x00\x00abc\x00" + wav[36:])
    streamed[4:8] = b"\xff" * 4
    streamed[52:56] = b"\xff" * 4
    assert main.settle_wav_lengths(streamed)
    assert int.from_bytes(streamed[4:8], "little") == len(streamed) - 8
    assert int.from_bytes(streamed[52:56], "little") == len(wav) - 44
    assert streamed[56:] == wav[44:]
    assert streamed[36:48] == b"JUNK\x03\x00\x00\x00abc\x00"


@pytest.mark.parametrize("value", [b"", b"RIFF", b"x" * 44, b"RIFF\x00\x00\x00\x00WAVEabc"])
def test_invalid_or_truncated_header(value):
    data = bytearray(value)
    assert main.settle_wav_lengths(data) is False
    assert data == value


def test_describe(wav):
    assert main.describe_wav(wav) == "16000 Hz, 1 ch, 16-bit, 0.01 s"
    assert main.describe_wav(b"invalid").startswith("not a readable WAV (")
