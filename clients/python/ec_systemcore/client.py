"""Reconnect-safe, bounded Python ECSC v1 client."""

from __future__ import annotations

import errno
import os
import queue
import select
import socket
import threading
import time
from collections import deque
from concurrent.futures import Future, InvalidStateError
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from types import MappingProxyType
from typing import Callable, Mapping

from . import _protocol as wire


class ClientRole(IntEnum):
    OBSERVER = 0
    CONTROLLER = 1


class AckStatus(IntEnum):
    OK = 0
    MALFORMED = 1
    UNAUTHORIZED = 2
    BUSY = 3
    DISABLED = 4
    BAD_TARGET = 5
    BOUNDS = 6
    STALE_EPOCH = 7
    UNSUPPORTED = 8
    ADAPTER_LOCKED = 9
    TOPOLOGY_MISMATCH = 10
    QUEUE_FULL = 11
    INTERNAL_ERROR = 12


class HealthState(IntEnum):
    STOPPED = 0
    CONNECTING = 1
    HANDSHAKING = 2
    OBSERVING = 3
    CONTROLLING_DISABLED = 4
    CONTROLLING_ENABLED = 5
    DEGRADED = 6


@dataclass(frozen=True, slots=True)
class Ack:
    status: AckStatus
    detail: int

    @property
    def ok(self) -> bool:
        return self.status is AckStatus.OK


@dataclass(frozen=True, slots=True)
class Health:
    state: HealthState
    message: str
    generation: int
    changed_monotonic: float
    cause: BaseException | None = None


@dataclass(frozen=True, slots=True)
class Status:
    aggregate_state: int
    operational: bool
    outputs_enabled: bool
    controller_connected: bool
    preempt_rt_available: bool
    realtime_applied: bool
    realtime_requested: bool
    timing_degraded: bool
    reinitializing: bool
    active_adapters: int
    subdevice_count: int
    active_faults: int
    maximum_jitter_us: int
    current_jitter_us: int
    lost_frames: int
    cycle_overruns: int
    epoch: int
    controller_pid: int
    configured_buses: int
    scheduling_fifo: bool
    realtime_error_bits: int
    monotonic_timestamp_us: int
    received_monotonic: float


@dataclass(frozen=True, slots=True)
class AdapterIdentity:
    id_path: str
    permanent_mac: str
    usb_serial: str
    usb_vendor_id: str
    usb_product_id: str


@dataclass(frozen=True, slots=True)
class BusInfo:
    bus_index: int
    state: int
    link_up: bool
    lock_state: int
    lock_reason: int
    subdevice_count: int
    epoch: int
    logical_name: str
    physical_interface: str
    identity: AdapterIdentity


@dataclass(frozen=True, slots=True)
class PdoInput:
    bus_index: int
    subdevice_index: int
    cycle_sequence: int
    epoch: int
    data: bytes
    received_monotonic: float


@dataclass(frozen=True, slots=True)
class OutputsDisabled:
    reason: int
    bus_index: int
    epoch: int
    received_monotonic: float


class SystemCoreListener:
    """Override the callbacks of interest; exceptions are isolated from IPC."""

    def on_health_changed(self, health: Health) -> None:
        pass

    def on_status(self, status: Status) -> None:
        pass

    def on_bus_info(self, bus_info: BusInfo) -> None:
        pass

    def on_pdo_input(self, pdo_input: PdoInput) -> None:
        pass

    def on_outputs_disabled(self, disabled: OutputsDisabled) -> None:
        pass


