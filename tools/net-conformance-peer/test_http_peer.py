#!/usr/bin/env python3

from __future__ import annotations

import http.client
import importlib.util
import socket
import sys
import threading
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("http_peer.py")
SPEC = importlib.util.spec_from_file_location("pocketjs_http_peer", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
http_peer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = http_peer
SPEC.loader.exec_module(http_peer)


class QuietSink:
    def emit(self, event: str, **fields: object) -> None:
        del event, fields


class PeerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = http_peer.ThreadingPeerServer(
            ("127.0.0.1", 0),
            body_limit=16 * 1024,
            header_limit=16 * 1024,
            socket_timeout_ms=1000,
            delay_ceiling_ms=2000,
            events=QuietSink(),
        )
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.host, cls.port = cls.server.server_address

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def test_health_echo_and_keep_alive(self) -> None:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=2)
        connection.request("GET", "/health")
        health = connection.getresponse()
        connection_id = health.getheader("X-PocketJS-Connection")
        self.assertEqual(health.status, 200)
        self.assertIn(b'"status":"ok"', health.read())

        payload = b"binary:\x00\xff"
        connection.request("POST", "/echo", body=payload)
        echo = connection.getresponse()
        self.assertEqual(echo.status, 200)
        self.assertEqual(echo.getheader("X-PocketJS-Connection"), connection_id)
        self.assertEqual(echo.read(), payload)
        connection.close()

    def test_valid_chunked_response_has_trailer(self) -> None:
        wire = self.raw_request(b"GET /chunked HTTP/1.1\r\nHost: peer\r\nConnection: close\r\n\r\n")
        self.assertIn(b"Transfer-Encoding: chunked\r\n", wire)
        self.assertIn(b"8\r\nPocketJS\r\n", wire)
        self.assertTrue(wire.endswith(b"0\r\nX-PocketJS-Trailer: complete\r\n\r\n"))

    def test_malformed_te_cl_is_sent_verbatim(self) -> None:
        cases = {
            "te-cl": (b"Transfer-Encoding: chunked\r\n", b"Content-Length: 5\r\n"),
            "duplicate-content-length": (
                b"Content-Length: 5\r\nContent-Length: 5\r\n",
            ),
            "obs-fold": (b"X-PocketJS: first\r\n second\r\n",),
            "te-duplicate": (
                b"Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
            ),
            "te-combined": (b"Transfer-Encoding: gzip, chunked\r\n",),
            "te-unknown": (b"Transfer-Encoding: gzip\r\n",),
            "trailer-forbidden": (b"0\r\nContent-Length: 2\r\n\r\n",),
            "chunk-size": (b"Z\r\ninvalid\r\n",),
        }
        for case, expected_fragments in cases.items():
            with self.subTest(case=case):
                wire = self.raw_request(
                    (
                        f"GET /malformed/{case} HTTP/1.1\r\n"
                        "Host: peer\r\nConnection: close\r\n\r\n"
                    ).encode()
                )
                for fragment in expected_fragments:
                    self.assertIn(fragment, wire)

    def test_disconnect_mid_body_is_incomplete(self) -> None:
        wire = self.raw_request(
            b"GET /disconnect?phase=mid_body HTTP/1.1\r\nHost: peer\r\n\r\n"
        )
        headers, body = wire.split(b"\r\n\r\n", 1)
        self.assertIn(b"Content-Length: 32", headers)
        self.assertEqual(body, b"partial")

    def test_retry_once_counts_without_logging_token(self) -> None:
        first = self.raw_request(
            b"GET /retry-once?token=test-token HTTP/1.1\r\nHost: peer\r\n\r\n"
        )
        self.assertEqual(first, b"")
        second = self.raw_request(
            b"GET /retry-once?token=test-token HTTP/1.1\r\nHost: peer\r\n\r\n"
        )
        self.assertIn(b"X-PocketJS-Attempt: 2\r\n", second)

    def raw_request(self, request: bytes) -> bytes:
        with socket.create_connection((self.host, self.port), timeout=2) as connection:
            connection.sendall(request)
            chunks: list[bytes] = []
            while True:
                chunk = connection.recv(4096)
                if not chunk:
                    return b"".join(chunks)
                chunks.append(chunk)


if __name__ == "__main__":
    unittest.main()
