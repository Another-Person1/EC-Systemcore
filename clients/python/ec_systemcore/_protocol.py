"""Strict, bounded ECSC version 1 wire codec."""

from __future__ import annotations

import struct
from dataclasses import dataclass

VERSION = 1
MAGIC = b"ECSC"
ENVELOPE_BYTES = 12
LENGTH_PREFIX_BYTES = 4
MAXIMUM_FRAME_BODY = 8192
MAXIMUM_BUFFERED_INPUT = 2 * (LENGTH_PREFIX_BYTES + MAXIMUM_FRAME_BODY)
MAXIMUM_OUTPUT_WRITE = 1024
MAXIMUM_PDO_CHUNK = 1024
MAXIMUM_PDO_IMAGE = 1024 * 1024
MAXIMUM_BUS_INFO = 2048

HELLO = 0x01
HEARTBEAT = 0x02
OUTPUT_ENABLE = 0x03
OUTPUT_WRITE = 0x04
CLEAR_COUNTERS = 0x05
RELEASE_CONTROL = 0x06
SUBSCRIBE_INPUTS = 0x07
ADAPTER_UNLOCK = 0x08
ADAPTER_RESCAN = 0x09
HELLO_ACK = 0x80
STATUS = 0x81
ACK = 0x82
PDO_INPUT = 0x83
BUS_INFO = 0x84
ERROR = 0x85
OUTPUTS_DISABLED = 0x86

_ENVELOPE = struct.Struct("<4sBBHI")
_LENGTH = struct.Struct("<I")
_HELLO_ACK = struct.Struct("<BBHQII")
_ACK = struct.Struct("<HHI")
_STATUS = struct.Struct("<BBHHHIIQQQIHBBQ")
_PDO = struct.Struct("<HHIIHHQQ")
_BUS_INFO = struct.Struct("<HBBBBH7HQ")


class ProtocolError(ValueError):
    """Malformed, unsupported, or unsafe ECSC data."""


@dataclass(frozen=True, slots=True)
class Frame:
    message_type: int
    request_id: int
    payload: bytes


@dataclass(frozen=True, slots=True)
class HelloAck:
    role: int
    outputs_enabled: bool
    heartbeat_timeout_ms: int
    epoch: int
    peer_pid: int
    capabilities: int


@dataclass(frozen=True, slots=True)
class PdoChunk:
    bus_index: int
    subdevice_index: int
    offset: int
    total_size: int
    epoch: int
    cycle_sequence: int
    data: bytes


def known_message_type(message_type: int) -> bool:
    return HELLO <= message_type <= ADAPTER_RESCAN or HELLO_ACK <= message_type <= OUTPUTS_DISABLED


def frame(message_type: int, request_id: int, payload: bytes = b"") -> bytes:
    payload = bytes(payload)
    if not known_message_type(message_type):
        raise ValueError(f"unknown ECSC message type {message_type}")
    if not 0 <= request_id <= 0xFFFF_FFFF:
        raise ValueError("request_id must be unsigned 32-bit")
    body_size = ENVELOPE_BYTES + len(payload)
    if body_size > MAXIMUM_FRAME_BODY:
        raise ValueError("frame exceeds ECSC maximum")
    return (
        _LENGTH.pack(body_size)
        + _ENVELOPE.pack(MAGIC, VERSION, message_type, 0, request_id)
        + payload
    )


def hello(role: int, subscribe: bool, period_ms: int, last_seen_epoch: int) -> bytes:
    _range(role, 0, 1, "role")
    _range(period_ms, 10, 1000, "period_ms")
    _u64(last_seen_epoch, "last_seen_epoch", allow_zero=True)
    return struct.pack("<BBHQ", role, int(subscribe), period_ms, last_seen_epoch)


def epoch(value: int) -> bytes:
    _u64(value, "epoch")
    return struct.pack("<Q", value)


def output_enable(value: int, enabled: bool) -> bytes:
    return epoch(value) + bytes((int(enabled),))


def output_write(
    value: int,
    bus_index: int,
    subdevice_index: int,
    offset: int,
    data: bytes,
) -> bytes:
    _u64(value, "epoch")
    _range(bus_index, 0, 0xFFFF, "bus_index")
    _range(subdevice_index, 1, 0xFFFF, "subdevice_index")
    _range(offset, 0, 0xFFFF_FFFF, "offset")
    data = bytes(data)
    if not 1 <= len(data) <= MAXIMUM_OUTPUT_WRITE:
        raise ValueError("output write must contain 1..1024 bytes")
    return (
        struct.pack("<QHHIHH", value, bus_index, subdevice_index, offset, len(data), 0)
        + data
    )


def subscription(enabled: bool, period_ms: int) -> bytes:
    _range(period_ms, 10, 1000, "period_ms")
    return struct.pack("<BBH", int(enabled), 0, period_ms)


def adapter_unlock(value: int, bus_index: int) -> bytes:
    _range(bus_index, 0, 0xFFFF, "bus_index")
    return epoch(value) + struct.pack("<H", bus_index)


def decode_hello_ack(payload: bytes) -> HelloAck:
    if len(payload) != _HELLO_ACK.size:
        raise ProtocolError("HELLO_ACK payload length is invalid")
    role, outputs, timeout, value, peer_pid, capabilities = _HELLO_ACK.unpack(payload)
    if role > 1 or outputs > 1 or timeout == 0 or value == 0:
        raise ProtocolError("HELLO_ACK fields are invalid")
    return HelloAck(role, bool(outputs), timeout, value, peer_pid, capabilities)


