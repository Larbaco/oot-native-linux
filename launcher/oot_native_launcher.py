#!/usr/bin/env python3
"""Small Linux GUI for the existing OoT native builder and generated runtime."""
from __future__ import annotations

import os
import pathlib
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
import json
import argparse
from tkinter import filedialog, messagebox, ttk

_SOURCE_HOME = pathlib.Path(__file__).resolve().parents[1]
if not getattr(sys, "frozen", False):
    sys.path.insert(0, str(_SOURCE_HOME))
from oot_native_i18n import (DEFAULT_LANGUAGE, LANGUAGES, doctor_detail_key,
                            doctor_label_key, dump_error_key, normalize_language,
                            resolve_language, tr)
import oot_native_jobs as njobs

TITLE_ID = "0400486024DEC000"
PACKAGE_HOME = pathlib.Path(os.environ.get("OOT_NATIVE_HOME") or (pathlib.Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else pathlib.Path(__file__).resolve().parents[1]))
_PACKAGED_BUILDER = PACKAGE_HOME / "builder" / "nxs2.py"
DEFAULT_BUILDER = _PACKAGED_BUILDER if _PACKAGED_BUILDER.is_file() else PACKAGE_HOME / "nxs2.py"
DEFAULT_OUT = pathlib.Path(os.environ.get("XDG_DATA_HOME", pathlib.Path.home() / ".local/share")) / "oot-native/build"
WINDOW_SIZES = ("1280x720", "1600x900", "1920x1080")
MARKER_NAME = ".oot-native-launcher.json"
MARKER_DATA = {"kind": "oot-native-launcher-build", "title_id": TITLE_ID}


def language_config_path(environ: dict[str, str] | None = None) -> pathlib.Path:
    environ = os.environ if environ is None else environ
    config_home = pathlib.Path(environ.get("XDG_CONFIG_HOME") or pathlib.Path.home() / ".config")
    return config_home / "oot-native" / "launcher.json"


def load_language_config(path: pathlib.Path | None = None) -> dict[str, object]:
    try:
        data = json.loads((path or language_config_path()).read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, UnicodeError, json.JSONDecodeError, TypeError):
        return {}


def update_config(changes: dict[str, object], path: pathlib.Path | None = None) -> bool:
    """Merge ``changes`` into launcher.json (a None value removes its key); the other keys are kept."""
    try:
        target = path or language_config_path()
        data = load_language_config(target)
        for key, value in changes.items():
            if value is None:
                data.pop(key, None)
            else:
                data[key] = value
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(data, ensure_ascii=False) + "\n", encoding="utf-8")
        return True
    except (OSError, UnicodeError, TypeError):
        return False


def save_language_config(language: str, path: pathlib.Path | None = None) -> bool:
    return update_config({"language": language}, path)


def save_jobs_config(jobs: int, default: int, path: pathlib.Path | None = None) -> bool:
    """Remember the GUI's build jobs; a value equal to the default is not stored, so the default keeps following the machine."""
    return update_config({"jobs": None if jobs == default else jobs}, path)


def package_env(language: str = DEFAULT_LANGUAGE) -> dict[str, str]:
    env = os.environ.copy()
    # PyInstaller sets a temporary extraction directory in LD_LIBRARY_PATH.
    # Keep it local to the launcher and give child tools their original value.
    original_library_path = env.pop("LD_LIBRARY_PATH_ORIG", None)
    if original_library_path is None:
        env.pop("LD_LIBRARY_PATH", None)
    else:
        env["LD_LIBRARY_PATH"] = original_library_path
    env["OOT_NATIVE_HOME"] = str(PACKAGE_HOME)
    builder_bin = PACKAGE_HOME / "builder" / "bin"
    env.setdefault("NXS2_RELEASE_PACKAGE", "1")
    env.setdefault("NXS2_BIN", str(builder_bin))
    env.setdefault("NXS2_LLD", str(builder_bin / "ld.lld"))
    env.setdefault("NXS2_ZBIC_UNPACK", str(builder_bin / "zbic-unpack"))
    env.setdefault("NXS2_DOTNET_ROOT", str(PACKAGE_HOME / "dotnet"))
    python_home = PACKAGE_HOME / "python"
    if python_home.is_dir():
        # The embedded interpreter is built before the package is relocated.
        # PYTHONHOME makes its stdlib and site-packages resolve beside this launcher.
        env["PYTHONHOME"] = str(python_home)
        bundled_font = PACKAGE_HOME / "fonts" / "DejaVuSans.ttf"
        if bundled_font.is_file():
            env.setdefault("STD_FONT", str(bundled_font))
            env.setdefault("LETTER_FONT", str(bundled_font))
    env.setdefault("DOTNET_ROOT", str(PACKAGE_HOME / "dotnet"))
    env["PATH"] = os.pathsep.join((str(PACKAGE_HOME / "dotnet"), str(builder_bin), env.get("PATH", "/usr/bin:/bin")))
    env["DOTNET_CLI_TELEMETRY_OPTOUT"] = "1"
    env["DOTNET_NOLOGO"] = "1"
    env["DOTNET_SYSTEM_GLOBALIZATION_INVARIANT"] = "1"
    env["OOT_NATIVE_LANG"] = normalize_language(language) or DEFAULT_LANGUAGE
    env["OOT_NATIVE_LAUNCHER"] = "1"   # the builder leaves the jobs warnings to the launcher, which already showed them
    return env