@dataclass(frozen=True, slots=True)
class SystemCoreConfig:
    socket_path: Path = Path("/run/ec-systemcore/ec-systemcore.sock")
    requested_role: ClientRole = ClientRole.OBSERVER
    subscribe_inputs: bool = False
    input_period_ms: int = 100
    minimum_reconnect_delay: float = 0.250
    maximum_reconnect_delay: float = 5.0
    request_timeout: float = 2.0
    maximum_queued_commands: int = 128
    maximum_outbound_bytes: int = 64 * 1024
    maximum_pdo_image_bytes: int = wire.MAXIMUM_PDO_IMAGE
    maximum_pdo_assemblies: int = 32
    maximum_pdo_assembly_bytes: int = 4 * 1024 * 1024
    maximum_tracked_buses: int = 32
    maximum_tracked_pdo_inputs: int = 256
    listener: SystemCoreListener = field(default_factory=SystemCoreListener)

    def __post_init__(self) -> None:
        path = Path(os.path.abspath(os.fspath(self.socket_path)))
        if "\0" in os.fspath(path) or len(os.fsencode(path)) > 107:
            raise ValueError("socket_path must fit the Linux sockaddr_un path limit")
        object.__setattr__(self, "socket_path", path)
        if not isinstance(self.requested_role, ClientRole):
            raise ValueError("requested_role must be a ClientRole")
        _bounded(self.input_period_ms, 10, 1000, "input_period_ms")
        _bounded(self.minimum_reconnect_delay, 0.001, 60.0, "minimum_reconnect_delay")
        _bounded(self.maximum_reconnect_delay, 0.001, 60.0, "maximum_reconnect_delay")
        if self.maximum_reconnect_delay < self.minimum_reconnect_delay:
            raise ValueError("maximum_reconnect_delay must be at least the minimum")
        _bounded(self.request_timeout, 0.010, 30.0, "request_timeout")
        _bounded(self.maximum_queued_commands, 1, 4096, "maximum_queued_commands")
        _bounded(self.maximum_outbound_bytes, 8192, 1024 * 1024, "maximum_outbound_bytes")
        _bounded(
            self.maximum_pdo_image_bytes,
            1,
            wire.MAXIMUM_PDO_IMAGE,
            "maximum_pdo_image_bytes",
        )
        _bounded(self.maximum_pdo_assemblies, 1, 256, "maximum_pdo_assemblies")
        _bounded(
            self.maximum_pdo_assembly_bytes,
            self.maximum_pdo_image_bytes,
            64 * 1024 * 1024,
            "maximum_pdo_assembly_bytes",
        )
        _bounded(self.maximum_tracked_buses, 1, 256, "maximum_tracked_buses")
        _bounded(
            self.maximum_tracked_pdo_inputs,
            1,
            4096,
            "maximum_tracked_pdo_inputs",
        )
        if not isinstance(self.listener, SystemCoreListener):
            raise ValueError("listener must be a SystemCoreListener")


@dataclass(frozen=True, slots=True)
class _Session:
    generation: int
    ready: bool = False
    role: ClientRole = ClientRole.OBSERVER
    epoch: int = 0
    outputs_enabled: bool = False
    disable_barrier: bool = True


@dataclass(slots=True)
class _Command:
    generation: int
    epoch: int
    message_type: int
    payload: bytes
    kind: str
    result: "_CommandFuture"


@dataclass(slots=True)
class _Pending:
    kind: str
    result: "_CommandFuture | None"
    deadline: float
    generation: int
    epoch: int


class _CommandFuture(Future[Ack]):
    def __init__(
        self,
        release_slot: Callable[[], None],
        safety_cancel: Callable[[], None] | None,
    ) -> None:
        super().__init__()
        self._release_slot = release_slot
        self._safety_cancel = safety_cancel
        self._retired = False
        self._retire_lock = threading.Lock()

    def cancel(self) -> bool:
        cancelled = super().cancel()
        if cancelled and self._safety_cancel is not None:
            self._safety_cancel()
        return cancelled

    def set_result(self, result: Ack) -> None:
        try:
            super().set_result(result)
        except InvalidStateError:
            pass
        finally:
            self.retire()

    def set_exception(self, exception: BaseException) -> None:
        try:
            super().set_exception(exception)
        except InvalidStateError:
            pass
        finally:
            self.retire()

    def retire(self) -> None:
        with self._retire_lock:
            if self._retired:
                return
            self._retired = True
        self._release_slot()


class _Dispatcher:
    def __init__(self, name: str, workers: int, capacity: int) -> None:
        self._queue: queue.Queue[Callable[[], None]] = queue.Queue(capacity)
        self._closed = threading.Event()
        self._threads: list[threading.Thread] = []
        for index in range(workers):
            thread = threading.Thread(
                target=self._run,
                name=f"{name}-{index}",
                daemon=True,
            )
            thread.start()
            self._threads.append(thread)

    def submit(self, action: Callable[[], None], *, critical: bool = False) -> bool:
        if self._closed.is_set():
            return False
        try:
            self._queue.put_nowait(action)
            return True
        except queue.Full:
            if not critical:
                return False
            try:
                self._queue.get_nowait()
                self._queue.task_done()
                self._queue.put_nowait(action)
                return True
            except (queue.Empty, queue.Full):
                return False

    def close(self) -> None:
        self._closed.set()

    def _run(self) -> None:
        while not self._closed.is_set() or not self._queue.empty():
            try:
                action = self._queue.get(timeout=0.050)
            except queue.Empty:
                continue
            try:
                action()
            except BaseException:
                pass
            finally:
                self._queue.task_done()


