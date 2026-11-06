"""Shared definitions for the solver-study scripts (standard library only).

The workload matrix, fixture specifications, file locations, CSV helpers, environment
capture and the run-status rules all live here, so every script applies them the same way.
See benchmarks/README.md for the protocol these implement.
"""

from __future__ import annotations

import csv
import ctypes
import datetime as _dt
import hashlib
import json
import os
import platform
import subprocess
import sys
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]          # project root
BENCH_DIR = ROOT / "benchmarks"
FIXTURES_DIR = BENCH_DIR / "fixtures"
RESULTS_DIR = BENCH_DIR / "results"
DEFAULT_BENCH = ROOT / "build-cmake" / ("sudoku_bench.exe" if os.name == "nt" else "sudoku_bench")

BOX_SIZES = range(1, 16)
SPARSITY_TARGETS = (10, 20, 30, 40, 50, 60, 70)
SOLVERS = ("naive", "mrv", "dlx")
OPS = ("first", "unique")
ENUMERATE_MAX_N = 2          # population A only

# Statuses (README section 9). Only MEASURED_STATUSES are observations.
MEASURED_STATUSES = {"completed", "timeout", "watchdog_killed", "memory_rejected", "unsupported", "error"}
NOT_OBSERVED_STATUSES = {"pruned_by_prefix", "not_run"}
TIMED_OUT = {"timeout", "watchdog_killed"}


# ---------------------------------------------------------------------------
# Workload matrix
# ---------------------------------------------------------------------------

def empty_count(n: int, pct: int) -> int:
    """round_half_up(pct * N^2 / 100), computed exactly."""
    cells = n ** 4
    return int((Decimal(pct) * cells / 100).quantize(Decimal(1), rounding=ROUND_HALF_UP))


def matrix_cells() -> list[dict]:
    """The 100 (n, empty) cells: duplicates within one n are merged; the first (lowest)
    target that produced an empty count labels it."""
    cells = []
    for n in BOX_SIZES:
        seen = set()
        for pct in SPARSITY_TARGETS:
            empty = empty_count(n, pct)
            if empty in seen:
                continue
            seen.add(empty)
            cells.append({"n": n, "N": n * n, "cells": n ** 4, "empty": empty, "target_pct": pct,
                          "actual_pct": 100.0 * empty / n ** 4})
    return cells


def fixture_id(population: str, n: int, empty: int, seed_index: int) -> str:
    return f"{population}-n{n:02d}-e{empty:06d}-s{seed_index:03d}"


# ---------------------------------------------------------------------------
# CSV helpers
# ---------------------------------------------------------------------------

def read_csv(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def write_csv(path: Path, rows: list[dict], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    tmp.replace(path)


def load_manifest(fixtures_dir: Path = FIXTURES_DIR) -> dict[str, dict]:
    return {row["id"]: row for row in read_csv(fixtures_dir / "manifest.csv")}


# ---------------------------------------------------------------------------
# Environment capture
# ---------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def source_hash() -> str:
    """Hash of the code that produced the results (the project is not a git repository)."""
    h = hashlib.sha256()
    files = [ROOT / "CMakeLists.txt", BENCH_DIR / "CMakeLists.txt"]
    for sub in ("include", "src", "app", "benchmarks/src"):
        files += sorted(p for p in (ROOT / sub).rglob("*") if p.is_file())
    for p in files:
        h.update(str(p.relative_to(ROOT)).replace("\\", "/").encode())
        h.update(p.read_bytes())
    return h.hexdigest()


def total_ram_bytes() -> int:
    if os.name != "nt":
        return 0

    class MemoryStatus(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

    status = MemoryStatus()
    status.dwLength = ctypes.sizeof(MemoryStatus)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return int(status.ullTotalPhys)


def cpu_name() -> str:
    if os.name == "nt":
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-Command",
                                  "(Get-CimInstance Win32_Processor).Name"],
                                 capture_output=True, text=True, timeout=60)
            if out.stdout.strip():
                return out.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return platform.processor()


def environment(bench: Path, extra: dict) -> dict:
    env_lines = subprocess.run([str(bench), "env"], capture_output=True, text=True, check=True).stdout
    build = dict(line.split("=", 1) for line in env_lines.splitlines() if "=" in line)
    return {
        "captured_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "cpu": cpu_name(),
        "logical_cpus": os.cpu_count(),
        "ram_bytes": total_ram_bytes(),
        "os": platform.platform(),
        "python": sys.version.split()[0],
        "build": build,
        "bench_binary": str(bench),
        "bench_binary_sha256": sha256_file(bench),
        "source_sha256": source_hash(),
        **extra,
    }


def append_json_line(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(record) + "\n")


def keep_awake(enable: bool) -> None:
    """Asks Windows not to sleep while this process runs (no settings are changed)."""
    if os.name != "nt":
        return
    es_continuous, es_system_required = 0x80000000, 0x00000001
    flags = es_continuous | (es_system_required if enable else 0)
    ctypes.windll.kernel32.SetThreadExecutionState(flags)


# ---------------------------------------------------------------------------
# Summary rules used by the schedule report (the full analysis lives in 10C)
# ---------------------------------------------------------------------------

def completion_summary(rows: list[dict]) -> dict:
    """Completion counts for one group of runs. Pruned and not-run entries are counted
    separately and never enter the denominator (README section 10)."""
    measured = [r for r in rows if r["status"] in MEASURED_STATUSES]
    completed = [r for r in measured if r["status"] == "completed"]
    return {
        "measured": len(measured),
        "completed": len(completed),
        "timed_out": sum(r["status"] in TIMED_OUT for r in measured),
        "memory_rejected": sum(r["status"] == "memory_rejected" for r in measured),
        "errors": sum(r["status"] in ("error", "unsupported") for r in measured),
        "pruned": sum(r["status"] == "pruned_by_prefix" for r in rows),
        "not_run": sum(r["status"] == "not_run" for r in rows),
        "completion_rate": (len(completed) / len(measured)) if measured else None,
    }
