"""Exercise keyboard handling through a real PTY without requiring uinput/root."""

import errno
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time


def exercise(binary, exit_signal=None):
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    process = subprocess.Popen(
        [binary, "--dry-run"], stdin=slave, stdout=slave, stderr=slave
    )

    def collect(seconds=0.3):
        output = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
            if ready:
                try:
                    output.extend(os.read(master, 65536))
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    break
        return output.decode()

    def send(keys, seconds=0.3):
        os.write(master, keys)
        return collect(seconds)

    try:
        assert "Focus this terminal" in collect()
        if exit_signal is None:
            for key, code in [(b"1", 304), (b"2", 315), (b"0", 305)]:
                output = send(key)
                assert f"KEY {code} 1" in output, output
                assert f"KEY {code} 0" in output, output
                if key != b"2":
                    assert "ABS 2 32767" in output, output
                    assert "ABS 2 -32767" in output, output

            # Repeated stand command must work after the release.
            assert "KEY 304 1" in send(b"1")
            for key, code, value in [
                (b"w", 1, -16384), (b"s", 1, 16384),
                (b"a", 0, -16384), (b"d", 0, 16384),
                (b"q", 3, -16384), (b"e", 3, 16384),
            ]:
                output = send(key, 0.55)
                assert f"ABS {code} {value}" in output, output
                assert f"ABS {code} 0" in output, output

            # Per-axis timeout: refreshing forward must not latch an old turn.
            send(b"qw", 0.25)
            output = send(b"w", 0.2)
            assert "ABS 3 0" in output, output
            assert "ABS 1 0" not in output, output
            output = send(b" ", 0.1)
            assert "ABS 1 0" in output and "ABS 3 0" in output, output

            # Arrow/function escape sequences must not act like lowercase commands.
            assert "KEY " not in send(b"\x1b[12~\x1bOA\x1b[A", 0.1)
            send(b"W", 0.1)
            output = send(b"x", 0.1)
        else:
            send(b"w", 0.05)
            process.send_signal(exit_signal)
            output = collect(0.1)
        assert process.wait(timeout=2) == 0
        assert "Inputs cleared" in output, output
        assert "ABS 1 0" in output, output
        assert termios.tcgetattr(slave) == original, "Terminal settings were not restored"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


def main():
    binary = sys.argv[1]
    for args in [("--axis-value", "nan"), ("--axis-value", "1.1"),
                 ("--axis-value", "0.5oops"), ("--axis-value",), ("--unknown",)]:
        result = subprocess.run([binary, *args], capture_output=True)
        assert result.returncode == 1, args
    assert subprocess.run([binary, "--help"], capture_output=True).returncode == 0
    # Reject noninteractive stdin clearly rather than leave a device unattended.
    assert subprocess.run([binary, "--dry-run"], input=b"", capture_output=True).returncode == 1
    exercise(binary)
    exercise(binary, signal.SIGINT)
    exercise(binary, signal.SIGTERM)
    print("PASS: button pulses, movement directions/expiry, escape handling, exit cleanup, CLI")


if __name__ == "__main__":
    main()
