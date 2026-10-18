#!/usr/bin/env python3
"""Exercise the running native HTTP/WebSocket listener over loopback."""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
from typing import Optional


GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def fail(message: str) -> None:
    raise AssertionError(message)


def recv_until(sock: socket.socket, marker: bytes, initial: bytes = b"") -> bytes:
    data = bytearray(initial)
    while marker not in data:
        chunk = sock.recv(65536)
        if not chunk:
            fail(f"connection closed before {marker!r}")
        data.extend(chunk)
    return bytes(data)


def recv_all(sock: socket.socket, initial: bytes = b"") -> bytes:
    data = bytearray(initial)
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            # Rejecting an oversized request can close with unread input after the response.
            if data:
                return bytes(data)
            raise
        if not chunk:
            return bytes(data)
        data.extend(chunk)


def request(host: str, port: int, wire: bytes, timeout: float = 8.0) -> bytes:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(wire)
        sock.shutdown(socket.SHUT_WR)
        return recv_all(sock)


def response_parts(response: bytes) -> tuple[int, dict[str, str], bytes]:
    head, separator, body = response.partition(b"\r\n\r\n")
    if not separator:
        fail("HTTP response has no header terminator")
    lines = head.split(b"\r\n")
    if len(lines) < 1:
        fail("HTTP response is empty")
    try:
        status = int(lines[0].split(b" ", 2)[1])
    except (IndexError, ValueError) as error:
        raise AssertionError(f"invalid HTTP status line: {lines[0]!r}") from error
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if b":" not in line:
            fail(f"malformed response header: {line!r}")
        name, value = line.split(b":", 1)
        headers[name.decode("ascii").lower()] = value.strip().decode("latin1")
    return status, headers, body


def assert_length(headers: dict[str, str], body: bytes) -> None:
    try:
        length = int(headers["content-length"])
    except (KeyError, ValueError) as error:
        raise AssertionError("response has no valid Content-Length") from error
    if length != len(body):
        fail(f"Content-Length {length} does not match body length {len(body)}")


class Rc4:
    def __init__(self, key: bytes) -> None:
        self.box = list(range(256))
        j = 0
        for i in range(256):
            j = (j + self.box[i] + key[i % len(key)]) % 256
            self.box[i], self.box[j] = self.box[j], self.box[i]
        self.a = 0
        self.b = 0

    def crypt(self, data: bytes) -> bytes:
        output = bytearray(data)
        for i in range(len(output)):
            self.a = (self.a + 1) % 256
            self.b = (self.b + self.box[self.a]) % 256
            self.box[self.a], self.box[self.b] = self.box[self.b], self.box[self.a]
            output[i] ^= self.box[(self.box[self.a] + self.box[self.b]) % 256]
        return bytes(output)


def masked_binary_frame(payload: bytes) -> bytes:
    mask = os.urandom(4)
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    length = len(payload)
    if length < 126:
        header = bytes((0x82, 0x80 | length))
    elif length <= 0xFFFF:
        header = bytes((0x82, 0xFE)) + struct.pack("!H", length)
    else:
        header = bytes((0x82, 0xFF)) + struct.pack("!Q", length)
    return header + mask + masked


def read_server_frame(sock: socket.socket, data: bytes) -> bytes:
    def need(size: int) -> bytes:
        nonlocal data
        while len(data) < size:
            chunk = sock.recv(65536)
            if not chunk:
                fail("connection closed in WebSocket frame")
            data += chunk
        result, data = data[:size], data[size:]
        return result

    first = need(2)
    if first[0] & 0x0F != 2 or not first[0] & 0x80:
        fail(f"unexpected WebSocket frame: {first[0]:#x}")
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", need(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", need(8))[0]
    if first[1] & 0x80:
        fail("server WebSocket frame is unexpectedly masked")
    return need(length)


