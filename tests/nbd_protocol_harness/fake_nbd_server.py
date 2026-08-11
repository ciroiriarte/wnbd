#!/usr/bin/env python3
"""Minimal NBD protocol assertion server for WNBD tests.

This is intentionally not a full NBD implementation.  It accepts one client,
performs a small fixed-newstyle handshake, records raw transmission requests,
and can assert command fields that black-box qemu tests do not expose.

The script is useful both as a standalone local harness and as a building block
for future GoogleTest/CI integration.
"""

from __future__ import annotations

import argparse
import enum
import errno
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Iterable, Optional

INIT_PASSWD = b"NBDMAGIC"
OPTION_MAGIC = 0x49484156454F5054
REPLY_MAGIC = 0x3E889045565A9
NBD_REQUEST_MAGIC = 0x25609513
NBD_REPLY_MAGIC = 0x67446698

NBD_FLAG_FIXED_NEWSTYLE = 1
NBD_FLAG_NO_ZEROES = 2

NBD_FLAG_HAS_FLAGS = 1 << 0
NBD_FLAG_READ_ONLY = 1 << 1
NBD_FLAG_SEND_FLUSH = 1 << 2
NBD_FLAG_SEND_FUA = 1 << 3
NBD_FLAG_SEND_TRIM = 1 << 5

NBD_OPT_EXPORT_NAME = 1
NBD_OPT_GO = 7
NBD_REP_ACK = 1
NBD_REP_INFO = 3
NBD_INFO_EXPORT = 0

NBD_CMD_READ = 0
NBD_CMD_WRITE = 1
NBD_CMD_DISC = 2
NBD_CMD_FLUSH = 3
NBD_CMD_TRIM = 4


class Scenario(enum.Enum):
    RECORD = "record"
    ASSERT_DISC = "assert-disc"
    ASSERT_FLUSH_ZERO = "assert-flush-zero"
    CLOSE_AFTER_WRITE_READ = "close-after-write-read"
    ERROR_AFTER_WRITE_READ = "error-after-write-read"
    STALL_AFTER_WRITE_READ = "stall-after-write-read"
    TRUNCATE_AFTER_WRITE_READ = "truncate-after-write-read"
    UNEXPECTED_HANDLE_AFTER_WRITE_READ = "unexpected-handle-after-write-read"
    INVALID_INIT_MAGIC = "invalid-init-magic"
    ACK_WITHOUT_EXPORT = "ack-without-export"
    TRUNCATED_INFO = "truncated-info"


@dataclass(frozen=True)
class NbdRequest:
    flags: int
    command: int
    cookie: int
    offset: int
    length: int
    payload: bytes = b""

    @property
    def is_disc(self) -> bool:
        return self.command == NBD_CMD_DISC

    @property
    def is_flush(self) -> bool:
        return self.command == NBD_CMD_FLUSH


def recv_exact(conn: socket.socket, length: int) -> bytes:
    chunks = []
    remaining = length
    while remaining:
        chunk = conn.recv(remaining)
        if not chunk:
            raise EOFError(f"peer closed while {remaining} byte(s) remained")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_option(conn: socket.socket) -> tuple[int, bytes]:
    magic, option, length = struct.unpack(">QII", recv_exact(conn, 16))
    if magic != OPTION_MAGIC:
        raise AssertionError(f"unexpected option magic: 0x{magic:x}")
    return option, recv_exact(conn, length)


def send_option_reply(conn: socket.socket, option: int, reply_type: int, payload: bytes = b"") -> None:
    conn.sendall(struct.pack(">QIII", REPLY_MAGIC, option, reply_type, len(payload)))
    conn.sendall(payload)


def export_info_payload(size: int, flags: int) -> bytes:
    return struct.pack(">H Q H", NBD_INFO_EXPORT, size, flags)


def send_simple_reply(conn: socket.socket, cookie: int, error: int = 0) -> None:
    conn.sendall(struct.pack(">IIQ", NBD_REPLY_MAGIC, error, cookie))


