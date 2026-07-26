from __future__ import annotations

import socket
import struct
import tempfile
import threading
import time
import unittest
from collections import deque
from pathlib import Path

from ec_systemcore import (
    AckStatus,
    ClientRole,
    HealthState,
    SystemCoreClient,
    SystemCoreConfig,
    SystemCoreListener,
)
from ec_systemcore import _protocol as wire


ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "protocol-v1-golden.properties"


def _vectors() -> dict[str, str]:
    result: dict[str, str] = {}
    for line in VECTORS.read_text(encoding="ascii").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            result[key] = value
    return result


class ProtocolTests(unittest.TestCase):
    def test_cross_language_golden_vectors(self) -> None:
        vectors = _vectors()
        epoch = 0x1122334455667788
        hello = wire.hello(ClientRole.CONTROLLER, True, 20, 0x0102030405060708)
        self.assertEqual(hello.hex(), vectors["hello.controller.payload"])
        self.assertEqual(
            wire.frame(wire.HELLO, 0, hello).hex(),
            vectors["hello.controller.frame"],
        )
        self.assertEqual(wire.epoch(epoch).hex(), vectors["heartbeat.payload"])
        self.assertEqual(
            wire.frame(wire.HEARTBEAT, 0xA1B2C3D4, wire.epoch(epoch)).hex(),
            vectors["heartbeat.frame"],
        )
        self.assertEqual(
            wire.output_enable(epoch, True).hex(),
            vectors["output_enable.payload"],
        )
        self.assertEqual(
            wire.output_write(epoch, 2, 3, 0, bytes.fromhex("deadbeef")).hex(),
            vectors["output_write.payload"],
        )
        self.assertEqual(
            struct.pack("<HHI", AckStatus.OK, 0, 0x12345678).hex(),
            vectors["ack.ok.payload"],
        )
        self.assertEqual(
            struct.pack("<HHQ", 10, 0xFFFF, epoch).hex(),
            vectors["outputs_disabled.timeout.payload"],
        )
        self.assertEqual(
            (
                struct.pack("<HHIIHHQQ", 2, 3, 0, 4, 4, 0, epoch, 1)
                + bytes.fromhex("deadbeef")
            ).hex(),
            vectors["pdo_input.payload"],
        )

    def test_fragmentation_coalescing_and_malformed_input(self) -> None:
        first = wire.frame(wire.ACK, 7, struct.pack("<HHI", 0, 0, 9))
        second = wire.frame(wire.OUTPUTS_DISABLED, 0, struct.pack("<HHQ", 4, 2, 11))
        decoder = wire.FrameDecoder()
        frames: list[wire.Frame] = []
        for value in first:
            frames.extend(decoder.feed(bytes((value,))))
        self.assertEqual([(item.message_type, item.request_id) for item in frames], [(wire.ACK, 7)])
        self.assertEqual(len(decoder.feed(first + second)), 2)
        with self.assertRaises(wire.ProtocolError):
            wire.FrameDecoder().feed(struct.pack("<I", wire.MAXIMUM_FRAME_BODY + 1))
        with self.assertRaises(wire.ProtocolError):
            wire.decode_ack(struct.pack("<HHI", 0, 1, 0))
        with self.assertRaises(wire.ProtocolError):
            wire.decode_pdo(
                struct.pack("<HHIIHHQQ", 0, 1, 0, 1, 2, 0, 1, 1) + b"x",
                32,
            )

    def test_configuration_bounds(self) -> None:
        with self.assertRaises(ValueError):
            SystemCoreConfig(request_timeout=31)
        with self.assertRaises(ValueError):
            SystemCoreConfig(maximum_reconnect_delay=61)
        with self.assertRaises(ValueError):
            SystemCoreConfig(socket_path=Path("/" + "x" * 108))


class _BlockingListener(SystemCoreListener):
    def __init__(self) -> None:
        self.entered = threading.Event()
        self.release = threading.Event()

    def on_status(self, status: object) -> None:
        self.entered.set()
        self.release.wait(5)


class _Connection:
    def __init__(self, active: socket.socket) -> None:
        self.active = active
        self.active.settimeout(2)
        self.decoder = wire.FrameDecoder()
        self.ready: deque[wire.Frame] = deque()

    def read(self) -> wire.Frame:
        while not self.ready:
            data = self.active.recv(8192)
            if not data:
                raise EOFError
            self.ready.extend(self.decoder.feed(data))
        return self.ready.popleft()

    def send(self, message_type: int, request_id: int, payload: bytes) -> None:
        self.active.sendall(wire.frame(message_type, request_id, payload))

    def ack(self, request: wire.Frame, status: AckStatus = AckStatus.OK) -> None:
        self.send(wire.ACK, request.request_id, struct.pack("<HHI", status, 0, 0))