class _PdoAssembler:
    def __init__(self, maximum_assemblies: int, maximum_bytes: int) -> None:
        self._maximum_assemblies = maximum_assemblies
        self._maximum_bytes = maximum_bytes
        self._assemblies: dict[tuple[int, int, int, int], list[object]] = {}
        self._delivered: dict[tuple[int, int, int], int] = {}
        self._allocated = 0

    def accept(self, chunk: wire.PdoChunk, now: float) -> PdoInput | None:
        target = (chunk.bus_index, chunk.subdevice_index, chunk.epoch)
        delivered = self._delivered.get(target)
        if delivered is not None and chunk.cycle_sequence <= delivered:
            return None
        key = (*target, chunk.cycle_sequence)
        assembly = self._assemblies.get(key)
        allocation = chunk.total_size * 2
        if assembly is None:
            if (
                len(self._assemblies) >= self._maximum_assemblies
                or allocation > self._maximum_bytes - self._allocated
            ):
                raise wire.ProtocolError("PDO assembly limits exceeded")
            assembly = [bytearray(chunk.total_size), bytearray(chunk.total_size), 0, now]
            self._assemblies[key] = assembly
            self._allocated += allocation
        elif len(assembly[0]) != chunk.total_size:
            raise wire.ProtocolError("PDO chunks disagree on total size")
        data = assembly[0]
        present = assembly[1]
        assert isinstance(data, bytearray) and isinstance(present, bytearray)
        present_count = int(assembly[2])
        for index, value in enumerate(chunk.data):
            destination = chunk.offset + index
            if present[destination] and data[destination] != value:
                raise wire.ProtocolError("overlapping PDO chunks contain different bytes")
            if not present[destination]:
                present[destination] = 1
                present_count += 1
            data[destination] = value
        assembly[2] = present_count
        if present_count != len(data):
            return None
        del self._assemblies[key]
        self._allocated -= allocation
        self._delivered[target] = chunk.cycle_sequence
        for candidate in list(self._assemblies):
            if candidate[:3] == target and candidate[3] <= chunk.cycle_sequence:
                old = self._assemblies.pop(candidate)
                self._allocated -= len(old[0]) * 2
        return PdoInput(
            chunk.bus_index,
            chunk.subdevice_index,
            chunk.cycle_sequence,
            chunk.epoch,
            bytes(data),
            now,
        )

    def expire(self, maximum_age: float, now: float) -> None:
        for key, assembly in list(self._assemblies.items()):
            if now - float(assembly[3]) > maximum_age:
                self._allocated -= len(assembly[0]) * 2
                del self._assemblies[key]