def perform_handshake(conn: socket.socket, scenario: Scenario, export_size: int) -> None:
    if scenario == Scenario.INVALID_INIT_MAGIC:
        # Send a complete otherwise-valid newstyle prefix after the bad magic so
        # a client must reject the mismatched magic itself rather than merely
        # failing on EOF. If the client ignores the bad magic and sends client
        # flags/options, fail the harness explicitly.
        conn.sendall(b"BADMAGIC")
        conn.sendall(struct.pack(">QH", OPTION_MAGIC, NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES))
        try:
            leaked = conn.recv(4)
        except socket.timeout:
            return
        if leaked:
            raise AssertionError("client continued negotiation after invalid initial magic")
        return

    conn.sendall(INIT_PASSWD)
    conn.sendall(struct.pack(">QH", OPTION_MAGIC, NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES))

    client_flags = struct.unpack(">I", recv_exact(conn, 4))[0]
    if not client_flags & NBD_FLAG_FIXED_NEWSTYLE:
        raise AssertionError("client did not request fixed-newstyle handshake")

    option, payload = read_option(conn)
    if option == NBD_OPT_EXPORT_NAME:
        # NBD_OPT_EXPORT_NAME is the legacy negotiation path: send export size/flags directly.
        conn.sendall(struct.pack(">QH", export_size, 0))
        return
    if option != NBD_OPT_GO:
        raise AssertionError(f"expected NBD_OPT_GO, got {option}")

    if scenario == Scenario.ACK_WITHOUT_EXPORT:
        send_option_reply(conn, option, NBD_REP_ACK)
        return
    if scenario == Scenario.TRUNCATED_INFO:
        send_option_reply(conn, option, NBD_REP_INFO, b"\x00")
        return

    send_option_reply(conn, option, NBD_REP_INFO, export_info_payload(export_size, NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_TRIM | NBD_FLAG_SEND_FUA))
    send_option_reply(conn, option, NBD_REP_ACK)


def read_request(conn: socket.socket) -> NbdRequest:
    magic, flags, command, cookie, offset, length = struct.unpack(">IHHQQI", recv_exact(conn, 28))
    if magic != NBD_REQUEST_MAGIC:
        raise AssertionError(f"unexpected request magic: 0x{magic:x}")
    payload = b""
    if command == NBD_CMD_WRITE and length:
        payload = recv_exact(conn, length)
    return NbdRequest(
        flags=flags, command=command, cookie=cookie, offset=offset,
        length=length, payload=payload)


def in_bounds(req: NbdRequest, export_size: int) -> bool:
    return req.offset <= export_size and req.length <= export_size - req.offset


