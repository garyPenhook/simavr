"""Python bindings for simavr's host API (simavr/sim/sim_host.h).

Importing the package is cheap: the cffi layer and libsimavr are only loaded
when a name below is first used.
"""

lazy from .core import (
    Cpu,
    Event,
    InvalidArgument,
    InterruptEvent,
    IrqEvent,
    LoadError,
    NotFound,
    NotSupported,
    OutOfRange,
    OverflowEvent,
    PinEvent,
    PinState,
    RawEvent,
    Simulator,
    SimavrError,
    Space,
    State,
    StateError,
    StateEvent,
    StopReason,
    UartTxEvent,
    api_version,
    cores,
    ioctl_def,
)

__all__ = [
    "Cpu", "Event", "InterruptEvent", "InvalidArgument", "IrqEvent", "LoadError", "NotFound",
    "NotSupported", "OutOfRange", "OverflowEvent", "PinEvent", "PinState",
    "RawEvent", "Simulator", "SimavrError", "Space", "State", "StateError",
    "StateEvent", "StopReason", "UartTxEvent", "api_version", "cores",
    "ioctl_def",
]