def check_websocket(host: str, port: int, password: Optional[str], timeout: float) -> None:
    key = base64.b64encode(os.urandom(16))
    request = json.dumps(
        {"id": 1, "module": "control", "function": "session_refresh", "params": []},
        separators=(",", ":"),
    ).encode() + b"\0"
    cipher = Rc4(password.encode()) if password else None
    wire_payload = cipher.crypt(request) if cipher else request
    frame = masked_binary_frame(wire_payload)
    wire = (
        b"GET / HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Version: 13\r\n"
        b"Sec-WebSocket-Key: " + key + b"\r\n\r\n" + frame
    )

    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(wire)
        received = recv_until(sock, b"\r\n\r\n")
        head, _, tail = received.partition(b"\r\n\r\n")
        status, headers, _ = response_parts(head + b"\r\n\r\n")
        if status != 101:
            fail(f"WebSocket handshake returned {status}")
        expected = base64.b64encode(hashlib.sha1(key + GUID).digest()).decode("ascii")
        if headers.get("sec-websocket-accept") != expected:
            fail("WebSocket accept key mismatch")
        response = read_server_frame(sock, tail)

    if cipher:
        response = cipher.crypt(response)
    if not response.endswith(b"\0"):
        fail("API WebSocket response is not NULL terminated")
    decoded = json.loads(response[:-1].decode("utf-8"))
    data = decoded.get("data")
    if (decoded.get("id") != 1 or decoded.get("errors") != []
            or not isinstance(data, list) or len(data) != 1
            or not isinstance(data[0], str) or len(data[0]) != 256):
        fail(f"unexpected API response: {decoded!r}")


def check_stream(host: str, port: int, expected: bool, timeout: float) -> None:
    wire = (
        b"GET /stream.mjpg?screen=0&q=80&fps=1 HTTP/1.1\r\n"
        b"Host: localhost\r\nConnection: close\r\n\r\n"
    )
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(wire)
        received = recv_until(sock, b"\r\n\r\n")
    status, headers, _ = response_parts(received)
    if expected:
        if status != 200 or not headers.get("content-type", "").startswith("multipart/"):
            fail(f"same-port MJPEG did not start: {status}, {headers}")
    elif status != 404:
        fail(f"disabled same-port MJPEG returned {status}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1338, help="WebSocket/API+1 port")
    parser.add_argument("--password", help="API password, when the server uses one")
    parser.add_argument("--expect-stream", action="store_true")
    args = parser.parse_args()
    timeout = 8.0

    get = request(
        args.host, args.port,
        b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
    )
    status, headers, body = response_parts(get)
    if status != 200 or not headers.get("content-type", "").startswith("text/html"):
        fail(f"GET / returned {status}, {headers}")
    assert_length(headers, body)
    if not body:
        fail("GET / returned an empty HTML resource")

    index = request(
        args.host, args.port,
        b"GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
    )
    index_status, index_headers, index_body = response_parts(index)
    if index_status != 200 or index_body != body:
        fail("GET /index.html differs from GET /")
    assert_length(index_headers, index_body)

    head = request(
        args.host, args.port,
        b"HEAD /index.html HTTP/1.0\r\n\r\n",
    )
    head_status, head_headers, head_body = response_parts(head)
    if head_status != 200 or head_body:
        fail(f"HEAD /index.html returned body or status {head_status}")
    if int(head_headers.get("content-length", "-1")) != len(body):
        fail("HEAD Content-Length differs from GET")

    unknown = request(args.host, args.port, b"GET /missing HTTP/1.1\r\n\r\n")
    if response_parts(unknown)[0] != 404:
        fail("unknown path did not return 404")
    wrong_method = request(args.host, args.port, b"POST / HTTP/1.1\r\n\r\n")
    if response_parts(wrong_method)[0] != 405:
        fail("POST / did not return 405")
    malformed = request(args.host, args.port, b"GET / HTTP/1.1\r\nHost localhost\r\n\r\n")
    if response_parts(malformed)[0] != 400:
        fail("malformed header did not return 400")
    oversized = request(args.host, args.port, b"GET / HTTP/1.1\r\nX-Fill: " + b"x" * 9000)
    if response_parts(oversized)[0] != 400:
        fail("oversized header did not return 400")

    check_websocket(args.host, args.port, args.password, timeout)
    check_stream(args.host, args.port, args.expect_stream, timeout)
    print("native HTTP/WebSocket socket contracts passed")


if __name__ == "__main__":
    main()
