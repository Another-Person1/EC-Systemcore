"""Public ec-systemcore ECSC v1 Python SDK."""

from .client import (
    Ack,
    AckStatus,
    AdapterIdentity,
    BusInfo,
    ClientRole,
    Health,
    HealthState,
    OutputsDisabled,
    PdoInput,
    Status,
    SystemCoreClient,
    SystemCoreConfig,
    SystemCoreListener,
)
from ._protocol import ProtocolError

__version__ = "2026.0.1"

__all__ = [
    "Ack",
    "AckStatus",
    "AdapterIdentity",
    "BusInfo",
    "ClientRole",
    "Health",
    "HealthState",
    "OutputsDisabled",
    "PdoInput",
    "ProtocolError",
    "Status",
    "SystemCoreClient",
    "SystemCoreConfig",
    "SystemCoreListener",
    "__version__",
]