def dump_rejection_message(data: dict, language: str) -> str:
    reason = tr(language, dump_error_key(data.get("code")))
    return tr(language, "dump.rejected", reason=reason)


def run_dump_check(builder: pathlib.Path, nup: pathlib.Path, language: str) -> dict:
    python = Launcher._builder_python(language)
    try:
        result = subprocess.run([python, str(builder), "--game", TITLE_ID, "check-dump", str(nup), "--json"],
                                capture_output=True, text=True, env=package_env(language))
    except OSError as exc:
        raise RuntimeError(tr(language, "error_process_start")) from exc
    try:
        data = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(tr(language, "dump.invalid")) from exc
    if result.returncode or not data.get("ok"):
        raise RuntimeError(dump_rejection_message(data, language))
    return data


def doctor_lines(report: dict, language: str) -> list[str]:
    lines = []
    for check in report.get("checks", []):
        code = check.get("code")
        label = tr(language, doctor_label_key(code))
        if check.get("ok"):
            lines.append(f"✓ {label}: {tr(language, 'doctor.ok')}")
            continue
        if code == "space_45gb":
            detail = tr(language, "doctor.detail_space", available=check.get("available_gb", "?"))
        else:
            detail = tr(language, doctor_detail_key(code))
        lines.append(f"• {label}: {detail}")
    return lines


def owned_build_dir(out_dir: pathlib.Path) -> bool:
    marker = out_dir / MARKER_NAME
    if not marker.is_file() or marker.is_symlink():
        return False
    try:
        return json.loads(marker.read_text(encoding="utf-8")) == MARKER_DATA
    except (OSError, json.JSONDecodeError):
        return False


def prepare_build_dir(out_dir: pathlib.Path, language: str = DEFAULT_LANGUAGE) -> pathlib.Path:
    """Only let the builder touch an empty directory or one previously marked as ours."""
    out_dir = out_dir.expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    if owned_build_dir(out_dir):
        return out_dir
    if any(out_dir.iterdir()):
        raise ValueError(tr(language, "error_folder_not_empty", path=out_dir))
    marker = out_dir / MARKER_NAME
    try:
        with marker.open("x", encoding="utf-8") as f:
            json.dump(MARKER_DATA, f)
            f.write("\n")
    except FileExistsError:
        if not owned_build_dir(out_dir):
            raise ValueError(tr(language, "error_marker_invalid", path=marker))
    return out_dir


def runtime_command(out_dir: pathlib.Path, *, fullscreen: bool, resolution: str, silent: bool, gamemode: bool,
                    language: str = DEFAULT_LANGUAGE) -> list[str]:
    """Build a safe argv for the generated launcher, without going through nxs2.py's run parser."""
    out_dir = out_dir.expanduser().resolve()
    if not owned_build_dir(out_dir):
        raise ValueError(tr(language, "error_build_unavailable"))
    try:
        version = (out_dir / "version.txt").read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise ValueError(tr(language, "error_version_missing", error=exc)) from exc
    if not re.fullmatch(r"[A-Za-z0-9._-]+", version):
        raise ValueError(tr(language, "error_version_invalid"))
    executable = out_dir / "out" / f"play-oot-{version}"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(tr(language, "error_runtime_not_found", path=executable))

    args = [str(executable)]
    if fullscreen:
        args.append("--fullscreen")
    else:
        if resolution not in WINDOW_SIZES:
            raise ValueError(tr(language, "error_version_invalid"))
        args.extend(("--window", resolution))
    if silent:
        args.append("--silent")
    if gamemode:
        args.append("--gamemode")
    return args