class SystemCoreClient:
    """Bounded, nonblocking robot-code client with fail-closed reconnects."""

    def __init__(self, config: SystemCoreConfig = SystemCoreConfig()) -> None:
        self._config = config
        self._commands: queue.Queue[_Command] = queue.Queue(config.maximum_queued_commands)
        self._future_slots = threading.BoundedSemaphore(config.maximum_queued_commands * 2)
        self._completions = _Dispatcher(
            "ec-systemcore-python-completion",
            2,
            config.maximum_queued_commands * 2,
        )
        self._listeners = _Dispatcher("ec-systemcore-python-listener", 1, 512)
        self._state_lock = threading.Lock()
        self._session = _Session(0)
        self._health = Health(HealthState.STOPPED, "client has not started", 0, time.monotonic())
        self._latest_status: Status | None = None
        self._latest_buses: dict[int, BusInfo] = {}
        self._latest_inputs: dict[tuple[int, int], PdoInput] = {}
        self._latest_disabled: OutputsDisabled | None = None
        self._last_seen_epoch = 0
        self._generation = 0
        self._running = threading.Event()
        self._reconnect_wake = threading.Event()
        self._closed = False
        self._worker: threading.Thread | None = None
        self._active_socket: socket.socket | None = None
        self._lifecycle_lock = threading.Lock()

    @classmethod
    def connect(cls, config: SystemCoreConfig = SystemCoreConfig()) -> "SystemCoreClient":
        return cls(config).start()

    def start(self) -> "SystemCoreClient":
        with self._lifecycle_lock:
            if self._closed:
                raise RuntimeError("client is closed")
            if self._running.is_set():
                return self
            self._running.set()
            self._worker = threading.Thread(
                target=self._run,
                name="ec-systemcore-python-io",
                daemon=True,
            )
            self._worker.start()
        return self

    @property
    def health(self) -> Health:
        with self._state_lock:
            return self._health

    @property
    def generation(self) -> int:
        with self._state_lock:
            return self._session.generation

    @property
    def outputs_enabled(self) -> bool:
        with self._state_lock:
            current = self._session
            return (
                current.ready
                and current.role is ClientRole.CONTROLLER
                and current.outputs_enabled
            )

    @property
    def granted_role(self) -> ClientRole | None:
        with self._state_lock:
            return self._session.role if self._session.ready else None

    @property
    def latest_status(self) -> Status | None:
        with self._state_lock:
            return self._latest_status

    @property
    def latest_buses(self) -> Mapping[int, BusInfo]:
        with self._state_lock:
            return MappingProxyType(dict(self._latest_buses))

    @property
    def latest_inputs(self) -> Mapping[tuple[int, int], PdoInput]:
        with self._state_lock:
            return MappingProxyType(dict(self._latest_inputs))

    @property
    def latest_outputs_disabled(self) -> OutputsDisabled | None:
        with self._state_lock:
            return self._latest_disabled

    def latest_input(self, bus_index: int, subdevice_index: int) -> PdoInput | None:
        with self._state_lock:
            return self._latest_inputs.get((bus_index, subdevice_index))

    def enable_outputs(self) -> Future[Ack]:
        return self._set_outputs(True)

    def disable_outputs(self) -> Future[Ack]:
        return self._set_outputs(False)

    def _set_outputs(self, enabled: bool) -> Future[Ack]:
        with self._state_lock:
            current = self._session
            if not current.ready or current.role is not ClientRole.CONTROLLER:
                return _failed_future(RuntimeError("controller session is not ready"))
            if current.disable_barrier:
                return _failed_future(RuntimeError("fresh connection required after disable"))
            if not enabled:
                current = _Session(
                    current.generation,
                    current.ready,
                    current.role,
                    current.epoch,
                    current.outputs_enabled,
                    True,
                )
                self._session = current
        return self._submit(
            current,
            wire.OUTPUT_ENABLE,
            wire.output_enable(current.epoch, enabled),
            "enable" if enabled else "disable",
        )

    def write_output(
        self,
        bus_index: int,
        subdevice_index: int,
        offset: int,
        data: bytes,
    ) -> Future[Ack]:
        with self._state_lock:
            current = self._session
            if (
                not current.ready
                or current.role is not ClientRole.CONTROLLER
                or not current.outputs_enabled
                or current.disable_barrier
            ):
                return _failed_future(RuntimeError("outputs are not enabled"))
        return self._submit(
            current,
            wire.OUTPUT_WRITE,
            wire.output_write(current.epoch, bus_index, subdevice_index, offset, data),
            "write",
        )

    def clear_counters(self) -> Future[Ack]:
        return self._ready_command(wire.CLEAR_COUNTERS, b"")

    def subscribe_inputs(self, enabled: bool, period_ms: int) -> Future[Ack]:
        return self._ready_command(
            wire.SUBSCRIBE_INPUTS,
            wire.subscription(enabled, period_ms),
        )

    def rescan_adapters(self) -> Future[Ack]:
        return self._ready_command(wire.ADAPTER_RESCAN, b"")

    def unlock_adapter(self, bus_index: int) -> Future[Ack]:
        with self._state_lock:
            current = self._session
            if (
                not current.ready
                or current.role is not ClientRole.CONTROLLER
                or current.outputs_enabled
                or current.disable_barrier
            ):
                return _failed_future(
                    RuntimeError("adapter unlock requires a disabled controller session")
                )
        return self._submit(
            current,
            wire.ADAPTER_UNLOCK,
            wire.adapter_unlock(current.epoch, bus_index),
            "generic",
        )

    def release_control(self) -> Future[Ack]:
        with self._state_lock:
            current = self._session
            if (
                not current.ready
                or current.role is not ClientRole.CONTROLLER
                or current.disable_barrier
            ):
                return _failed_future(RuntimeError("controller session is not ready"))
            current = _Session(
                current.generation,
                current.ready,
                current.role,
                current.epoch,
                current.outputs_enabled,
                True,
            )
            self._session = current
        return self._submit(
            current,
            wire.RELEASE_CONTROL,
            wire.epoch(current.epoch),
            "release",
        )

    def request_reconnect(self) -> None:
        with self._state_lock:
            active = self._active_socket
        if active is not None:
            try:
                active.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                active.close()
            except OSError:
                pass

    def close(self) -> None:
        with self._lifecycle_lock:
            if self._closed:
                return
            self._closed = True
            self._running.clear()
            self._reconnect_wake.set()
            worker = self._worker
        self.request_reconnect()
        if worker is not None and worker is not threading.current_thread():
            worker.join(2.0)
        self._fail_queued(RuntimeError("client is closed"))
        self._update_health(HealthState.STOPPED, "client stopped", None, self.generation)
        self._listeners.close()
        self._completions.close()

    def __enter__(self) -> "SystemCoreClient":
        return self.start()

    def __exit__(self, *_: object) -> None:
        self.close()

    def _ready_command(self, message_type: int, payload: bytes) -> Future[Ack]:
        with self._state_lock:
            current = self._session
            if not current.ready:
                return _failed_future(RuntimeError("session is not ready"))
        return self._submit(current, message_type, payload, "generic")

    def _submit(
        self,
        current: _Session,
        message_type: int,
        payload: bytes,
        kind: str,
    ) -> Future[Ack]:
        safety = kind in {"disable", "release"}
        if not self._future_slots.acquire(blocking=False):
            if safety:
                self.request_reconnect()
            return _failed_future(RuntimeError("too many outstanding command futures"))
        result = _CommandFuture(self._future_slots.release, self.request_reconnect if safety else None)
        if not self._running.is_set():
            result.set_exception(RuntimeError("client is not running"))
            return result
        try:
            self._commands.put_nowait(
                _Command(
                    current.generation,
                    current.epoch,
                    message_type,
                    bytes(payload),
                    kind,
                    result,
                )
            )
        except queue.Full:
            result.set_exception(RuntimeError("client command queue is full"))
            if safety:
                self.request_reconnect()
        return result

    def _run(self) -> None:
        delay = self._config.minimum_reconnect_delay
        try:
            while self._running.is_set():
                self._generation += 1
                generation = self._generation
                established = False
                try:
                    self._run_session(generation)
                except Exception as error:
                    if self._running.is_set():
                        self._update_health(
                            HealthState.DEGRADED,
                            str(error) or "connection lost",
                            error,
                            generation,
                        )
                finally:
                    with self._state_lock:
                        established = (
                            self._session.generation == generation
                            and self._session.ready
                        )
                        self._active_socket = None
                    self._invalidate(generation, OSError("transport generation ended"))
                if not self._running.is_set():
                    break
                if established:
                    delay = self._config.minimum_reconnect_delay
                self._reconnect_wake.wait(delay)
                self._reconnect_wake.clear()
                if not self._running.is_set():
                    break
                delay = min(self._config.maximum_reconnect_delay, delay * 2)
        finally:
            self._running.clear()

    def _run_session(self, generation: int) -> None:
        self._update_health(HealthState.CONNECTING, "connecting to daemon", None, generation)
        transport = _Transport(self._config)
        active = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        active.setblocking(False)
        with self._state_lock:
            self._active_socket = active
        try:
            self._connect(active)
            with self._state_lock:
                self._latest_status = None
                self._latest_buses.clear()
                self._latest_inputs.clear()
                self._session = _Session(generation)
            self._update_health(HealthState.HANDSHAKING, "waiting for HELLO_ACK", None, generation)
            transport.enqueue(
                wire.frame(
                    wire.HELLO,
                    0,
                    wire.hello(
                        int(self._config.requested_role),
                        self._config.subscribe_inputs,
                        self._config.input_period_ms,
                        self._last_seen_epoch,
                    ),
                )
            )
            transport.hello_deadline = time.monotonic() + self._config.request_timeout
            while self._running.is_set():
                self._process_commands(transport, generation)
                self._queue_heartbeat(transport, generation)
                readable, writable, _ = select.select(
                    [active],
                    [active] if transport.outbound else [],
                    [],
                    0.002,
                )
                if writable:
                    transport.flush(active)
                if readable:
                    self._read_available(active, transport, generation)
                now = time.monotonic()
                self._expire_requests(transport, generation, now)
                transport.assemblies.expire(2.0, now)
                if not transport.handshake_complete and now >= transport.hello_deadline:
                    raise TimeoutError("HELLO_ACK timed out")
                if transport.force_reconnect:
                    raise OSError("daemon safety state requires a fresh connection")
        finally:
            transport.fail_pending(OSError("transport generation ended"), self)
            try:
                active.close()
            except OSError:
                pass

    def _connect(self, active: socket.socket) -> None:
        result = active.connect_ex(os.fspath(self._config.socket_path))
        pending_errors = {
            0,
            errno.EINPROGRESS,
            errno.EAGAIN,
            errno.EWOULDBLOCK,
            getattr(errno, "EALREADY", errno.EINPROGRESS),
        }
        if result not in pending_errors:
            raise OSError(result, os.strerror(result))
        if result == 0:
            return
        deadline = time.monotonic() + self._config.request_timeout
        while self._running.is_set():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("daemon connection timed out")
            _, writable, exceptional = select.select([], [active], [active], min(0.1, remaining))
            if active in exceptional or active in writable:
                error = active.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if error:
                    raise OSError(error, os.strerror(error))
                return
        raise OSError("daemon connection was cancelled")

    def _process_commands(self, transport: "_Transport", generation: int) -> None:
        for _ in range(self._config.maximum_queued_commands):
            if len(transport.pending) >= self._config.maximum_queued_commands:
                return
            try:
                command = self._commands.get_nowait()
            except queue.Empty:
                return
            with self._state_lock:
                current = self._session
            if (
                not transport.handshake_complete
                or not current.ready
                or command.generation != generation
                or command.generation != current.generation
                or command.epoch != current.epoch
            ):
                self._complete_exception(
                    command.result,
                    RuntimeError("command invalidated by connection or epoch"),
                )
                if command.kind in {"disable", "release"}:
                    transport.force_reconnect = True
                continue
            if command.result.cancelled():
                command.result.retire()
                if command.kind in {"disable", "release"}:
                    transport.force_reconnect = True
                continue
            request_id = transport.next_request_id()
            encoded = wire.frame(command.message_type, request_id, command.payload)
            if not transport.can_enqueue(len(encoded)):
                self._complete_exception(command.result, RuntimeError("outbound byte limit reached"))
                if command.kind in {"disable", "release"}:
                    transport.force_reconnect = True
                continue
            transport.pending[request_id] = _Pending(
                command.kind,
                command.result,
                time.monotonic() + self._config.request_timeout,
                generation,
                command.epoch,
            )
            transport.enqueue(encoded)

    def _queue_heartbeat(self, transport: "_Transport", generation: int) -> None:
        now = time.monotonic()
        if not transport.handshake_complete or now < transport.next_heartbeat:
            return
        with self._state_lock:
            current = self._session
        if (
            not current.ready
            or current.role is not ClientRole.CONTROLLER
            or current.generation != generation
            or transport.heartbeat_request_id
        ):
            return
        request_id = transport.next_request_id()
        encoded = wire.frame(wire.HEARTBEAT, request_id, wire.epoch(current.epoch))
        if not transport.can_enqueue(len(encoded)):
            raise wire.ProtocolError("unable to queue safety heartbeat")
        transport.pending[request_id] = _Pending(
            "heartbeat",
            None,
            now + min(self._config.request_timeout, transport.heartbeat_ack_timeout),
            generation,
            current.epoch,
        )
        transport.heartbeat_request_id = request_id
        transport.next_heartbeat = now + transport.heartbeat_interval
        transport.enqueue(encoded)

    def _read_available(
        self,
        active: socket.socket,
        transport: "_Transport",
        generation: int,
    ) -> None:
        while True:
            try:
                data = active.recv(8192)
            except BlockingIOError:
                return
            if not data:
                raise EOFError("daemon closed the socket")
            for frame_value in transport.decoder.feed(data):
                self._process_frame(frame_value, transport, generation)

    def _process_frame(
        self,
        frame_value: wire.Frame,
        transport: "_Transport",
        generation: int,
    ) -> None:
        if not transport.handshake_complete:
            if frame_value.message_type != wire.HELLO_ACK or frame_value.request_id != 0:
                raise wire.ProtocolError("expected HELLO_ACK first")
            hello = wire.decode_hello_ack(frame_value.payload)
            role = ClientRole(hello.role)
            if role is ClientRole.CONTROLLER and hello.outputs_enabled:
                raise wire.ProtocolError("new controller generation has outputs enabled")
            transport.handshake_complete = True
            transport.heartbeat_interval = max(
                0.010, min(0.100, hello.heartbeat_timeout_ms / 3000.0)
            )
            transport.heartbeat_ack_timeout = max(
                0.010, hello.heartbeat_timeout_ms / 2000.0
            )
            transport.next_heartbeat = time.monotonic() + transport.heartbeat_interval
            self._last_seen_epoch = hello.epoch
            with self._state_lock:
                self._session = _Session(
                    generation,
                    True,
                    role,
                    hello.epoch,
                    False,
                    False,
                )
            self._update_health(
                HealthState.CONTROLLING_DISABLED
                if role is ClientRole.CONTROLLER
                else HealthState.OBSERVING,
                "HELLO handshake complete",
                None,
                generation,
            )
            return

        if frame_value.message_type == wire.HELLO_ACK or (
            frame_value.request_id != 0
            and frame_value.message_type not in {wire.ACK, wire.ERROR}
        ):
            raise wire.ProtocolError("daemon event/request ID pairing is invalid")
        if frame_value.message_type in {wire.ACK, wire.ERROR}:
            self._process_ack(frame_value, transport, generation)
        elif frame_value.message_type == wire.STATUS:
            self._process_status(frame_value, transport, generation)
        elif frame_value.message_type == wire.BUS_INFO:
            self._process_bus_info(frame_value, transport, generation)
        elif frame_value.message_type == wire.PDO_INPUT:
            self._process_pdo(frame_value, transport, generation)
        elif frame_value.message_type == wire.OUTPUTS_DISABLED:
            reason, bus, value = wire.decode_outputs_disabled(frame_value.payload)
            disabled = OutputsDisabled(reason, bus, value, time.monotonic())
            with self._state_lock:
                current = self._session
                self._session = _Session(
                    current.generation,
                    current.ready,
                    current.role,
                    current.epoch,
                    False,
                    current.disable_barrier,
                )
                self._latest_disabled = disabled
            self._last_seen_epoch = value
            self._notify(lambda listener: listener.on_outputs_disabled(disabled), critical=True)
            transport.force_reconnect = True
        else:
            raise wire.ProtocolError("unexpected daemon message")

    def _process_ack(
        self,
        frame_value: wire.Frame,
        transport: "_Transport",
        generation: int,
    ) -> None:
        if frame_value.request_id == 0:
            raise wire.ProtocolError("ACK request ID is zero")
        pending = transport.pending.pop(frame_value.request_id, None)
        if pending is None:
            raise wire.ProtocolError("unsolicited or expired ACK")
        if transport.heartbeat_request_id == frame_value.request_id:
            transport.heartbeat_request_id = 0
        status_value, detail = wire.decode_ack(frame_value.payload)
        status = AckStatus(status_value)
        with self._state_lock:
            current = self._session
        if (
            pending.generation != generation
            or pending.epoch != current.epoch
        ):
            self._complete_exception(
                pending.result,
                RuntimeError("ACK belongs to an invalidated generation"),
            )
            transport.force_reconnect = True
            return
        if pending.kind == "heartbeat":
            if status is not AckStatus.OK:
                raise wire.ProtocolError(f"heartbeat rejected with {status.name}")
            return
        if pending.kind == "enable" and status is AckStatus.OK:
            self._set_local_outputs(generation, pending.epoch, True)
        if pending.kind in {"disable", "release"}:
            if status is AckStatus.OK:
                self._set_local_outputs(generation, pending.epoch, False)
            transport.force_reconnect = True
        if status in {
            AckStatus.STALE_EPOCH,
            AckStatus.UNAUTHORIZED,
            AckStatus.DISABLED,
        }:
            transport.force_reconnect = True
        self._complete_result(pending.result, Ack(status, detail))

    def _process_status(
        self,
        frame_value: wire.Frame,
        transport: "_Transport",
        generation: int,
    ) -> None:
        values = wire.decode_status(frame_value.payload)
        state, flags, adapters, subdevices, faults, maximum_jitter, current_jitter = values[:7]
        lost, overruns, value, controller_pid, buses, scheduling, errors, monotonic = values[7:]
        status = Status(
            state,
            bool(flags & 1),
            bool(flags & 2),
            bool(flags & 4),
            bool(flags & 8),
            bool(flags & 16),
            bool(flags & 32),
            bool(flags & 64),
            bool(flags & 128),
            adapters,
            subdevices,
            faults,
            maximum_jitter,
            current_jitter,
            lost,
            overruns,
            value,
            controller_pid,
            buses,
            bool(scheduling),
            errors,
            monotonic,
            time.monotonic(),
        )
        with self._state_lock:
            if self._session.generation != generation or value != self._session.epoch:
                transport.force_reconnect = True
                return
            self._latest_status = status
        self._notify(lambda listener: listener.on_status(status))

    def _process_bus_info(
        self,
        frame_value: wire.Frame,
        transport: "_Transport",
        generation: int,
    ) -> None:
        decoded = wire.decode_bus_info(frame_value.payload)
        bus, state, link, lock_state, lock_reason, subdevices, value, *strings = decoded
        bus_info = BusInfo(
            int(bus),
            int(state),
            bool(link),
            int(lock_state),
            int(lock_reason),
            int(subdevices),
            int(value),
            str(strings[0]),
            str(strings[1]),
            AdapterIdentity(*(str(item) for item in strings[2:])),
        )
        with self._state_lock:
            if self._session.generation != generation or bus_info.epoch != self._session.epoch:
                transport.force_reconnect = True
                return
            if (
                bus_info.bus_index not in self._latest_buses
                and len(self._latest_buses) >= self._config.maximum_tracked_buses
            ):
                raise wire.ProtocolError("retained BUS_INFO limit exceeded")
            self._latest_buses[bus_info.bus_index] = bus_info
        self._notify(lambda listener: listener.on_bus_info(bus_info))

    def _process_pdo(
        self,
        frame_value: wire.Frame,
        transport: "_Transport",
        generation: int,
    ) -> None:
        chunk = wire.decode_pdo(frame_value.payload, self._config.maximum_pdo_image_bytes)
        with self._state_lock:
            if self._session.generation != generation or chunk.epoch != self._session.epoch:
                transport.force_reconnect = True
                return
        completed = transport.assemblies.accept(chunk, time.monotonic())
        if completed is None:
            return
        key = (completed.bus_index, completed.subdevice_index)
        with self._state_lock:
            if (
                key not in self._latest_inputs
                and len(self._latest_inputs) >= self._config.maximum_tracked_pdo_inputs
            ):
                raise wire.ProtocolError("retained PDO input limit exceeded")
            self._latest_inputs[key] = completed
        self._notify(lambda listener: listener.on_pdo_input(completed))

    def _expire_requests(
        self,
        transport: "_Transport",
        generation: int,
        now: float,
    ) -> None:
        expired = [
            request_id
            for request_id, pending in transport.pending.items()
            if now >= pending.deadline
        ]
        if not expired:
            return
        for request_id in expired:
            pending = transport.pending.pop(request_id)
            self._complete_exception(
                pending.result,
                TimeoutError(f"daemon request timed out in generation {generation}"),
            )
        raise TimeoutError("daemon request timed out")

    def _set_local_outputs(self, generation: int, value: int, enabled: bool) -> None:
        with self._state_lock:
            current = self._session
            if current.generation != generation or current.epoch != value:
                return
            self._session = _Session(
                current.generation,
                current.ready,
                current.role,
                current.epoch,
                enabled,
                current.disable_barrier,
            )
        self._update_health(
            HealthState.CONTROLLING_ENABLED
            if enabled
            else HealthState.CONTROLLING_DISABLED,
            "outputs enabled" if enabled else "outputs disabled",
            None,
            generation,
        )

    def _invalidate(self, generation: int, failure: BaseException) -> None:
        with self._state_lock:
            if self._session.generation <= generation:
                self._session = _Session(generation)
            self._latest_status = None
            self._latest_buses.clear()
            self._latest_inputs.clear()
        self._fail_queued(failure)

    def _fail_queued(self, failure: BaseException) -> None:
        while True:
            try:
                command = self._commands.get_nowait()
            except queue.Empty:
                return
            self._complete_exception(command.result, failure)

    def _update_health(
        self,
        state: HealthState,
        message: str,
        cause: BaseException | None,
        generation: int,
    ) -> None:
        updated = Health(state, message, generation, time.monotonic(), cause)
        with self._state_lock:
            previous = self._health
            self._health = updated
        if (
            previous.state != updated.state
            or previous.message != updated.message
            or previous.generation != updated.generation
        ):
            self._notify(lambda listener: listener.on_health_changed(updated), critical=True)

    def _notify(
        self,
        invocation: Callable[[SystemCoreListener], None],
        *,
        critical: bool = False,
    ) -> None:
        def safe() -> None:
            try:
                invocation(self._config.listener)
            except BaseException:
                pass

        self._listeners.submit(safe, critical=critical)

    def _complete_result(self, result: _CommandFuture | None, value: Ack) -> None:
        if result is None:
            return
        if result.done():
            result.retire()
            return
        if not self._completions.submit(lambda: result.set_result(value)):
            result.retire()
            self.request_reconnect()

    def _complete_exception(
        self,
        result: _CommandFuture | None,
        failure: BaseException,
    ) -> None:
        if result is None:
            return
        if result.done():
            result.retire()
            return
        if not self._completions.submit(lambda: result.set_exception(failure)):
            result.retire()
            self.request_reconnect()


