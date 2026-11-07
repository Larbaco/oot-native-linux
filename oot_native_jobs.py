"""Build job count: the one default, its ceilings, and how a user's choice is validated.

Shared by the launcher (GUI and CLI) and the builder (nxs2.py), so both pick the same number.

The default rule is ``default_jobs() = max(1, floor(0.9 * available_threads))``.

* ``available_threads`` is ``len(os.sched_getaffinity(0))``, which respects ``taskset``/cpusets and containers,
  falling back to ``os.cpu_count()`` where the affinity call does not exist.
* If cgroup v2 sets a CPU quota (``/sys/fs/cgroup/cpu.max`` reads ``<quota> <period>``), the number of whole CPUs
  that quota allows (``floor(quota / period)``, at least 1) is used when it is smaller than the affinity count.
  Only the cgroup the process sees at that path is read (the container's own cgroup under a cgroup namespace).
* Memory ceiling: ``MEM_PER_JOB_BYTES`` is the RAM one job needs at its peak. It is ``None`` because the build has
  no reliable per-job figure (the only measurement is 4.5 GB peak RSS of the whole build with 2 jobs). While it is
  ``None`` nothing is capped. Once a measured value is put there, the default is limited so that
  ``jobs * MEM_PER_JOB_BYTES`` stays within 85 % of ``MemAvailable``, and the log says so.

A user's explicit value always wins over the ceilings; it only draws a warning. Precedence:
``--jobs`` > ``OOT_NATIVE_JOBS`` > the value saved by the launcher GUI > ``default_jobs()``.
"""
from __future__ import annotations

import math
import os
import pathlib
import re
from dataclasses import dataclass, field

from oot_native_i18n import tr

ENV_VAR = "OOT_NATIVE_JOBS"
THREAD_FRACTION = 0.9
MEMORY_FRACTION = 0.85
# RAM one build job needs at peak, in bytes. None = no reliable estimate (see the module docstring).
MEM_PER_JOB_BYTES: int | None = None

CGROUP_CPU_MAX = pathlib.Path("/sys/fs/cgroup/cpu.max")
PROC_MEMINFO = pathlib.Path("/proc/meminfo")


class JobsError(ValueError):
    """An invalid job count. ``value`` is the offending input, ``source`` where it came from."""

    def __init__(self, value: object, source: str = "--jobs"):
        super().__init__(f"{source}: {value!r}")
        self.value = value
        self.source = source


def affinity_threads() -> int:
    """Threads this process may run on: the affinity mask, else the CPU count, never less than 1."""
    try:
        count = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        count = os.cpu_count() or 1
    return max(1, count)