class Launcher:
    def __init__(self, root: tk.Tk, language: str = DEFAULT_LANGUAGE):
        self.lang = normalize_language(language) or DEFAULT_LANGUAGE
        self.root = root
        self.root.title(tr(self.lang, "app_title"))
        self.root.minsize(700, 530)
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.process: subprocess.Popen[str] | None = None
        self.nup = tk.StringVar()
        self.builder = tk.StringVar(value=str(DEFAULT_BUILDER))
        self.out = tk.StringVar(value=str(DEFAULT_OUT))
        self.fullscreen = tk.BooleanVar(value=False)
        self.resolution = tk.StringVar(value="1600x900")
        self.silent = tk.BooleanVar(value=False)
        self.gamemode = tk.BooleanVar(value=False)
        self.plan = njobs.default_plan()
        initial = self._initial_jobs()
        self.jobs = tk.StringVar(value=str(initial))
        self.jobs_summary = tk.StringVar()
        self.jobs_priority = tk.StringVar()
        self._ui()
        self.jobs.trace_add("write", lambda *_: self._jobs_changed())
        self._jobs_changed()
        for line in self._startup_notes:
            self._append(line + "\n")
        self.root.after(100, self._pump)

    def _initial_jobs(self) -> int:
        """OOT_NATIVE_JOBS, else the saved value, else the default (the GUI has no --jobs)."""
        self._startup_notes: list[str] = []
        try:
            choice = njobs.resolve_jobs(None, saved=njobs.saved_jobs(load_language_config()), plan=self.plan)
        except njobs.JobsError as exc:
            self._startup_notes.append(tr(self.lang, "jobs_ignored_env", value=exc.value))
            choice = njobs.resolve_jobs(None, environ={}, saved=njobs.saved_jobs(load_language_config()), plan=self.plan)
        self._startup_notes.extend(njobs.render_warnings(self.lang, choice.warnings))
        return choice.jobs

    def _ui(self) -> None:
        body = ttk.Frame(self.root, padding=14)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text=tr(self.lang, "app_title"), font=("TkDefaultFont", 16, "bold")).pack(anchor="w")
        ttk.Label(body, text=tr(self.lang, "subtitle")).pack(anchor="w", pady=(2, 8))
        language_row = ttk.Frame(body)
        language_row.pack(anchor="e", fill="x", pady=(0, 6))
        ttk.Label(language_row, text=tr(self.lang, "language_label")).pack(side="left")
        current_display = tr(self.lang, "language_en" if self.lang == "en" else "language_pt")
        self.language_choice = tk.StringVar(value=current_display)
        self.language_combo = ttk.Combobox(
            language_row, textvariable=self.language_choice,
            values=(tr(self.lang, "language_en"), tr(self.lang, "language_pt")),
            width=22, state="readonly")
        self.language_combo.pack(side="right")
        self.language_combo.bind("<<ComboboxSelected>>", self._language_changed)

        self._path_row(body, tr(self.lang, "dump_label"), self.nup, self._pick_nup)
        self._path_row(body, tr(self.lang, "build_folder_label"), self.out, self._pick_out)
        ttk.Label(body, text=tr(self.lang, "legal_notice"), foreground="#7a3b00", wraplength=650).pack(anchor="w", pady=(0, 5))
        ttk.Label(body, text=tr(self.lang, "space_notice"), foreground="#7a3b00", wraplength=650).pack(anchor="w", pady=(0, 5))

        options = ttk.LabelFrame(body, text=tr(self.lang, "options_title"), padding=10)
        options.pack(fill="x", pady=(6, 10))
        ttk.Checkbutton(options, text=tr(self.lang, "fullscreen"), variable=self.fullscreen).grid(row=0, column=0, sticky="w")
        ttk.Label(options, text=tr(self.lang, "window_label")).grid(row=0, column=1, sticky="e", padx=(12, 4))
        ttk.Combobox(options, textvariable=self.resolution, values=("1280x720", "1600x900", "1920x1080"), width=13, state="readonly").grid(row=0, column=2, sticky="w")
        ttk.Checkbutton(options, text=tr(self.lang, "silent"), variable=self.silent).grid(row=1, column=0, sticky="w")
        ttk.Checkbutton(options, text=tr(self.lang, "gamemode"), variable=self.gamemode).grid(row=1, column=1, columnspan=2, sticky="w")
        jobs_row = ttk.Frame(options)
        jobs_row.grid(row=2, column=0, columnspan=3, sticky="w", pady=(7, 0))
        ttk.Label(jobs_row, text=tr(self.lang, "jobs_label")).pack(side="left")
        self.jobs_spin = ttk.Spinbox(jobs_row, textvariable=self.jobs, from_=1, to=max(64, self.plan.threads * 2), width=6)
        self.jobs_spin.pack(side="left", padx=(6, 10))
        self.jobs_spin.bind("<FocusOut>", lambda _e: self._save_jobs())
        ttk.Label(jobs_row, textvariable=self.jobs_summary).pack(side="left")
        ttk.Label(options, text=tr(self.lang, "jobs_default_hint", default=self.plan.jobs), foreground="#555", wraplength=620).grid(row=3, column=0, columnspan=3, sticky="w", pady=(3, 0))
        ttk.Label(options, textvariable=self.jobs_priority, foreground="#555").grid(row=4, column=0, columnspan=3, sticky="w", pady=(3, 0))

        controls = ttk.Frame(body)
        controls.pack(fill="x", pady=(0, 8))
        self.build_btn = ttk.Button(controls, text=tr(self.lang, "build_button"), command=self.build)
        self.build_btn.pack(side="left")
        self.run_btn = ttk.Button(controls, text=tr(self.lang, "run_button"), command=self.run, state="disabled")
        self.run_btn.pack(side="left", padx=8)
        self.out.trace_add("write", lambda *_: self._refresh_run())
        self._refresh_run()
        self.status = tk.StringVar(value=tr(self.lang, "status_ready"))
        ttk.Label(controls, textvariable=self.status).pack(side="right")

        self.log = tk.Text(body, height=16, wrap="word", state="disabled")
        self.log.pack(fill="both", expand=True)
        ttk.Label(body, text=tr(self.lang, "output_footer"), foreground="#555", wraplength=650).pack(anchor="w", pady=(8, 0))

    def _jobs_value(self) -> int | None:
        try:
            return njobs.parse_jobs(self.jobs.get())
        except njobs.JobsError:
            return None

    def _jobs_changed(self) -> None:
        value = self._jobs_value()
        self.jobs_summary.set(tr(self.lang, "jobs_summary", jobs=value, threads=self.plan.threads) if value else "")
        self.jobs_priority.set(tr(self.lang, "build_priority", jobs=value if value else "?"))

    def _save_jobs(self) -> None:
        value = self._jobs_value()
        if value is not None:
            save_jobs_config(value, self.plan.jobs)

    def _language_changed(self, _event=None) -> None:
        selected = self.language_choice.get()
        language = "pt-BR" if selected in ("Português (Brasil)", tr(self.lang, "language_pt")) else "en"
        saved = save_language_config(language)
        messagebox.showinfo(self.root.title(), tr(self.lang, "language_saved" if saved else "language_save_failed"))

    def _path_row(self, parent: ttk.Frame, label: str, var: tk.StringVar, command) -> None:
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=3)
        ttk.Label(row, text=label, width=16).pack(side="left")
        ttk.Entry(row, textvariable=var).pack(side="left", fill="x", expand=True)
        ttk.Button(row, text=tr(self.lang, "browse"), command=command).pack(side="left", padx=(6, 0))

    def _pick_nup(self) -> None:
        path = filedialog.askopenfilename(title=tr(self.lang, "dump_label"), filetypes=((tr(self.lang, "dump_filetype"), "*.nup"), (tr(self.lang, "all_files"), "*")))
        if path:
            self.nup.set(path)

    def _pick_out(self) -> None:
        path = filedialog.askdirectory(title=tr(self.lang, "build_folder_label"), mustexist=False)
        if path:
            self.out.set(path)

    def _validate(self, need_dump: bool) -> tuple[pathlib.Path, pathlib.Path] | None:
        builder = DEFAULT_BUILDER
        out = self._validate_out()
        if out is None:
            return None
        nup = pathlib.Path(self.nup.get()).expanduser()
        if not builder.is_file() or builder.name != "nxs2.py":
            messagebox.showerror(tr(self.lang, "error_title_builder"), tr(self.lang, "error_builder_not_found", path=builder))
            return None
        if need_dump and (not nup.is_file() or nup.suffix.lower() != ".nup"):
            messagebox.showerror(tr(self.lang, "error_dump_title"), tr(self.lang, "error_invalid_file"))
            return None
        return builder, out

    def _validate_out(self) -> pathlib.Path | None:
        out = pathlib.Path(self.out.get()).expanduser()
        if not out.is_absolute():
            messagebox.showerror(tr(self.lang, "error_path_title"), tr(self.lang, "error_path_absolute"))
            return None
        return out

    def build(self) -> None:
        paths = self._validate(True)
        if paths is None:
            return
        jobs = self._jobs_value()
        if jobs is None:
            messagebox.showerror(tr(self.lang, "jobs_invalid_title"), tr(self.lang, "jobs_invalid_gui"))
            return
        plan = njobs.default_plan()   # the RAM available now
        for warning in njobs.render_warnings(self.lang, njobs.explicit_warnings(jobs, plan)):
            self._append(warning + "\n")
        self._save_jobs()
        builder, out = paths
        nup = pathlib.Path(self.nup.get()).expanduser()
        try:
            check = self._check_dump(builder, nup)
        except RuntimeError as exc:
            messagebox.showerror(tr(self.lang, "error_dump_title"), str(exc))
            return
        self._append(tr(self.lang, "dump.accepted", version=check.get("version", "2.0.0")) + "\n")
        if check.get("warnings"):
            self._append(tr(self.lang, "dump.warning_count", count=len(check["warnings"])) + "\n")
        try:
            doctor = subprocess.run([self._builder_python(self.lang), str(builder), "doctor", "--release", "--json", "--out", str(out)], capture_output=True, text=True, env=package_env(self.lang))
            report = json.loads(doctor.stdout)
            self._append(tr(self.lang, "cli_doctor_ready" if report.get("ok") else "cli_doctor_missing") + "\n")
            if doctor.returncode:
                messagebox.showerror(tr(self.lang, "error_doctor_title"), tr(self.lang, "error_doctor_failed") + "\n" + "\n".join(doctor_lines(report, self.lang)))
                return
        except (OSError, json.JSONDecodeError):
            messagebox.showerror(tr(self.lang, "error_doctor_title"), tr(self.lang, "error_doctor_unavailable", error=tr(self.lang, "error_process_start")))
            return
        try:
            out = prepare_build_dir(out, self.lang)
        except (OSError, ValueError) as exc:
            messagebox.showerror(tr(self.lang, "error_build_folder_title"), str(exc) or tr(self.lang, "error_build_unavailable"))
            return
        try:
            python = self._builder_python(self.lang)
        except RuntimeError as exc:
            messagebox.showerror(tr(self.lang, "error_python_title"), str(exc))
            return
        cmd = [python, str(builder), "--game", TITLE_ID, "build", str(nup), "--out", str(out), "--linux", "--jobs", str(jobs)]
        self._start(cmd, tr(self.lang, "status_building"), "build")

    def run(self) -> None:
        out = self._validate_out()
        if out is None:
            return
        if self.gamemode.get():
            if not shutil.which("gamemoderun"):
                messagebox.showerror(tr(self.lang, "error_gamemode_title"), tr(self.lang, "error_gamemode_missing"))
                return
        try:
            cmd = runtime_command(out, fullscreen=self.fullscreen.get(), resolution=self.resolution.get(), silent=self.silent.get(), gamemode=self.gamemode.get(), language=self.lang)
        except ValueError as exc:
            messagebox.showerror(tr(self.lang, "error_run_title"), tr(self.lang, "error_runtime_unavailable"))
            return
        self._start(cmd, tr(self.lang, "status_running"), "run")

    @staticmethod
    def _builder_python(language: str = DEFAULT_LANGUAGE) -> str:
        bundled = PACKAGE_HOME / "python" / "bin" / "python3"
        if bundled.is_file() and os.access(bundled, os.X_OK):
            return str(bundled)
        configured = os.environ.get("OOT_NXS2_PYTHON")
        candidate = shutil.which(configured) if configured else shutil.which("python3")
        if not candidate:
            raise RuntimeError(tr(language, "error_python_missing"))
        try:
            check = subprocess.run([candidate, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"], capture_output=True, text=True, env=package_env(language))
        except OSError as exc:
            raise RuntimeError(tr(language, "error_python_start", path=candidate, error=tr(language, "error_process_start"))) from exc
        version = check.stdout.strip()
        match = re.fullmatch(r"(\d+)\.(\d+)", version)
        if check.returncode or not match or tuple(map(int, match.groups())) < (3, 12):
            raise RuntimeError(tr(language, "error_python_version", path=candidate, version=version or "unavailable"))
        return candidate

    def _check_dump(self, builder: pathlib.Path, nup: pathlib.Path) -> dict:
        return run_dump_check(builder, nup, self.lang)

    def _refresh_run(self) -> None:
        if not hasattr(self, "run_btn"):
            return
        out = pathlib.Path(self.out.get()).expanduser()
        try:
            ready = owned_build_dir(out) and runtime_command(out, fullscreen=False, resolution="1600x900", silent=False, gamemode=False, language=self.lang)
        except (OSError, ValueError):
            ready = False
        self.run_btn.configure(state="normal" if ready else "disabled")

    def _start(self, command: list[str], label: str, kind: str) -> None:
        if self.process and self.process.poll() is None:
            return
        env = package_env(self.lang)
        env["PYTHONUNBUFFERED"] = "1"
        self._append("$ " + " ".join(command) + "\n")
        self.status.set(label)
        self.build_btn.configure(state="disabled")
        self.run_btn.configure(state="disabled")

        def worker() -> None:
            try:
                self.process = subprocess.Popen(
                    (["nice", "-n", "19", "ionice", "-c", "3", *command]
                     if kind == "build" and shutil.which("nice") and shutil.which("ionice")
                     else command),
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env,
                )
                assert self.process.stdout is not None
                for line in self.process.stdout:
                    self.events.put(("line", line))
                code = self.process.wait()
                self.events.put(("done", (code, kind)))
            except Exception as exc:  # present process/setup errors in the window
                    self.events.put(("error", (kind, tr(self.lang, "error_process_start"))))

        threading.Thread(target=worker, daemon=True).start()

    def _pump(self) -> None:
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "line":
                    self._append(str(value))
                elif kind == "done":
                    code, process_kind = value  # type: ignore[misc]
                    code = int(code)
                    self.status.set(tr(self.lang, "status_completed") if code == 0 else tr(self.lang, "status_failed_code", code=code))
                    self.build_btn.configure(state="normal")
                    out = pathlib.Path(self.out.get()).expanduser()
                    ready_after_build = process_kind == "build" and code == 0
                    retryable_runtime = process_kind == "run"
                    self.run_btn.configure(state="normal" if (ready_after_build or retryable_runtime) and owned_build_dir(out) and self._has_runtime(out) else "disabled")
                else:
                    process_kind, error = value  # type: ignore[misc]
                    self.status.set(tr(self.lang, "status_failed_start"))
                    self._append(tr(self.lang, "error_message_prefix", error=tr(self.lang, "error_process_start")) + "\n")
                    self.build_btn.configure(state="normal")
                    out = pathlib.Path(self.out.get()).expanduser()
                    self.run_btn.configure(state="normal" if process_kind == "run" and owned_build_dir(out) and self._has_runtime(out) else "disabled")
        except queue.Empty:
            pass
        self.root.after(100, self._pump)

    def _append(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    @staticmethod
    def _has_runtime(out: pathlib.Path) -> bool:
        try:
            runtime_command(out, fullscreen=False, resolution="1600x900", silent=False, gamemode=False)
            return True
        except (OSError, ValueError):
            return False


def main() -> int:
    root = tk.Tk()
    Launcher(root)
    root.mainloop()
    return 0


class LocalizedArgumentParser(argparse.ArgumentParser):
    def __init__(self, *args, language: str = DEFAULT_LANGUAGE, **kwargs):
        self.language = language
        kwargs.setdefault("add_help", False)
        super().__init__(*args, **kwargs)

    def format_usage(self) -> str:
        return super().format_usage().replace("usage:", tr(self.language, "cli_usage") + ":", 1)

    def format_help(self) -> str:
        return (super().format_help()
                .replace("usage:", tr(self.language, "cli_usage") + ":", 1)
                .replace("options:", tr(self.language, "cli_options") + ":", 1))

    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        self.exit(2, tr(self.language, "cli_argument_error", error=message) + "\n")


def jobs_argument(language: str):
    def convert(text: str) -> int:
        try:
            return njobs.parse_jobs(text)
        except njobs.JobsError as exc:
            raise argparse.ArgumentTypeError(tr(language, "jobs.invalid", source="--jobs", value=exc.value)) from None
    return convert


def make_parser(language: str) -> argparse.ArgumentParser:
    parser = LocalizedArgumentParser(description=tr(language, "cli_description"), language=language)
    parser.add_argument("-h", "--help", action="help", help=tr(language, "cli_help_help"))
    parser.add_argument("--lang", choices=LANGUAGES, help=tr(language, "cli_help_lang"))
    parser.add_argument("--check", metavar="NUP", help=tr(language, "cli_help_check"))
    parser.add_argument("--build", metavar="NUP", help=tr(language, "cli_help_build"))
    parser.add_argument("--out", metavar="DIR", help=tr(language, "cli_help_out"))
    parser.add_argument("--jobs", "-j", metavar="N", type=jobs_argument(language), help=tr(language, "cli_help_jobs").replace("%", "%%"))
    return parser


def cli_main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    lang_parser = argparse.ArgumentParser(add_help=False)
    lang_parser.add_argument("--lang")
    lang_args, _ = lang_parser.parse_known_args(argv)
    language = resolve_language(lang_args.lang, config=load_language_config())
    parser = make_parser(language)
    args = parser.parse_args(argv)
    if not (args.check or args.build):
        root = tk.Tk()
        Launcher(root, language=language)
        root.mainloop()
        return 0

    try:
        choice = njobs.resolve_jobs(args.jobs, saved=njobs.saved_jobs(load_language_config()))
    except njobs.JobsError as exc:   # OOT_NATIVE_JOBS
        parser.error(tr(language, "jobs.invalid", source=exc.source, value=exc.value))

    builder = DEFAULT_BUILDER
    if not builder.is_file():
        print(tr(language, "error_builder_not_found", path=builder), file=sys.stderr)
        return 2
    try:
        nup = pathlib.Path(args.check or args.build).expanduser().resolve()
        checked = run_dump_check(builder, nup, language)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(tr(language, "cli_dump_accepted", version=checked.get("version", "2.0.0")))
    if checked.get("warnings"):
        print(tr(language, "dump.warning_count", count=len(checked["warnings"])))

    if not args.build:
        return 0
    if not args.out:
        parser.error(tr(language, "cli_build_requires_out"))
    python = Launcher._builder_python(language)
    try:
        doctor = subprocess.run([python, str(builder), "doctor", "--release", "--json", "--out", str(args.out)],
                                capture_output=True, text=True, env=package_env(language))
    except OSError as exc:
        print(tr(language, "error_doctor_unavailable", error=tr(language, "error_process_start")), file=sys.stderr)
        return 1
    try:
        report = json.loads(doctor.stdout)
    except json.JSONDecodeError:
        report = {"ok": False, "checks": [], "error": doctor.stderr.strip()}
    if doctor.returncode or not report.get("ok"):
        print(tr(language, "cli_doctor_missing"), file=sys.stderr)
        for line in doctor_lines(report, language):
            if line.startswith("•"):
                print(line, file=sys.stderr)
        return 1
    print(tr(language, "cli_doctor_ready"))
    try:
        out = prepare_build_dir(pathlib.Path(args.out), language)
    except (OSError, ValueError) as exc:
        print(str(exc) or tr(language, "error_build_unavailable"), file=sys.stderr)
        return 1
    for warning in njobs.render_warnings(language, choice.warnings):
        print(warning, file=sys.stderr)
    command = [python, str(builder), "--game", TITLE_ID, "build", str(nup), "--out", str(out), "--linux", "--jobs", str(choice.jobs)]
    if shutil.which("nice") and shutil.which("ionice"):
        command = ["nice", "-n", "19", "ionice", "-c", "3", *command]
    try:
        return subprocess.call(command, env=package_env(language))
    except OSError:
        print(tr(language, "error_process_start"), file=sys.stderr)
        return 1


def main_entry() -> int:
    # Parse the interface language before argparse renders help or errors.
    return cli_main()


if __name__ == "__main__":
    raise SystemExit(main_entry())
