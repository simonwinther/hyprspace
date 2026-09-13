"""Bound fixture replies, including peers that write a partial line and stall."""

import os
import select
import time


def reply(process, timeout=3):
    deadline = time.monotonic() + timeout
    result = bytearray()
    while len(result) < 65536:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([process.stdout], [], [], remaining)[0]:
            raise TimeoutError("fixture reply timed out")
        byte = os.read(process.stdout.fileno(), 1)
        if not byte:
            raise RuntimeError(f"fixture exited before replying: {process.poll()}")
        if byte == b"\n":
            return result.decode().strip()
        result.extend(byte)
    raise ValueError("fixture reply exceeds 64 KiB")