def wait_for_peer_close(conn: socket.socket, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if not conn.recv(1):
                return
        except socket.timeout:
            continue
        except OSError:
            return


def serve(args: argparse.Namespace) -> int:
    scenario = Scenario(args.scenario)
    requests: list[NbdRequest] = []
    storage = bytearray(args.export_size)
    fault_armed = False
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.settimeout(args.timeout)
        server.bind((args.host, args.port))
        server.listen(1)
        print(f"fake NBD server listening on {args.host}:{server.getsockname()[1]}", flush=True)
        conn, addr = server.accept()
        with conn:
            conn.settimeout(args.timeout)
            perform_handshake(conn, scenario, args.export_size)
            if scenario in {Scenario.INVALID_INIT_MAGIC, Scenario.ACK_WITHOUT_EXPORT, Scenario.TRUNCATED_INFO}:
                return 0
            for _ in range(args.max_requests):
                try:
                    req = read_request(conn)
                except EOFError:
                    break
                requests.append(req)
                print(
                    f"request command={req.command} flags={req.flags} "
                    f"cookie={req.cookie} offset={req.offset} length={req.length}",
                    flush=True,
                )
                if scenario == Scenario.ASSERT_DISC and req.is_disc:
                    if req.offset != 0 or req.length != 0:
                        raise AssertionError("NBD_CMD_DISC must use zero offset/length")
                    return 0
                if scenario == Scenario.ASSERT_FLUSH_ZERO and req.is_flush:
                    if req.offset != 0 or req.length != 0:
                        raise AssertionError("NBD_CMD_FLUSH must use zero offset/length")
                    send_simple_reply(conn, req.cookie)
                    return 0
                if req.is_disc:
                    return 0

                if req.command == NBD_CMD_WRITE:
                    if not in_bounds(req, args.export_size):
                        send_simple_reply(conn, req.cookie, errno.EINVAL)
                    else:
                        storage[req.offset:req.offset + req.length] = req.payload
                        fault_armed = True
                        send_simple_reply(conn, req.cookie)
                    continue

                if req.command == NBD_CMD_READ:
                    if fault_armed and scenario == Scenario.CLOSE_AFTER_WRITE_READ:
                        return 0
                    if fault_armed and scenario == Scenario.UNEXPECTED_HANDLE_AFTER_WRITE_READ:
                        send_simple_reply(conn, req.cookie + 1)
                        return 0
                    if fault_armed and scenario == Scenario.ERROR_AFTER_WRITE_READ:
                        send_simple_reply(conn, req.cookie, errno.EIO)
                        return 0
                    if fault_armed and scenario == Scenario.STALL_AFTER_WRITE_READ:
                        wait_for_peer_close(conn, args.stall_seconds)
                        return 0
                    if not in_bounds(req, args.export_size):
                        send_simple_reply(conn, req.cookie, errno.EINVAL)
                    else:
                        send_simple_reply(conn, req.cookie)
                        payload = bytes(storage[req.offset:req.offset + req.length])
                        if fault_armed and scenario == Scenario.TRUNCATE_AFTER_WRITE_READ:
                            conn.sendall(payload[:max(0, len(payload) // 2)])
                            return 0
                        conn.sendall(payload)
                    continue

                if req.command == NBD_CMD_TRIM:
                    if not in_bounds(req, args.export_size):
                        send_simple_reply(conn, req.cookie, errno.EINVAL)
                    else:
                        storage[req.offset:req.offset + req.length] = bytes(req.length)
                        send_simple_reply(conn, req.cookie)
                    continue

                send_simple_reply(conn, req.cookie)

    if scenario == Scenario.ASSERT_DISC and not any(r.is_disc for r in requests):
        raise AssertionError("client never sent NBD_CMD_DISC")
    if scenario == Scenario.ASSERT_FLUSH_ZERO and not any(r.is_flush for r in requests):
        raise AssertionError("client never sent NBD_CMD_FLUSH")
    return 0


def parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--export-size", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--max-requests", type=int, default=128)
    parser.add_argument("--stall-seconds", type=float, default=5.0)
    parser.add_argument("--scenario", choices=[s.value for s in Scenario], default=Scenario.RECORD.value)
    parser.add_argument("--self-test", action="store_true", help="run a loopback harness smoke test and exit")
    return parser.parse_args(argv)


def read_listening_port(proc: subprocess.Popen[str]) -> int:
    line = proc.stdout.readline().strip() if proc.stdout else ""
    if "listening" not in line:
        raise AssertionError(f"server did not start: {line}")
    return int(line.rsplit(":", 1)[1])


def self_test() -> int:
    proc = subprocess.Popen(
        [sys.executable, __file__, "--port", "0", "--scenario", "record", "--max-requests", "1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        port = read_listening_port(proc)
        with socket.create_connection(("127.0.0.1", port), timeout=2) as conn:
            if recv_exact(conn, 8) != INIT_PASSWD:
                raise AssertionError("bad server initial magic")
            magic, flags = struct.unpack(">QH", recv_exact(conn, 10))
            if magic != OPTION_MAGIC or not (flags & NBD_FLAG_FIXED_NEWSTYLE):
                raise AssertionError("bad server newstyle prefix")
            conn.sendall(struct.pack(">I", NBD_FLAG_FIXED_NEWSTYLE))
            payload = struct.pack(">I", 4) + b"test" + struct.pack(">H", 0)
            conn.sendall(struct.pack(">QII", OPTION_MAGIC, NBD_OPT_GO, len(payload)) + payload)
            reply = recv_exact(conn, 20)
            _, _, reply_type, length = struct.unpack(">QIII", reply)
            if reply_type != NBD_REP_INFO:
                raise AssertionError("expected NBD_REP_INFO")
            recv_exact(conn, length)
            reply = recv_exact(conn, 20)
            _, _, reply_type, length = struct.unpack(">QIII", reply)
            if reply_type != NBD_REP_ACK or length:
                raise AssertionError("expected empty NBD_REP_ACK")
            conn.sendall(struct.pack(">IHHQQI", NBD_REQUEST_MAGIC, 0, NBD_CMD_FLUSH, 1, 0, 0))
            magic, error, cookie = struct.unpack(">IIQ", recv_exact(conn, 16))
            if magic != NBD_REPLY_MAGIC or error != 0 or cookie != 1:
                raise AssertionError("bad simple reply for FLUSH")
        rc = proc.wait(timeout=2)
        if rc:
            raise AssertionError(proc.stderr.read() if proc.stderr else f"server exited {rc}")

        invalid_proc = subprocess.Popen(
            [sys.executable, __file__, "--port", "0", "--scenario", "invalid-init-magic"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            invalid_port = read_listening_port(invalid_proc)
            with socket.create_connection(("127.0.0.1", invalid_port), timeout=2) as conn:
                recv_exact(conn, 18)
                conn.sendall(struct.pack(">I", NBD_FLAG_FIXED_NEWSTYLE))
            invalid_rc = invalid_proc.wait(timeout=2)
            if invalid_rc == 0:
                raise AssertionError("invalid-magic scenario did not fail when client continued")
        finally:
            if invalid_proc.poll() is None:
                invalid_proc.kill()

        print("fake NBD server self-test OK")
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        return serve(args)
    except Exception as exc:  # pragma: no cover - CLI diagnostic path
        print(f"fake NBD server failed: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
