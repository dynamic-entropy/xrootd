#!/usr/bin/env python3
"""Send a raw HTTP request over TCP; print the response status line.

stdin  — request bytes to send verbatim
argv   — host port

The server may need multiple reads to finish parsing headers before it
sends 400; the peer may also close abruptly. We half-close the write side
after sendall(), then read until an HTTP/1.x status line appears.
"""

import re
import select
import socket
import sys
import time

TIMEOUT = 15.0
STATUS_RE = re.compile(rb"HTTP/1\.[01] [0-9]{3}[^\r\n]*")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: smuggling_send.py host port", file=sys.stderr)
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2])
    payload = sys.stdin.buffer.read()

    data = b""
    try:
        with socket.create_connection((host, port), timeout=TIMEOUT) as sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.sendall(payload)
            sock.shutdown(socket.SHUT_WR)

            end = time.monotonic() + TIMEOUT
            while time.monotonic() < end:
                wait = max(0.0, end - time.monotonic())
                ready, _, _ = select.select([sock], [], [], wait)
                if not ready:
                    break
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
                match = STATUS_RE.search(data)
                if match:
                    sys.stdout.write(match.group(0).decode("latin-1"))
                    sys.stdout.flush()
                    return 0
    except OSError:
        pass

    if not data:
        sys.stdout.write(
            "<server closed the connection without sending a status line>"
        )
    else:
        sys.stdout.write(
            "<request stalled — read timed out waiting for a status line>"
        )
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