@unittest.skipUnless(hasattr(socket, "AF_UNIX"), "Unix-domain sockets unavailable")
class ClientIntegrationTests(unittest.TestCase):
    def test_completion_isolation_reconnect_and_no_replay(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ecsc-py-")
        self.addCleanup(temporary.cleanup)
        socket_path = Path(temporary.name) / "d.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(server.close)
        try:
            server.bind(str(socket_path))
        except OSError as error:
            self.skipTest(f"Unix-domain socket bind unavailable: {error}")
        server.listen(2)
        server.settimeout(5)
        failure: list[BaseException] = []
        replay_seen = threading.Event()

        def daemon() -> None:
            try:
                first_socket, _ = server.accept()
                with first_socket:
                    first = _Connection(first_socket)
                    self.assertEqual(first.read().message_type, wire.HELLO)
                    first.send(
                        wire.HELLO_ACK,
                        0,
                        struct.pack("<BBHQII", ClientRole.CONTROLLER, 0, 250, 41, 1, 0xF),
                    )
                    while True:
                        request = first.read()
                        if request.message_type == wire.HEARTBEAT:
                            first.ack(request)
                        elif request.message_type == wire.OUTPUT_ENABLE and request.payload[8]:
                            ack = wire.frame(
                                wire.ACK,
                                request.request_id,
                                struct.pack("<HHI", AckStatus.OK, 0, 0),
                            )
                            status = wire.frame(wire.STATUS, 0, _status(41, False))
                            first_socket.sendall(ack + status)
                        elif request.message_type == wire.OUTPUT_WRITE:
                            first.ack(request)
                        elif request.message_type == wire.OUTPUT_ENABLE:
                            # Cancellation closes this generation.
                            pass
            except EOFError:
                pass
            try:
                second_socket, _ = server.accept()
                with second_socket:
                    second = _Connection(second_socket)
                    self.assertEqual(second.read().message_type, wire.HELLO)
                    second.send(
                        wire.HELLO_ACK,
                        0,
                        struct.pack("<BBHQII", ClientRole.CONTROLLER, 0, 250, 42, 2, 0xF),
                    )
                    deadline = time.monotonic() + 0.4
                    second_socket.settimeout(0.05)
                    while time.monotonic() < deadline:
                        try:
                            request = second.read()
                        except socket.timeout:
                            continue
                        if request.message_type == wire.HEARTBEAT:
                            second.ack(request)
                        elif request.message_type in {wire.OUTPUT_ENABLE, wire.OUTPUT_WRITE}:
                            replay_seen.set()
            except BaseException as error:
                failure.append(error)

        daemon_thread = threading.Thread(target=daemon, daemon=True)
        daemon_thread.start()
        listener = _BlockingListener()
        client = SystemCoreClient.connect(
            SystemCoreConfig(
                socket_path=socket_path,
                requested_role=ClientRole.CONTROLLER,
                request_timeout=0.5,
                minimum_reconnect_delay=0.02,
                maximum_reconnect_delay=0.05,
                listener=listener,
            )
        )
        self.addCleanup(client.close)
        _wait(lambda: client.health.state is HealthState.CONTROLLING_DISABLED)
        enabled = client.enable_outputs()
        self.assertTrue(listener.entered.wait(2))
        self.assertTrue(enabled.result(2).ok)
        self.assertTrue(client.write_output(0, 1, 0, b"\x01\x02").result(2).ok)
        first_generation = client.generation
        disabled = client.disable_outputs()
        disabled.cancel()
        _wait(
            lambda: client.generation > first_generation
            and client.health.state is HealthState.CONTROLLING_DISABLED
        )
        self.assertFalse(client.outputs_enabled)
        time.sleep(0.45)
        self.assertFalse(replay_seen.is_set())
        listener.release.set()
        client.close()
        daemon_thread.join(5)
        self.assertFalse(daemon_thread.is_alive())
        if failure:
            raise failure[0]


def _status(epoch: int, enabled: bool) -> bytes:
    flags = 1 | 4 | (2 if enabled else 0)
    return struct.pack(
        "<BBHHHIIQQQIHBBQ",
        4,
        flags,
        1,
        1,
        0,
        10,
        5,
        0,
        0,
        epoch,
        1,
        1,
        0,
        0,
        10_000,
    )


def _wait(condition: object, timeout: float = 5) -> None:
    assert callable(condition)
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() >= deadline:
            raise AssertionError("condition timed out")
        time.sleep(0.002)


if __name__ == "__main__":
    unittest.main()
