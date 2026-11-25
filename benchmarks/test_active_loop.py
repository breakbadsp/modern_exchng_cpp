"""Drive the native client through socket pairs to test report correlation."""
import select
import socket
import struct
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from pathlib import Path
from tempfile import TemporaryDirectory
import time
import unittest

from perf_active_sessions import Options, compile_loop, work

ORDER = struct.Struct("<HBQQIBBqI")
ACK = struct.Struct("<HBQQIQI")


def exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("Driver socket closed")
        data += chunk
    return data


class ActiveLoopTest(unittest.TestCase):
    def test_hot_symbol_reply_from_other_socket_is_rejected(self):
        with TemporaryDirectory() as temp, ExitStack() as cleanup:
            library = Path(temp) / "loop.so"
            compile_loop(library)
            poll = cleanup.enter_context(select.epoll())
            driver, exchange = [], []
            for _ in range(2):
                client, peer = socket.socketpair()
                driver.append(cleanup.enter_context(client))
                exchange.append(cleanup.enter_context(peer))
                client.setblocking(False)
                peer.settimeout(3)
                poll.register(client, select.EPOLLIN)
            with ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(work, library, driver, poll,
                                         Options(time.monotonic_ns() + 30_000_000, 1, 6, 0, 0, 1))
                orders = [ORDER.unpack(exact(peer, ORDER.size)) for peer in exchange]
                self.assertEqual(orders[0][4], orders[1][4])
                self.assertNotEqual(orders[0][3], orders[1][3])
                for peer, order in zip(exchange, reversed(orders)):
                    peer.sendall(ACK.pack(ACK.size, 66, 2, order[3], order[4], 1, 1))
                with self.assertRaisesRegex(RuntimeError, "unexpected report"):
                    future.result(timeout=4)


if __name__ == "__main__":
    unittest.main()