def cgroup_cpu_limit(path: pathlib.Path | None = None) -> int | None:
    """Whole CPUs a cgroup v2 quota allows, or None when there is no (readable, valid) quota."""
    try:
        fields = (path or CGROUP_CPU_MAX).read_text(encoding="ascii").split()
        if len(fields) < 2 or fields[0] == "max":
            return None
        quota, period = int(fields[0]), int(fields[1])
    except (OSError, ValueError):
        return None
    if quota <= 0 or period <= 0:
        return None
    return max(1, quota // period)


def available_threads(cgroup_file: pathlib.Path | None = None) -> int:
    """The smaller of the affinity thread count and the cgroup v2 CPU quota."""
    threads = affinity_threads()
    limit = cgroup_cpu_limit(cgroup_file)
    return min(threads, limit) if limit else threads


def mem_available_bytes(meminfo: pathlib.Path | None = None) -> int | None:
    """``MemAvailable`` from /proc/meminfo in bytes, or None when it cannot be read."""
    try:
        for line in (meminfo or PROC_MEMINFO).read_text(encoding="ascii", errors="replace").splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        pass
    return None


def memory_cap(mem_available: int | None, mem_per_job: int | None = None) -> int | None:
    """Most jobs that fit in 85 % of the available RAM (at least 1), or None when it cannot be told."""
    if mem_per_job is None:
        mem_per_job = MEM_PER_JOB_BYTES
    if not mem_per_job or mem_per_job <= 0 or mem_available is None:
        return None
    return max(1, int(MEMORY_FRACTION * mem_available // mem_per_job))


@dataclass(frozen=True)
class JobsPlan:
    threads: int                 # available threads (affinity and cgroup quota)
    cpu_jobs: int                # 90 % of them, at least 1
    jobs: int                    # the default: cpu_jobs, lowered by the memory ceiling
    mem_available: int | None    # bytes, None when unknown
    mem_cap: int | None          # None when no ceiling applies
    capped_by_memory: bool

    @property
    def mem_per_job(self) -> int | None:
        return MEM_PER_JOB_BYTES


def default_plan(*, cgroup_file: pathlib.Path | None = None, meminfo: pathlib.Path | None = None,
                 mem_per_job: int | None = None) -> JobsPlan:
    threads = available_threads(cgroup_file)
    cpu_jobs = max(1, math.floor(THREAD_FRACTION * threads))
    available = mem_available_bytes(meminfo)
    cap = memory_cap(available, mem_per_job)
    jobs = min(cpu_jobs, cap) if cap else cpu_jobs
    return JobsPlan(threads, cpu_jobs, jobs, available, cap, jobs < cpu_jobs)


def default_jobs(**kwargs) -> int:
    """The default number of build jobs: 90 % of the available threads (at least 1), within the memory ceiling."""
    return default_plan(**kwargs).jobs


def parse_jobs(value: object, source: str = "--jobs") -> int:
    """An integer >= 1 from user input; JobsError otherwise (booleans, floats, empty, zero and negatives included)."""
    if isinstance(value, bool):
        raise JobsError(value, source)
    if isinstance(value, int):
        number = value
    elif isinstance(value, str):
        text = value.strip()
        if not re.fullmatch(r"[0-9]+", text):
            raise JobsError(value, source)
        number = int(text)
    else:
        raise JobsError(value, source)
    if number < 1:
        raise JobsError(value, source)
    return number


def saved_jobs(config: dict | None) -> int | None:
    """The launcher.json ``jobs`` value, or None when absent or invalid (a hand-edited file never blocks the launcher)."""
    if not isinstance(config, dict) or "jobs" not in config:
        return None
    try:
        return parse_jobs(config["jobs"], "launcher.json")
    except JobsError:
        return None


@dataclass
class JobsChoice:
    jobs: int
    source: str                                  # "cli", "env", "saved" or "default"
    plan: JobsPlan
    warnings: list[tuple[str, dict]] = field(default_factory=list)   # (i18n key, values), see oot_native_i18n


def explicit_warnings(jobs: int, plan: JobsPlan) -> list[tuple[str, dict]]:
    """Warnings for a user-chosen count that goes past the threads or the memory ceiling (it is used anyway)."""
    warnings: list[tuple[str, dict]] = []
    if jobs > plan.threads:
        warnings.append(("jobs.warn_over_threads", {"jobs": jobs, "threads": plan.threads}))
    if plan.mem_cap and jobs > plan.mem_cap:
        warnings.append(("jobs.warn_over_memory", {"jobs": jobs, "cap": plan.mem_cap}))
    return warnings


def resolve_jobs(cli: object = None, environ: dict[str, str] | None = None, saved: object = None,
                 plan: JobsPlan | None = None) -> JobsChoice:
    """Pick the job count: CLI > OOT_NATIVE_JOBS > saved GUI value > default. Raises JobsError on invalid CLI/env input.

    ``cli`` and ``saved`` may be None (not given). An empty OOT_NATIVE_JOBS counts as not set.
    """
    plan = plan or default_plan()
    env = os.environ if environ is None else environ
    if cli is not None:
        jobs, source = parse_jobs(cli, "--jobs"), "cli"
    elif (env.get(ENV_VAR) or "").strip():
        jobs, source = parse_jobs(env[ENV_VAR], ENV_VAR), "env"
    elif (stored := saved_jobs({"jobs": saved}) if saved is not None else None) is not None:
        jobs, source = stored, "saved"
    else:
        jobs, source = plan.jobs, "default"
    choice = JobsChoice(jobs, source, plan)
    if source == "default":
        if plan.capped_by_memory:
            choice.warnings.append(("jobs.log_capped_memory", {"cpu_jobs": plan.cpu_jobs, "jobs": plan.jobs}))
    else:
        choice.warnings.extend(explicit_warnings(jobs, plan))
    return choice


def low_memory(jobs: int, plan: JobsPlan) -> bool:
    """True when the per-job RAM estimate exists and ``jobs`` would not fit in 85 % of the available RAM."""
    return bool(plan.mem_cap and jobs > plan.mem_cap)


def render_warnings(language: str, warnings: list[tuple[str, dict]]) -> list[str]:
    """The warnings of a JobsChoice as text in the interface language."""
    return [tr(language, key, **values) for key, values in warnings]