def decode_ack(payload: bytes) -> tuple[int, int]:
    if len(payload) != _ACK.size:
        raise ProtocolError("ACK payload length is invalid")
    status, reserved, detail = _ACK.unpack(payload)
    if status > 12 or reserved != 0:
        raise ProtocolError("ACK fields are invalid")
    return status, detail


def decode_status(payload: bytes) -> tuple[int, ...]:
    if len(payload) != _STATUS.size:
        raise ProtocolError("STATUS payload length is invalid")
    values = _STATUS.unpack(payload)
    state, _, _, _, _, _, _, _, _, value, _, _, scheduling, errors, _ = values
    if state > 6 or value == 0 or scheduling > 1 or errors & 0x80:
        raise ProtocolError("STATUS fields are invalid")
    return values


def decode_bus_info(payload: bytes) -> tuple[object, ...]:
    if not 30 <= len(payload) <= MAXIMUM_BUS_INFO:
        raise ProtocolError("BUS_INFO payload length is invalid")
    fixed = _BUS_INFO.unpack_from(payload)
    bus, state, link, lock_state, lock_reason, subdevices, *tail = fixed
    lengths = tail[:7]
    value = tail[7]
    if state > 6 or link > 1 or lock_state > 4 or lock_reason > 9 or value == 0:
        raise ProtocolError("BUS_INFO fields are invalid")
    if 30 + sum(lengths) != len(payload):
        raise ProtocolError("BUS_INFO string lengths are invalid")
    strings: list[str] = []
    offset = 30
    for length in lengths:
        encoded = payload[offset : offset + length]
        offset += length
        try:
            decoded = encoded.decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            raise ProtocolError("BUS_INFO string is not UTF-8") from error
        if "\0" in decoded:
            raise ProtocolError("BUS_INFO string contains NUL")
        strings.append(decoded)
    return (bus, state, link, lock_state, lock_reason, subdevices, value, *strings)


def decode_pdo(payload: bytes, maximum_total_size: int) -> PdoChunk:
    if len(payload) < _PDO.size:
        raise ProtocolError("PDO_INPUT payload is truncated")
    bus, subdevice, offset, total, length, reserved, value, sequence = _PDO.unpack_from(payload)
    if (
        subdevice == 0
        or reserved != 0
        or value == 0
        or sequence == 0
        or total == 0
        or length == 0
        or length > MAXIMUM_PDO_CHUNK
        or total > maximum_total_size
        or offset > total
        or length > total - offset
        or len(payload) != _PDO.size + length
    ):
        raise ProtocolError("PDO_INPUT bounds are invalid")
    return PdoChunk(bus, subdevice, offset, total, value, sequence, payload[_PDO.size :])


def decode_outputs_disabled(payload: bytes) -> tuple[int, int, int]:
    if len(payload) != 12:
        raise ProtocolError("OUTPUTS_DISABLED payload length is invalid")
    reason, bus, value = struct.unpack("<HHQ", payload)
    if not 1 <= reason <= 10 or value == 0:
        raise ProtocolError("OUTPUTS_DISABLED fields are invalid")
    return reason, bus, value


class FrameDecoder:
    """Incremental stream decoder with a hard memory bound."""

    __slots__ = ("_buffer",)

    def __init__(self) -> None:
        self._buffer = bytearray()

    @property
    def buffered_bytes(self) -> int:
        return len(self._buffer)

    def feed(self, data: bytes | bytearray | memoryview) -> list[Frame]:
        if len(data) > MAXIMUM_BUFFERED_INPUT - len(self._buffer):
            self._buffer.clear()
            raise ProtocolError("ECSC input buffer exceeded its limit")
        self._buffer.extend(data)
        decoded: list[Frame] = []
        offset = 0
        while len(self._buffer) - offset >= LENGTH_PREFIX_BYTES:
            (body_size,) = _LENGTH.unpack_from(self._buffer, offset)
            if not ENVELOPE_BYTES <= body_size <= MAXIMUM_FRAME_BODY:
                self._buffer.clear()
                raise ProtocolError("invalid ECSC frame length")
            total_size = LENGTH_PREFIX_BYTES + body_size
            if len(self._buffer) - offset < total_size:
                break
            magic, version, message_type, flags, request_id = _ENVELOPE.unpack_from(
                self._buffer, offset + LENGTH_PREFIX_BYTES
            )
            if (
                magic != MAGIC
                or version != VERSION
                or flags != 0
                or not known_message_type(message_type)
            ):
                self._buffer.clear()
                raise ProtocolError("invalid ECSC envelope")
            payload_offset = offset + LENGTH_PREFIX_BYTES + ENVELOPE_BYTES
            decoded.append(
                Frame(
                    message_type,
                    request_id,
                    bytes(self._buffer[payload_offset : offset + total_size]),
                )
            )
            offset += total_size
        if offset:
            del self._buffer[:offset]
        return decoded


def _range(value: int, minimum: int, maximum: int, field: str) -> None:
    if not minimum <= value <= maximum:
        raise ValueError(f"{field} must be between {minimum} and {maximum}")


def _u64(value: int, field: str, *, allow_zero: bool = False) -> None:
    minimum = 0 if allow_zero else 1
    _range(value, minimum, 0xFFFF_FFFF_FFFF_FFFF, field)