class _Transport:
    def __init__(self, config: SystemCoreConfig) -> None:
        self.decoder = wire.FrameDecoder()
        self.outbound: deque[memoryview] = deque()
        self.outbound_bytes = 0
        self.maximum_outbound_bytes = config.maximum_outbound_bytes
        self.pending: dict[int, _Pending] = {}
        self.assemblies = _PdoAssembler(
            config.maximum_pdo_assemblies,
            config.maximum_pdo_assembly_bytes,
        )
        self.request_id = 1
        self.hello_deadline = 0.0
        self.next_heartbeat = 0.0
        self.heartbeat_request_id = 0
        self.heartbeat_interval = 0.100
        self.heartbeat_ack_timeout = 0.100
        self.handshake_complete = False
        self.force_reconnect = False

    def next_request_id(self) -> int:
        selected = self.request_id
        self.request_id = 1 if selected == 0xFFFF_FFFF else selected + 1
        return selected

    def can_enqueue(self, size: int) -> bool:
        return size <= self.maximum_outbound_bytes - self.outbound_bytes

    def enqueue(self, encoded: bytes) -> None:
        if not self.can_enqueue(len(encoded)):
            raise RuntimeError("outbound byte limit exceeded")
        self.outbound.append(memoryview(encoded))
        self.outbound_bytes += len(encoded)

    def flush(self, active: socket.socket) -> None:
        while self.outbound:
            current = self.outbound[0]
            try:
                sent = active.send(current)
            except BlockingIOError:
                return
            if sent == 0:
                raise EOFError("daemon socket closed during write")
            self.outbound_bytes -= sent
            if sent == len(current):
                self.outbound.popleft()
            else:
                self.outbound[0] = current[sent:]

    def fail_pending(self, failure: BaseException, owner: SystemCoreClient) -> None:
        for pending in self.pending.values():
            owner._complete_exception(pending.result, failure)
        self.pending.clear()
        self.outbound.clear()
        self.outbound_bytes = 0


def _failed_future(failure: BaseException) -> Future[Ack]:
    result: Future[Ack] = Future()
    result.set_exception(failure)
    return result


def _bounded(value: float, minimum: float, maximum: float, field_name: str) -> None:
    if not minimum <= value <= maximum:
        raise ValueError(f"{field_name} must be between {minimum} and {maximum}")
