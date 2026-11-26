"""Check the real load generator against a loopback peer with fixed responses."""

import socket
import struct
import subprocess
import sys
import tempfile


LOGON, NEW, CANCEL, REPLACE = 1, 2, 3, 4
LOGON_ACK, REJECT, CANCEL_REJECT, REPLACE_REJECT = 64, 67, 70, 72
BOOK_FULL, UNKNOWN_ORDER = 7, 8
TRACKED_CAPACITY = 1024
REQUESTS = 8 * TRACKED_CAPACITY


def receive(stream):
    header = stream.read(3)
    assert len(header) == 3, "The load generator ended before sending all frames"
    size, kind = struct.unpack("<HB", header)
    body = stream.read(size - 3)
    assert len(body) == size - 3
    return kind, body


def reply(connection, kind, seq, body=b""):
    connection.sendall(struct.pack("<HBQ", 11 + len(body), kind, seq) + body)


def check_commands(connection):
    tracked = {}
    seen = set()
    with connection.makefile("rb") as stream:
        kind, body = receive(stream)
        assert kind == LOGON and len(body) == 32
        reply(connection, LOGON_ACK, 1)
        for request in range(1, REQUESTS + 1):
            kind, body = receive(stream)
            number, order, symbol = struct.unpack_from("<QQI", body)
            assert number == request
            assert symbol in (1, 2)
            seen.add(kind)
            if kind == NEW:
                assert order not in tracked and len(tracked) < TRACKED_CAPACITY
                tracked[order] = symbol
                answer, reason = REJECT, BOOK_FULL
            elif kind == CANCEL:
                assert order == next(iter(tracked)) and tracked.pop(order) == symbol
                answer, reason = CANCEL_REJECT, UNKNOWN_ORDER
            else:
                assert kind == REPLACE and tracked[order] == symbol
                answer, reason = REPLACE_REJECT, UNKNOWN_ORDER
            reply(connection, answer, request + 1, struct.pack("<QIB", order, symbol, reason))
    assert seen == {NEW, CANCEL, REPLACE}


def main():
    with socket.socket() as listener, tempfile.TemporaryFile(mode="w+") as log:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        with subprocess.Popen(
            [sys.argv[1], "--port", str(listener.getsockname()[1]), "--symbols", "1,2",
             "--orders", str(REQUESTS), "--window", "64"],
            stdout=log, stderr=subprocess.STDOUT,
        ) as client:
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(10)
                    check_commands(connection)
                    assert client.wait(timeout=10) == 0
            except BaseException:
                if client.poll() is None:
                    client.kill()
                client.wait()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise


if __name__ == "__main__":
    main()
