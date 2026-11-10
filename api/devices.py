"""Which accelerator torch may use, in one place.

Every call into a torch backend that exists only on one platform lives here: the manager, the search
scoring and the settings router ask this module instead of asking torch.cuda or torch.mps directly.
Devices are named the way torch names them, "cuda" / "mps" / "cpu", so a device string travels
unchanged into model.to(), tensor.to() and the autocast arguments.

The CUDA special cases are kept: a device capability that decides FP16, the free-VRAM reading of the
VRAM Guard, the out-of-memory error type. Each has a defined answer on a device that lacks it, and
no function here raises for a missing backend - a machine without torch's CUDA build (a CPU wheel,
a Windows machine without the driver) must start normally.
"""
import platform
from contextlib import contextmanager

import torch

CUDA = "cuda"
MPS = "mps"
CPU = "cpu"


def cuda_available() -> bool:
    """True when torch was built with CUDA and a driver answers."""
    try:
        return torch.cuda.is_available()
    except Exception:
        return False


def mps_available() -> bool:
    """True on an Apple-silicon Mac with an MPS-enabled torch build."""
    try:
        backend = getattr(torch.backends, "mps", None)
        return bool(backend is not None and backend.is_built() and backend.is_available())
    except Exception:
        return False


def available() -> list:
    """Every accelerator this machine exposes, fastest first; the CPU is always usable and not listed."""
    found = []
    if cuda_available():
        found.append(CUDA)
    if mps_available():
        found.append(MPS)
    return found


def preferred() -> str:
    """The device to use when the user asked for GPU acceleration but named no kind: CUDA, then MPS, else CPU."""
    for candidate in (CUDA, MPS):
        if candidate in available():
            return candidate
    return CPU


def label(device: str) -> str:
    """Human-readable name of a device, for logs and the settings screen."""
    return {
        CUDA: "CUDA (NVIDIA)",
        MPS: "MPS (Apple Metal)",
        CPU: "CPU",
    }.get(device, device)


def name(device: str) -> str:
    """The adapter's own name, or "N/A" when the backend does not report one."""
    if device == CUDA:
        try:
            return torch.cuda.get_device_name(0)
        except Exception:
            return "N/A"
    if device == MPS:
        return f"Apple Silicon GPU ({platform.machine()})"
    if device == CPU:
        # The settings screen labels this field "GPU"; on the CPU there is no GPU name to show ("N/A" is
        # what the Windows-only builds reported when CUDA was missing, so old frontends keep working).
        return "N/A"
    return "N/A"


def total_memory_mb(device: str):
    """The memory budget in MiB that batch sizing may use, or None when the backend reports nothing (CPU).
    CUDA answers with the card's VRAM; MPS with torch's recommended cap, which is below the Mac's RAM."""
    try:
        if device == CUDA:
            return torch.cuda.get_device_properties(0).total_memory / (1024 * 1024)
        if device == MPS:
            return torch.mps.recommended_max_memory() / (1024 * 1024)
    except Exception:
        return None
    return None


def mem_info(device: str):
    """(free, total) bytes of device memory, or None when the backend cannot report free memory.
    Only CUDA can: the VRAM Guard that reads this therefore runs on CUDA alone."""
    if device == CUDA:
        try:
            return torch.cuda.mem_get_info()
        except Exception:
            return None
    return None


def memory_reserved(device: str) -> int:
    """Bytes held by the caching allocator (0 on backends without one)."""
    try:
        if device == CUDA:
            return torch.cuda.memory_reserved()
        if device == MPS:
            return torch.mps.driver_allocated_memory()
    except Exception:
        pass
    return 0


def memory_allocated(device: str) -> int:
    """Bytes held by live tensors (0 on backends without an allocator to ask)."""
    try:
        if device == CUDA:
            return torch.cuda.memory_allocated()
        if device == MPS:
            return torch.mps.current_allocated_memory()
    except Exception:
        pass
    return 0


def supports_fp16(device: str) -> bool:
    """Whether half precision is worth using on this device: native FP16 tensor cores on CUDA (7.5+, Turing
    and later), the Metal backend's own FP16 on MPS, never on the CPU (float16 there is slower than float32)."""
    if device == CUDA:
        try:
            major, minor = torch.cuda.get_device_capability()
            return major > 7 or (major == 7 and minor >= 5)
        except Exception:
            return False
    if device == MPS:
        return True
    return False


@contextmanager
def autocast(device: str):
    """Mixed-precision context for a forward pass; a no-op context on the CPU or an old torch build."""
    if device not in (CUDA, MPS):
        yield
        return
    try:
        context = torch.autocast(device, dtype=torch.float16)
    except TypeError:
        # This torch build does not accept the device type: run in the model's own dtype
        yield
        return
    with context:
        yield


def empty_cache(device: str = None):
    """Release cached device memory; safe on every device, including a machine without an accelerator."""
    for candidate in ([device] if device else available()):
        try:
            if candidate == CUDA:
                torch.cuda.empty_cache()
            elif candidate == MPS:
                torch.mps.empty_cache()
        except Exception:
            pass


def synchronize(device: str = None):
    """Wait for queued device work; needed before timing, harmless elsewhere."""
    try:
        if device == CUDA:
            torch.cuda.synchronize()
        elif device == MPS:
            torch.mps.synchronize()
    except Exception:
        pass


def is_out_of_memory(error: BaseException) -> bool:
    """Whether a failed forward pass ran out of device memory. CUDA raises a dedicated type; MPS and the CPU
    allocator raise a RuntimeError whose message says so, which is also what a wrapped CUDA OOM does."""
    types = []
    for candidate in (getattr(torch, "OutOfMemoryError", None), getattr(torch.cuda, "OutOfMemoryError", None)):
        if isinstance(candidate, type):
            types.append(candidate)
    if types and isinstance(error, tuple(types)):
        return True
    return "out of memory" in str(error).lower()
