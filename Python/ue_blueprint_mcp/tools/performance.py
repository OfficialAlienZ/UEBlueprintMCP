"""
Performance tools - measure, audit and optimise an Unreal project.

Plain tools forward to C++ actions (get_performance_stats, audit_*, optimize_*, set_project_config,
manage_level_streaming, build_level). Composite tools orchestrate the engine's own profilers from Python:
CSV Profiler (capture_performance), profilegpu (profile_gpu), stat dumps, view modes, memreport,
Unreal trace, and packaged-build profiling (package_project / profile_packaged_build).
See docs/OPTIMIZE.md for the workflow.
"""

import asyncio
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable
from mcp.types import Tool, TextContent, ImageContent

from ..connection import get_connection
from . import editor as editor_tools
from .perf_analysis import analyze_csv, analyze_trace_exports, compare_analyses, parse_gpu_profile, scan_cpp_source

MAX_CAPTURE_SECONDS = 120.0
CSV_WAIT_SECONDS = 20.0

# Long-lived child processes (RunUAT packaging, packaged game profiling), one per kind.
_JOBS: dict[str, dict[str, Any]] = {}


# =============================================================================
# Connection helpers
# =============================================================================

def _send(command_type: str, params: dict | None = None) -> dict:
    conn = get_connection()
    if not conn.is_connected:
        conn.connect()
    return conn.send_command(command_type, params).to_dict()


def _console(command: str, world: str = "auto") -> dict:
    return _send("execute_console_command", {"command": command, "world": world})


def _log_cursor() -> int:
    result = _send("get_output_log", {"limit": 1})
    return int(result.get("next_id", 0)) if result.get("success") else 0


def _log_since(cursor: int, limit: int = 1000) -> list[str]:
    result = _send("get_output_log", {"since_id": cursor, "limit": limit})
    if not result.get("success"):
        return []
    return [str(entry.get("message", "")) for entry in result.get("entries", [])]


def _paths() -> dict:
    stats = _send("get_performance_stats")
    if not stats.get("success"):
        raise RuntimeError(stats.get("error", "get_performance_stats failed"))
    return stats


def _text(payload: dict) -> list[TextContent]:
    return [TextContent(type="text", text=json.dumps(payload, indent=2))]


def _wait_for_new_file(directory: Path, pattern: str, since: float, timeout: float = CSV_WAIT_SECONDS) -> Path | None:
    """Newest file in directory matching pattern, modified after `since`, once its size stopped changing."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        candidates = [p for p in directory.glob(pattern) if p.stat().st_mtime >= since - 1.0] if directory.exists() else []
        if candidates:
            newest = max(candidates, key=lambda p: p.stat().st_mtime)
            size = newest.stat().st_size
            time.sleep(1.0)
            if newest.exists() and newest.stat().st_size == size and size > 0:
                return newest
        time.sleep(0.5)
    return None


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


# =============================================================================
# Composite tools
# =============================================================================

def _capture_performance(args: dict) -> dict:
    seconds = min(max(float(args.get("seconds", 10)), 2.0), MAX_CAPTURE_SECONDS)
    skip_frames = int(args.get("skip_frames", 30))
    stats = _paths()
    csv_dir = Path(stats["saved_dir"]) / "Profiling" / "CSV"

    started_at = time.time()
    start = _console("csvprofile start")
    if not start.get("success") or start.get("handled") is False:
        return {"success": False, "error": f"csvprofile start was not accepted: {start}. The CSV Profiler exists in Development/Debug builds and the editor, not in Shipping."}
    time.sleep(seconds)
    _console("csvprofile stop")

    csv_file = _wait_for_new_file(csv_dir, "*.csv", started_at)
    if not csv_file:
        return {"success": False, "error": f"No CSV file appeared in {csv_dir} after stopping the capture. Check get_output_log for LogCsvProfiler messages."}

    try:
        analysis = analyze_csv(_read_text(csv_file), skip_frames=skip_frames)
    except ValueError as exc:
        return {"success": False, "error": str(exc), "file": str(csv_file)}

    notes = []
    if not stats.get("pie_running"):
        notes.append("Captured in the editor viewport, not PIE. Editor overhead inflates game/render time; use start_pie (or profile_packaged_build) for realistic numbers.")
    notes.append("Editor timings are not packaged-build timings: confirm important findings with package_project + profile_packaged_build.")
    return {"success": True, "file": str(csv_file), "capture_seconds": seconds, "notes": notes, **analysis}


def _analyze_performance_csv(args: dict) -> dict:
    path = args.get("path")
    if not path:
        return {"success": False, "error": "path is required"}
    csv_file = Path(path).expanduser()
    if not csv_file.is_file():
        return {"success": False, "error": f"File not found: {csv_file}"}
    try:
        return {"success": True, "file": str(csv_file), **analyze_csv(_read_text(csv_file), skip_frames=int(args.get("skip_frames", 30)))}
    except ValueError as exc:
        return {"success": False, "error": str(exc), "file": str(csv_file)}


def _profile_gpu(args: dict) -> dict:
    _console("r.ProfileGPU.ShowUI 0")
    cursor = _log_cursor()
    started = _console("profilegpu")
    if not started.get("success"):
        return {"success": False, "error": f"profilegpu failed: {started}"}

    messages: list[str] = []
    deadline = time.monotonic() + float(args.get("wait_seconds", 6))
    parsed: dict = {}
    while time.monotonic() < deadline:
        time.sleep(1.0)
        messages = _log_since(cursor)
        parsed = parse_gpu_profile(messages)
        if parsed:
            time.sleep(0.5)  # let the rest of the hierarchy arrive
            messages = _log_since(cursor)
            parsed = parse_gpu_profile(messages)
            break

    result: dict[str, Any] = {"success": True, **parsed}
    if not parsed:
        result["note"] = ("No GPU hierarchy found in the log. The editor window must be visible and rendering; GPU timestamp "
                          "queries may be unsupported by this RHI (check get_performance_stats rhi). Raw log lines are included.")
    if args.get("include_raw") or not parsed:
        result["raw_lines"] = [m for m in messages if m.strip()][:120]
    return result


def _dump_cpu_stats(args: dict) -> dict:
    mode = args.get("mode", "average")
    root = str(args.get("root", "GameThread"))
    if not re.fullmatch(r"[A-Za-z0-9_]+", root):
        return {"success": False, "error": "root must be letters, digits or underscore (e.g. GameThread, Game, Anim)"}
    ms = float(args.get("min_ms", 0.2))
    frames = int(args.get("frames", 120))
    commands = {
        "frame": f"stat dumpframe -ms={ms} -root={root}",
        "average": f"stat dumpave -root={root} -num={frames} -ms={ms}",
        "hitches": "stat dumphitches",
    }
    if mode not in commands:
        return {"success": False, "error": "mode must be frame, average or hitches"}
    cursor = _log_cursor()
    started = _console(commands[mode])
    if not started.get("success"):
        return {"success": False, "error": f"{commands[mode]} failed: {started}"}
    time.sleep(float(args.get("wait_seconds", 4 if mode == "frame" else max(4, frames / 30 + 2))))
    if mode == "hitches":
        _console("stat dumphitches")  # toggle off
    lines = [m for m in _log_since(cursor) if m.strip()]
    return {
        "success": True,
        "command": commands[mode],
        "line_count": len(lines),
        "lines": lines[:int(args.get("max_lines", 150))],
        "note": "Raw engine stat output (the format differs between engine versions). Stat groups: Game, Anim, Physics, AI, Slate, Niagara, ...",
    }


def _compare_performance(args: dict) -> dict:
    """Compare two CSV captures: before/after a fix, or editor vs packaged build."""
    paths = [args.get("before"), args.get("after")]
    if not all(paths):
        return {"success": False, "error": "before and after (paths to CSV Profiler files) are required. capture_performance and profile_packaged_build both return their file path."}
    skip = int(args.get("skip_frames", 30))
    analyses = []
    for path in paths:
        csv_file = Path(str(path)).expanduser()
        if not csv_file.is_file():
            return {"success": False, "error": f"File not found: {csv_file}"}
        try:
            analyses.append(analyze_csv(_read_text(csv_file), skip_frames=skip))
        except ValueError as exc:
            return {"success": False, "error": f"{csv_file}: {exc}"}
    label_before = str(args.get("label_before", "before"))
    label_after = str(args.get("label_after", "after"))
    return {"success": True, "before_file": str(paths[0]), "after_file": str(paths[1]),
            **compare_analyses(analyses[0], analyses[1], label_before, label_after),
            "note": "Only compare captures of the same scene and camera path, with the same resolution and settings. "
                    "For editor vs packaged use label_before=editor, label_after=packaged: the gap is the editor overhead."}


_PHYSICS_ROOTS = ["Physics", "Collision"]


def _profile_physics(args: dict) -> dict:
    """Physics cost: stat dumps of the physics groups plus the project's physics settings."""
    roots = args.get("roots") or _PHYSICS_ROOTS
    dumps = []
    for root in roots:
        dumps.append({"root": root, **_dump_cpu_stats({"mode": "average", "root": root, "min_ms": args.get("min_ms", 0.05),
                                                       "frames": args.get("frames", 120), "max_lines": args.get("max_lines", 60)})})
    settings = _send("get_project_config", {"file": "Engine", "section": "/Script/Engine.PhysicsSettings"})
    result: dict[str, Any] = {
        "success": True,
        "stat_dumps": dumps,
        "physics_settings": settings if settings.get("success") else {"error": settings.get("error")},
        "checklist": [
            "Many simulating bodies: turn off Simulate Physics / put to sleep when idle; lower sleep thresholds; use simple collision.",
            "Complex collision as simple on moving meshes is very costly: run audit_assets asset_type=static_mesh and fix with optimize_static_mesh collision=box/convex.",
            "Many overlap/hit events: disable Generate Overlap Events and Simulation Generates Hit Events where unused; use collision channels.",
            "Per-frame traces from Blueprints/C++: run scan_project_cpp (trace pattern) and audit_assets asset_type=blueprint.",
            "Fixed physics step: with substepping, lower Max Substep Delta Time / Max Substeps in the settings above (set_project_config file=Engine).",
            "Ragdolls and cloth: limit counts, use LOD'd physics assets, disable cloth at distance.",
        ],
        "note": "Stat dumps are raw engine output; empty lines mean this root has no data in this engine build or the game was not simulating. "
                "Try dump_cpu_stats with root=GameThread and look for physics entries, or run it during PIE with many physics objects active.",
    }
    return result


_SOURCE_SKIP_DIRS = {"ThirdParty", "Intermediate", "Binaries", "Saved", ".git", "node_modules"}
_SOURCE_EXTENSIONS = {".cpp", ".h", ".inl"}
MAX_SOURCE_FILES = 4000
MAX_SOURCE_FILE_BYTES = 1_500_000


def _collect_source_files(roots: list[Path]) -> list[tuple[str, str]]:
    files: list[tuple[str, str]] = []
    for root in roots:
        for current, dirs, names in os.walk(root):
            dirs[:] = [d for d in dirs if d not in _SOURCE_SKIP_DIRS]
            for name in names:
                path = Path(current) / name
                if path.suffix.lower() not in _SOURCE_EXTENSIONS:
                    continue
                try:
                    if path.stat().st_size > MAX_SOURCE_FILE_BYTES:
                        continue
                    files.append((str(path.relative_to(root.parent)), _read_text(path)))
                except OSError:
                    continue
                if len(files) >= MAX_SOURCE_FILES:
                    return files
    return files


def _scan_project_cpp(args: dict) -> dict:
    """Find C++ code behind a CPU hotspot: pattern scan and/or symbol search in the project's own source."""
    source_dir = args.get("source_dir")
    if source_dir:
        roots = [Path(str(source_dir)).expanduser()]
    else:
        project = Path(_paths()["project_dir"])
        roots = [p for p in [project / "Source"] if p.is_dir()]
        plugins = project / "Plugins"
        if plugins.is_dir():
            roots += [p / "Source" for p in plugins.glob("*/") if (p / "Source").is_dir()]
            roots += [p / "Source" for p in plugins.glob("*/*/") if (p / "Source").is_dir()]
    roots = [r for r in roots if r.is_dir()]
    if not roots:
        return {"success": True, "files_scanned": 0, "note": "This project has no C++ source (Blueprint-only): CPU cost is in Blueprints. Use audit_assets asset_type=blueprint and find_in_blueprints."}

    files = _collect_source_files(roots)
    result: dict[str, Any] = {"success": True, "roots": [str(r) for r in roots]}

    names = [str(n) for n in (args.get("names") or []) if str(n).strip()]
    if names:
        symbols = []
        for name in names[:20]:
            pattern = re.compile(re.escape(name))
            hits = []
            for path, content in files:
                for number, line in enumerate(content.splitlines(), 1):
                    if pattern.search(line):
                        hits.append({"file": path, "line": number, "code": line.strip()[:160]})
                        if len(hits) >= 30:
                            break
                if len(hits) >= 30:
                    break
            symbols.append({"name": name, "hits": hits})
        result["symbols"] = symbols
    if args.get("patterns", not names):
        result.update(scan_cpp_source(files, int(args.get("max_hits", 15))))
    result["files_scanned"] = len(files)
    result["note"] = ("Heuristic text scan, not a profiler: it lists code worth reading. Confirm with dump_cpu_stats / capture_performance "
                      "(stat names usually match function or class names), then edit the C++ and rebuild. This MCP cannot edit or compile your game code.")
    return result


def _capture_viewmode(args: dict) -> list[TextContent | ImageContent]:
    modes = args.get("modes") or ["shadercomplexity"]
    allowed = {"shadercomplexity", "shadercomplexitywithquadoverdraw", "quadoverdraw", "lightcomplexity", "lodcoloration",
               "lightmapdensity", "wireframe", "unlit", "detaillighting", "reflectionoverride", "lit"}
    bad = [m for m in modes if m not in allowed]
    if bad:
        return _text({"success": False, "error": f"Unknown view mode(s) {bad}. Allowed: {sorted(allowed)}"})
    pie = _send("get_pie_status")
    if pie.get("state") != "running":
        return _text({"success": False, "error": "View modes are captured in the running PIE game. Call start_pie first and wait until it is running."})

    content: list[TextContent | ImageContent] = []
    summaries = []
    try:
        for mode in modes:
            _console(f"viewmode {mode}", world="pie")
            time.sleep(float(args.get("wait_seconds", 1.5)))
            shot = editor_tools._take_screenshot_sync({"target": "pie", "label": mode, "max_width": int(args.get("max_width", 960))})
            summaries.append({"mode": mode, "meta": json.loads(shot[0].text)})
            content.extend(shot[1:])
    finally:
        _console("viewmode lit", world="pie")
    legend = {
        "shadercomplexity": "green = cheap, red/white = expensive pixel shading (materials, overdraw).",
        "quadoverdraw": "bright = many overlapping triangles/quads (overdraw, tiny triangles).",
        "lightcomplexity": "more light overlap = brighter; reduce overlapping dynamic lights.",
        "lodcoloration": "colours show which LOD is drawn; missing LODs show as the base colour everywhere.",
    }
    return _text({"success": True, "views": summaries, "legend": {m: legend[m] for m in modes if m in legend}}) + content


def _parse_memreport(text: str) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current: str | None = None
    for line in text.splitlines():
        begin = re.match(r'MemReport: Begin command "(.*)"', line)
        if begin:
            current = begin.group(1)
            sections[current] = []
        elif line.startswith("MemReport: End command"):
            current = None
        elif current is not None and line.strip():
            sections[current].append(line.rstrip())
    return sections


def _get_memory_report(args: dict) -> dict:
    stats = _paths()
    folder = Path(stats["saved_dir"]) / "Profiling" / "MemReports"
    started_at = time.time()
    run = _console("memreport -full")
    if not run.get("success"):
        return {"success": False, "error": f"memreport failed: {run}"}
    report = _wait_for_new_file(folder, "*.memreport", started_at, timeout=30.0)
    if not report:
        return {"success": False, "error": f"No .memreport appeared in {folder}."}
    sections = _parse_memreport(_read_text(report))
    wanted = [str(s).lower() for s in (args.get("sections") or [])]
    per = int(args.get("lines_per_section", 20))
    chosen = {name: lines for name, lines in sections.items() if not wanted or any(w in name.lower() for w in wanted)}
    return {
        "success": True,
        "file": str(report),
        "section_names": list(sections.keys()),
        "sections": {name: lines[:per] for name, lines in chosen.items()},
        "hint": "Look at the texture / static mesh / skeletal mesh object lists (largest first) and 'Streaming' sections; fix the biggest assets with optimize_texture / optimize_static_mesh.",
    }


def _trace_control(args: dict) -> dict:
    action = args.get("action", "status")
    channels = str(args.get("channels", "cpu,gpu,frame,bookmark"))
    if not re.fullmatch(r"[A-Za-z0-9_,]+", channels):
        return {"success": False, "error": "channels may contain letters, digits, comma and underscore only"}
    commands = {"start": f"Trace.Start {channels}", "stop": "Trace.Stop", "status": "Trace.Status"}
    if action not in commands:
        return {"success": False, "error": "action must be start, stop or status"}
    cursor = _log_cursor()
    run = _console(commands[action])
    time.sleep(1.0)
    result: dict[str, Any] = {"success": run.get("success", False), "command": commands[action],
                              "log": [m for m in _log_since(cursor) if m.strip()][:20]}
    stats = _send("get_performance_stats")
    if stats.get("success"):
        traces = list(Path(stats["saved_dir"]).rglob("*.utrace"))
        if traces:
            newest = max(traces, key=lambda p: p.stat().st_mtime)
            result["latest_trace"] = {"file": str(newest), "megabytes": round(newest.stat().st_size / 1048576, 2)}
    result["note"] = ("Analyse the .utrace with analyze_trace (needs UnrealInsights from UE 5.1+ with the command-line exporters; UE 4.27 ships Insights but without them). "
                      "Otherwise use capture_performance / profile_gpu / dump_cpu_stats for numbers the AI can read.")
    return result


def _find_insights(engine_dir: Path, explicit: str | None) -> Path | None:
    env = os.environ.get("UEBLUEPRINTMCP_INSIGHTS")
    for candidate in [explicit, env]:
        if candidate and Path(candidate).expanduser().is_file():
            return Path(candidate).expanduser()
    binaries = engine_dir / "Binaries"
    for relative in ("Win64/UnrealInsights.exe", "Mac/UnrealInsights.app/Contents/MacOS/UnrealInsights", "Linux/UnrealInsights"):
        if (binaries / relative).is_file():
            return binaries / relative
    return None


def _engine_version(engine_dir: Path) -> tuple[int, int] | None:
    try:
        info = json.loads((engine_dir / "Build" / "Build.version").read_text(encoding="utf-8"))
        return int(info["MajorVersion"]), int(info["MinorVersion"])
    except (OSError, ValueError, KeyError):
        return None


def _analyze_trace(args: dict) -> dict:
    """Export a .utrace to CSV with UnrealInsights (command line, no UI) and aggregate it into a compact report."""
    stats = _paths()
    saved = Path(stats["saved_dir"])
    trace_arg = args.get("trace")
    if trace_arg:
        trace = Path(str(trace_arg)).expanduser()
    else:
        traces = list(saved.rglob("*.utrace"))
        if not traces:
            return {"success": False, "error": f"No .utrace found under {saved}. Record one with trace_control action=start, play, then action=stop."}
        trace = max(traces, key=lambda p: p.stat().st_mtime)
    if not trace.is_file():
        return {"success": False, "error": f"Trace not found: {trace}"}

    insights = _find_insights(Path(stats["engine_dir"]), args.get("insights_path"))
    if not insights:
        return {"success": False, "trace": str(trace),
                "error": "UnrealInsights was not found under <Engine>/Binaries (Win64/UnrealInsights.exe, Mac/UnrealInsights.app, Linux/UnrealInsights). "
                         "Build the UnrealInsights target, or pass insights_path / set UEBLUEPRINTMCP_INSIGHTS."}
    version = _engine_version(Path(stats["engine_dir"]))
    if version and version < (5, 1) and not args.get("insights_path"):
        return {"success": False, "trace": str(trace), "insights": str(insights),
                "error": f"UnrealInsights {version[0]}.{version[1]} is installed but has no command-line CSV export: it only understands -OpenTraceFile, "
                         "-AutoQuit and -ExecOnAnalysisCompleteCmd for its own test commands, and TimingInsights.Export* arrived in UE 5.1. "
                         "Open the trace in the Insights GUI (export from the Timing view), or pass insights_path of a 5.1+ UnrealInsights "
                         "(trace formats are not guaranteed to be compatible across versions). capture_performance, profile_gpu and dump_cpu_stats work on 4.27."}

    threads_filter = str(args.get("threads", "*"))
    if not re.fullmatch(r"[A-Za-z0-9_ #*?,.-]+", threads_filter):
        return {"success": False, "error": "threads may contain letters, digits, spaces and the characters _ # * ? , . - only"}
    window = ""
    for key in ("start_time", "end_time"):
        if args.get(key) is not None:
            window += f" -{'startTime' if key == 'start_time' else 'endTime'}={float(args[key])}"

    out_dir = saved / "MCP" / f"trace_export_{int(time.time())}"
    out_dir.mkdir(parents=True, exist_ok=True)
    threads_csv, timers_csv, events_csv = out_dir / "Threads.csv", out_dir / "Timers.csv", out_dir / "TimingEvents.csv"
    rsp = out_dir / "export.rsp"
    rsp.write_text(
        f'TimingInsights.ExportThreads "{threads_csv}"\n'
        f'TimingInsights.ExportTimers "{timers_csv}"\n'
        f'TimingInsights.ExportTimingEvents "{events_csv}" -columns="ThreadId,TimerId,StartTime,EndTime,Depth" '
        f'-threads="{threads_filter}" -timers="*"{window}\n',
        encoding="utf-8",
    )
    log_path = out_dir / "insights.log"
    timeout = min(max(float(args.get("timeout_seconds", 600)), 30.0), 3600.0)
    command = [str(insights), f"-OpenTraceFile={trace}", f"-ABSLOG={log_path}", "-NoUI", "-AutoQuit", f"-ExecOnAnalysisCompleteCmd=@={rsp}"]
    try:
        completed = subprocess.run(command, capture_output=True, timeout=timeout, text=True, errors="replace")
    except subprocess.TimeoutExpired:
        return {"success": False, "error": f"UnrealInsights did not finish within {timeout:.0f}s (big trace?). Narrow it with start_time / end_time / threads.", "export_dir": str(out_dir)}
    missing = [p.name for p in (threads_csv, timers_csv, events_csv) if not p.is_file()]
    if missing:
        tail = log_path.read_text(errors="replace")[-1500:] if log_path.is_file() else (completed.stderr or completed.stdout)[-1500:]
        return {"success": False, "error": f"UnrealInsights finished (exit {completed.returncode}) but did not write {', '.join(missing)}. "
                                           "The exporters need UE 5.1+; check the log tail.", "log_tail": tail, "export_dir": str(out_dir)}
    try:
        report = analyze_trace_exports(str(threads_csv), str(timers_csv), str(events_csv), top_n=int(args.get("top_n", 15)),
                                       slow_frames=int(args.get("slow_frames", 5)))
    except ValueError as exc:
        return {"success": False, "error": str(exc), "export_dir": str(out_dir)}
    return {"success": True, "trace": str(trace), "export_dir": str(out_dir), "insights": str(insights),
            "events_csv_megabytes": round(events_csv.stat().st_size / 1048576, 1), **report,
            "note": "Timer names are engine/game function names: locate game code with scan_project_cpp names=[...]. Exclusive time = time in the timer itself, "
                    "inclusive = including callees. The CSVs stay in export_dir for further queries."}


# ---- Packaging and packaged-build profiling ----

def _default_platform() -> str:
    return {"darwin": "Mac", "win32": "Win64"}.get(sys.platform, "Linux")


def _tail(path: Path, lines: int = 25) -> list[str]:
    try:
        return _read_text(path).splitlines()[-lines:]
    except OSError:
        return []


def _job_status(kind: str) -> dict:
    job = _JOBS.get(kind)
    if not job:
        return {"success": False, "error": f"No {kind} job has been started in this MCP session."}
    code = job["process"].poll()
    log = Path(job["log"])
    errors = [l for l in _read_text(log).splitlines() if re.search(r"\berror\b[: ]", l, re.IGNORECASE)][:10] if log.exists() else []
    return {"success": True, "state": "running" if code is None else ("finished" if code == 0 else "failed"),
            "exit_code": code, "log": str(log), "elapsed_seconds": round(time.time() - job["started"]),
            "log_tail": _tail(log), "errors": errors, "info": job.get("info", {})}


def _cancel_job(kind: str) -> dict:
    job = _JOBS.get(kind)
    if not job or job["process"].poll() is not None:
        return {"success": False, "error": f"No running {kind} job."}
    job["process"].terminate()
    return {"success": True, "state": "terminating"}


def _find_executables(root: Path) -> list[str]:
    found: list[str] = []
    for path in root.rglob("*"):
        if len(found) >= 10:
            break
        relative = path.relative_to(root)
        if len(relative.parts) > 4:
            continue
        if path.suffix == ".app" and path.is_dir():
            found.append(str(path))
        elif path.suffix == ".exe" and "Engine" not in relative.parts and "Binaries" in relative.parts:
            found.append(str(path))
    return found


def _package_project(args: dict) -> dict:
    action = args.get("action", "start")
    if action == "status":
        return _job_status("package")
    if action == "cancel":
        return _cancel_job("package")
    existing = _JOBS.get("package")
    if existing and existing["process"].poll() is None:
        return {"success": False, "error": "A packaging job is already running. Use action=status or action=cancel."}

    stats = _paths()
    engine_dir = Path(stats["engine_dir"])
    script = engine_dir / "Build" / "BatchFiles" / ("RunUAT.bat" if sys.platform == "win32" else "RunUAT.sh")
    if not script.exists():
        return {"success": False, "error": f"RunUAT not found at {script}"}
    configuration = args.get("configuration", "Development")
    if configuration not in {"Development", "Test", "DebugGame", "Shipping"}:
        return {"success": False, "error": "configuration must be Development (recommended for profiling), Test, DebugGame or Shipping"}
    platform = args.get("platform", _default_platform())
    if not re.fullmatch(r"[A-Za-z0-9]+", platform):
        return {"success": False, "error": "Invalid platform"}
    archive = Path(args.get("output_dir") or (Path(stats["saved_dir"]) / "MCP" / "Packaged")).expanduser()
    log_dir = Path(stats["saved_dir"]) / "MCP"
    log_dir.mkdir(parents=True, exist_ok=True)
    archive.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"package_{int(time.time())}.log"

    command = [str(script), "BuildCookRun", f"-project={stats['project_file']}", "-noP4", f"-platform={platform}",
               f"-clientconfig={configuration}", "-cook", "-stage", "-pak", "-archive", f"-archivedirectory={archive}",
               "-utf8output", "-unattended"]
    if args.get("build_code", True):
        command.append("-build")
    with open(log_path, "w") as log_file:
        process = subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT, start_new_session=True)
    _JOBS["package"] = {"process": process, "log": str(log_path), "started": time.time(),
                        "info": {"archive_dir": str(archive), "configuration": configuration, "platform": platform,
                                 "project_name": stats.get("project_name", "")}}
    return {"success": True, "state": "running", "log": str(log_path), "archive_dir": str(archive),
            "message": "Packaging started in the background (cook + build can take a long time). Poll package_project action=status. "
                       "Close other heavy work; the editor stays usable."}


def _resolve_executable(path: Path) -> Path | None:
    if path.suffix == ".app" and path.is_dir():
        macos = path / "Contents" / "MacOS"
        preferred = macos / path.stem
        if preferred.exists():
            return preferred
        candidates = [p for p in macos.iterdir() if p.is_file()] if macos.exists() else []
        return candidates[0] if candidates else None
    return path if path.is_file() else None


def _packaged_csv_roots(exe: Path, project_name: str) -> list[Path]:
    roots = [exe.parent.parent.parent if exe.parent.name == "MacOS" else exe.parent]
    home = Path.home()
    roots.append(home / "Library" / "Application Support" / "Epic" / project_name)
    roots.append(home / ".config" / "Epic" / project_name)
    roots.append(Path(os.environ.get("LOCALAPPDATA", str(home))) / project_name)
    return [r for r in roots if r.exists()]


def _find_new_csvs(roots: list[Path], since: float) -> list[Path]:
    found: list[Path] = []
    for root in roots:
        for p in root.rglob("Profiling/CSV/*.csv"):
            if p.stat().st_mtime >= since - 1.0:
                found.append(p)
    return sorted(found, key=lambda p: p.stat().st_mtime, reverse=True)


def _profile_packaged_build(args: dict) -> dict:
    action = args.get("action", "start")
    if action == "cancel":
        return _cancel_job("profile")
    if action == "status":
        status = _job_status("profile")
        if not status.get("success") or status["state"] == "running":
            return status
        job = _JOBS["profile"]
        csvs = _find_new_csvs(job["roots"], job["started"])
        if not csvs:
            status["note"] = "The game ended but no CSV was found. Check the log and the CSV roots searched."
            status["searched"] = [str(r) for r in job["roots"]]
            return status
        try:
            analysis = analyze_csv(_read_text(csvs[0]), skip_frames=job["skip_frames"])
        except ValueError as exc:
            return {**status, "success": False, "error": str(exc), "file": str(csvs[0])}
        return {**status, "file": str(csvs[0]), "build": "packaged", **analysis}

    existing = _JOBS.get("profile")
    if existing and existing["process"].poll() is None:
        return {"success": False, "error": "A packaged profile is already running. Use action=status or action=cancel."}

    exe_arg = args.get("executable")
    package = _JOBS.get("package", {}).get("info", {})
    if not exe_arg and package.get("archive_dir"):
        exes = _find_executables(Path(package["archive_dir"]))
        exe_arg = exes[0] if exes else None
    if not exe_arg:
        return {"success": False, "error": "Pass executable (.app on Mac, .exe on Windows) or run package_project first."}
    exe = _resolve_executable(Path(exe_arg).expanduser())
    if not exe:
        return {"success": False, "error": f"Executable not found: {exe_arg}"}

    frames = int(args.get("frames", 1800))
    if not 120 <= frames <= 36000:
        return {"success": False, "error": "frames must be between 120 and 36000"}
    command = [str(exe)]
    if args.get("map"):
        if not re.fullmatch(r"[A-Za-z0-9_/\.\-]+", str(args["map"])):
            return {"success": False, "error": "Invalid map name"}
        command.append(str(args["map"]))
    command += [f"-csvCaptureFrames={frames}", "-ExitAfterCsvProfiling", "-nosplash", "-windowed",
                f"-ResX={int(args.get('res_x', 1280))}", f"-ResY={int(args.get('res_y', 720))}"]
    if args.get("gpu_stats", True):
        command.append("-csvGpuStats")
    for extra in args.get("extra_args") or []:
        if not re.fullmatch(r"-[A-Za-z0-9_=,\.\-/]+", str(extra)):
            return {"success": False, "error": f"Invalid extra arg: {extra}"}
        command.append(str(extra))

    try:
        stats = _paths()
        log_dir = Path(stats["saved_dir"]) / "MCP"
    except RuntimeError:
        log_dir = Path.home()
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"packaged_profile_{int(time.time())}.log"
    project_name = package.get("project_name") or exe.name.split("-")[0].replace(".exe", "")
    with open(log_path, "w") as log_file:
        process = subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT, start_new_session=True)
    _JOBS["profile"] = {"process": process, "log": str(log_path), "started": time.time(),
                        "roots": _packaged_csv_roots(exe, project_name), "skip_frames": int(args.get("skip_frames", 60)),
                        "info": {"executable": str(exe), "frames": frames}}
    return {"success": True, "state": "running", "executable": str(exe), "log": str(log_path),
            "message": f"Launched the packaged game; it exits by itself after {frames} frames (-ExitAfterCsvProfiling). "
                       "Let the game run a representative scene (use `map` to open a specific level). Poll profile_packaged_build action=status."}


COMPOSITES: dict[str, Callable[[dict], Any]] = {
    "capture_performance": _capture_performance,
    "analyze_performance_csv": _analyze_performance_csv,
    "profile_gpu": _profile_gpu,
    "dump_cpu_stats": _dump_cpu_stats,
    "compare_performance": _compare_performance,
    "profile_physics": _profile_physics,
    "scan_project_cpp": _scan_project_cpp,
    "capture_viewmode": _capture_viewmode,
    "get_memory_report": _get_memory_report,
    "trace_control": _trace_control,
    "analyze_trace": _analyze_trace,
    "package_project": _package_project,
    "profile_packaged_build": _profile_packaged_build,
}


# =============================================================================
# Tool definitions
# =============================================================================

def _tool(name: str, description: str, properties: dict | None = None, required: list[str] | None = None) -> Tool:
    schema: dict[str, Any] = {"type": "object", "properties": properties or {}}
    if required:
        schema["required"] = required
    return Tool(name=name, description=description, inputSchema=schema)


def get_tools() -> list[Tool]:
    """Get all performance tools."""
    return [
        _tool(
            "get_performance_stats",
            "Snapshot of the current frame: fps, game / render / RHI / GPU thread times and which one is the bottleneck, memory "
            "(process + texture pool), RHI and feature level, key rendering cvars, project/saved/engine paths. Thread times can read 0 "
            "until the engine has measured them (run execute_console_command `stat unit` once); for stable numbers use capture_performance. "
            "Start here to see whether the game is CPU (game/render thread) or GPU bound.",
        ),
        _tool(
            "capture_performance",
            "Record the engine's CSV Profiler for N seconds and analyse it: avg / p50 / p95 / p99 / max of frame, game thread, render "
            "thread, RHI and GPU time, draw calls, the most expensive stats, the worst hitch frames (with the stat that spiked), the bottleneck "
            "(game / render / gpu / frame_cap) and concrete recommendations naming the next tools to run. Do the action you want to measure "
            "(start_pie, inject_input a busy scene) first; PIE numbers are more realistic than the editor viewport. Editor numbers are NOT "
            "packaged-build numbers. Works in the editor and Development builds, not Shipping.",
            {"seconds": {"type": "number", "description": "Capture length in seconds (default 10, max 120)"},
             "skip_frames": {"type": "integer", "description": "Warm-up frames to ignore at the start (default 30)"}},
        ),
        _tool(
            "analyze_performance_csv",
            "Analyse an existing CSV Profiler file (e.g. from a packaged build or a teammate): same output as capture_performance.",
            {"path": {"type": "string", "description": "Absolute path to the .csv"},
             "skip_frames": {"type": "integer", "description": "Warm-up frames to ignore (default 30)"}},
            ["path"],
        ),
        _tool(
            "profile_gpu",
            "Run one GPU profile (`profilegpu`) and parse the pass hierarchy from the log: total GPU ms and the most expensive passes "
            "(BasePass, ShadowDepths, Lights, PostProcessing, Translucency...). Use when capture_performance says the GPU is the bottleneck. "
            "The editor window must be visible. Falls back to raw log lines if the RHI gives no GPU timings.",
            {"wait_seconds": {"type": "number", "description": "How long to wait for the output (default 6)"},
             "include_raw": {"type": "boolean", "description": "Also return the raw log lines"}},
        ),
        _tool(
            "dump_cpu_stats",
            "Dump the CPU stat tree (`stat dumpframe|dumpave|dumphitches`) and return the raw lines: which game-thread functions, "
            "components, animation, physics or AI cost the most. Use when the game thread is the bottleneck.",
            {"mode": {"type": "string", "enum": ["average", "frame", "hitches"], "description": "average over N frames (default), one frame, or hitch frames"},
             "root": {"type": "string", "description": "Stat root, e.g. GameThread (default), Game, Anim, Physics"},
             "min_ms": {"type": "number", "description": "Hide entries below this many ms (default 0.2)"},
             "frames": {"type": "integer", "description": "Frames to average (default 120)"},
             "wait_seconds": {"type": "number", "description": "How long to wait before reading the log"},
             "max_lines": {"type": "integer", "description": "Max lines returned (default 150)"}},
        ),
        _tool(
            "compare_performance",
            "Compare two CSV Profiler captures (the `file` returned by capture_performance / profile_packaged_build): per-metric avg / p95 / p99 "
            "deltas, draw calls, hitches, which stats changed most and a verdict. Use it to prove an optimisation worked (before vs after) and to "
            "quantify editor overhead (label_before=editor, label_after=packaged). Only compare identical scenes and settings.",
            {"before": {"type": "string", "description": "CSV path of the first capture"},
             "after": {"type": "string", "description": "CSV path of the second capture"},
             "label_before": {"type": "string", "description": "Name for the first capture (default before)"},
             "label_after": {"type": "string", "description": "Name for the second capture (default after)"},
             "skip_frames": {"type": "integer", "description": "Warm-up frames ignored in both (default 30)"}},
            ["before", "after"],
        ),
        _tool(
            "profile_physics",
            "Physics cost: stat dumps of the Physics and Collision groups (use during PIE with the physics-heavy scene running) plus the project's "
            "PhysicsSettings and a checklist of the usual causes (simulating bodies, complex collision, hit/overlap events, traces, substepping). "
            "Fixes are done with optimize_static_mesh (collision), set_project_config and Blueprint/level edits.",
            {"roots": {"type": "array", "items": {"type": "string"}, "description": "Stat roots (default Physics, Collision)"},
             "frames": {"type": "integer"}, "min_ms": {"type": "number"}, "max_lines": {"type": "integer"}},
        ),
        _tool(
            "scan_project_cpp",
            "Point a CPU bottleneck at the project's own C++ (Source/ and Plugins/*/Source/): scans for costly patterns (per-frame actor/object "
            "scans, sync loads, traces, spawns, string building, enabled ticks) and reports the enclosing function, flagging Tick-like ones first; "
            "and/or finds names (function/class/stat names from dump_cpu_stats or capture_performance) in the code. It reads code; it does not "
            "profile or edit it. Blueprint-only projects return a note instead.",
            {"names": {"type": "array", "items": {"type": "string"}, "description": "Symbols to locate, e.g. [\"AMyEnemy::Tick\", \"STAT_MyAI\"]"},
             "patterns": {"type": "boolean", "description": "Run the pattern scan (default true when no names are given)"},
             "source_dir": {"type": "string", "description": "Scan this folder instead of the project's Source and plugin sources"},
             "max_hits": {"type": "integer", "description": "Hits listed per pattern (default 15)"}},
        ),
        _tool(
            "capture_viewmode",
            "In the running PIE game, switch to optimisation view modes and return a screenshot of each: shadercomplexity (expensive "
            "materials), quadoverdraw, lightcomplexity, lodcoloration, lightmapdensity, wireframe. Resets to lit afterwards. Requires start_pie.",
            {"modes": {"type": "array", "items": {"type": "string"}, "description": "View modes (default [\"shadercomplexity\"])"},
             "wait_seconds": {"type": "number", "description": "Settle time before each screenshot (default 1.5)"},
             "max_width": {"type": "integer", "description": "Screenshot width (default 960)"}},
        ),
        _tool(
            "get_memory_report",
            "Run `memreport -full` and return the report sections (largest textures, meshes, objects, pools, streaming). Use to find which "
            "assets use the most memory; then fix with optimize_texture / optimize_static_mesh.",
            {"sections": {"type": "array", "items": {"type": "string"}, "description": "Only sections whose command contains one of these words (e.g. [\"texture\", \"staticmesh\"])"},
             "lines_per_section": {"type": "integer", "description": "Lines kept per section (default 20)"}},
        ),
        _tool(
            "trace_control",
            "Start / stop / query an Unreal Trace (`Trace.Start|Stop|Status`) and report the newest .utrace file. Analyse it afterwards with "
            "analyze_trace (UE 5.1+ with UnrealInsights); on 4.27 prefer capture_performance.",
            {"action": {"type": "string", "enum": ["start", "stop", "status"], "description": "Default status"},
             "channels": {"type": "string", "description": "Comma list (default cpu,gpu,frame,bookmark)"}},
        ),
        _tool(
            "analyze_trace",
            "Turn an Unreal Insights .utrace into an AI-readable report: runs UnrealInsights headless (-NoUI -AutoQuit with "
            "TimingInsights.ExportThreads / ExportTimers / ExportTimingEvents), then aggregates the CSVs into per-thread busy time, the timers with "
            "the largest exclusive and inclusive time on the game / render / RHI / GPU threads, and the slowest frames with what the game thread "
            "spent them on. Requires UnrealInsights from UE 5.1 or newer (the exporters do not exist in 4.27) and a trace with the cpu and frame "
            "channels (trace_control action=start). Default: newest .utrace under Saved.",
            {"trace": {"type": "string", "description": "Path to a .utrace (default: newest under the project's Saved)"},
             "insights_path": {"type": "string", "description": "UnrealInsights executable (default: <Engine>/Binaries, or env UEBLUEPRINTMCP_INSIGHTS)"},
             "threads": {"type": "string", "description": "Thread filter with wildcards, e.g. \"GameThread,RenderThread*,RHIThread,GPU*\" (default all; narrowing shrinks the export a lot)"},
             "start_time": {"type": "number", "description": "Only events from this trace time (seconds)"},
             "end_time": {"type": "number", "description": "Only events until this trace time (seconds)"},
             "top_n": {"type": "integer", "description": "Timers listed per thread group (default 15)"},
             "slow_frames": {"type": "integer", "description": "Slowest frames detailed (default 5)"},
             "timeout_seconds": {"type": "number", "description": "Insights run limit (default 600)"}},
        ),
        _tool(
            "audit_assets",
            "Cost audit of project assets, worst first, with issue lists: textures (size, streaming, mips, compression, wrong group), static "
            "meshes (triangles, LODs, collision, lightmap res), skeletal meshes, materials (instruction counts, blend mode, usage flags), "
            "Blueprints (tick, graph size). Use after capture_performance / get_memory_report to find what to fix.",
            {"asset_type": {"type": "string", "enum": ["texture", "static_mesh", "skeletal_mesh", "material", "blueprint"]},
             "path": {"type": "string", "description": "Content folder, default /Game"},
             "name_contains": {"type": "string", "description": "Only assets whose name contains this"},
             "limit": {"type": "integer", "description": "Max entries returned (default 30)"},
             "max_scan": {"type": "integer", "description": "Max assets inspected (default 400, max 3000)"}},
            ["asset_type"],
        ),
        _tool(
            "audit_level",
            "Audit the open level (or the PIE world with world=pie): actor/component counts, ticking actors, meshes that should be instanced, "
            "movable meshes, lights and shadows, volumes, navigation, streaming levels, HLOD / world settings, with an issues list.",
            {"world": editor_tools.WORLD_PROPERTY,
             "instancing_threshold": {"type": "integer", "description": "Report meshes placed at least this many times as instancing candidates (default 10)"}},
        ),
        _tool(
            "optimize_static_mesh",
            "Optimise Static Mesh assets (UE 4.27): generate LODs by triangle percentage, set LOD group, replace collision (box/sphere/capsule/"
            "kdop/convex/none) and collision complexity, lightmap resolution/UVs, distance field, min LOD. Returns before/after triangles per LOD. "
            "Max 25 meshes per call. Blocked while PIE runs. Re-run audit_assets static_mesh afterwards.",
            {"mesh": {"type": "string", "description": "Mesh name or /Game path"},
             "meshes": {"type": "array", "items": {"type": "string"}, "description": "Several meshes (same settings)"},
             "lods": {"type": "array", "items": {"type": "object", "properties": {"percent_triangles": {"type": "number"}, "screen_size": {"type": "number"}}},
                      "description": "LOD1..N reduction, e.g. [{\"percent_triangles\":0.5},{\"percent_triangles\":0.25}]"},
             "auto_compute_screen_size": {"type": "boolean"},
             "remove_lods": {"type": "boolean"},
             "lod_group": {"type": "string", "description": "LOD group name (LevelArchitecture, SmallProp, LargeProp, Deco, Vehicle...) or None"},
             "collision": {"type": "string", "enum": ["none", "box", "sphere", "capsule", "kdop10x", "kdop10y", "kdop10z", "kdop18", "kdop26", "convex"]},
             "hull_count": {"type": "integer"}, "max_hull_verts": {"type": "integer"}, "hull_precision": {"type": "integer"},
             "collision_complexity": {"type": "string", "enum": ["project_default", "simple_and_complex", "use_simple_as_complex", "use_complex_as_simple"]},
             "lightmap_resolution": {"type": "integer"}, "generate_lightmap_uvs": {"type": "boolean"},
             "allow_cpu_access": {"type": "boolean"}, "distance_field": {"type": "boolean"}, "min_lod": {"type": "integer"}},
        ),
        _tool(
            "optimize_texture",
            "Optimise Texture2D assets: max size cap, LOD group, compression, mip generation, never-stream, sRGB, LOD bias. Returns the size before "
            "and an estimate after. Max 25 per call. Blocked while PIE runs. Typical wins: cap 4K textures on small props to 1024/2048, use "
            "TC_Normalmap / TC_Masks for data textures, TMGS_NoMipmaps for UI.",
            {"texture": {"type": "string"}, "textures": {"type": "array", "items": {"type": "string"}},
             "max_texture_size": {"type": "integer", "description": "Power of two, 0 = no cap"},
             "lod_group": {"type": "string", "description": "TextureGroup (World, WorldNormalMap, Character, UI, Effects...)"},
             "compression": {"type": "string", "description": "TC_Default, TC_Normalmap, TC_Masks, TC_Grayscale, TC_Alpha, TC_BC7..."},
             "mip_gen": {"type": "string", "description": "TMGS_FromTextureGroup, TMGS_NoMipmaps, TMGS_Sharpen0..."},
             "never_stream": {"type": "boolean"}, "srgb": {"type": "boolean"}, "lod_bias": {"type": "integer"}},
        ),
        _tool(
            "set_project_config",
            "Persistently write project settings to Config/Default<File>.ini (survives restarts, used by packaged builds): rendering and "
            "scalability cvars, physics, streaming, etc. Keeps comments, writes a .mcpbak backup, supports set / add (+Key=) / remove. "
            "apply_live also sets the matching console variable in the running editor. Example: file=Engine, section=/Script/Engine.RendererSettings, "
            "values={\"r.Shadow.MaxResolution\": 1024}. Read first with get_project_config.",
            {"file": {"type": "string", "enum": ["Engine", "Game", "Input", "Editor", "Scalability", "GameUserSettings", "EditorPerProjectUserSettings"]},
             "section": {"type": "string", "description": "e.g. /Script/Engine.RendererSettings or SystemSettings"},
             "key": {"type": "string"}, "value": {"type": "string"},
             "values": {"type": "object", "description": "key -> value pairs"},
             "operation": {"type": "string", "enum": ["set", "add", "remove"], "description": "Default set"},
             "apply_live": {"type": "boolean", "description": "Also set the console variable now (default false)"}},
            ["file", "section"],
        ),
        _tool(
            "manage_level_streaming",
            "List / add / remove / edit streaming sub-levels of the open level (split big maps so parts load on demand). "
            "add: level=/Game/Maps/Sub, streaming_class=always_loaded|blueprint. set: should_be_loaded / should_be_visible. Edits are blocked while PIE runs.",
            {"action": {"type": "string", "enum": ["list", "add", "remove", "set"]},
             "level": {"type": "string", "description": "Level package (/Game/Maps/Sub) or short name"},
             "streaming_class": {"type": "string", "enum": ["always_loaded", "blueprint"]},
             "should_be_loaded": {"type": "boolean"}, "should_be_visible": {"type": "boolean"}},
            ["action"],
        ),
        _tool(
            "build_level",
            "Build the open level: lighting (preview/medium/high/production, runs in the background: poll with what=status), navmesh, hlod "
            "(hierarchical LOD proxies), geometry, or status. Saves dirty packages first. Not allowed while PIE runs.",
            {"what": {"type": "string", "enum": ["lighting", "navmesh", "hlod", "geometry", "status"]},
             "quality": {"type": "string", "enum": ["preview", "medium", "high", "production"]}},
            ["what"],
        ),
        _tool(
            "package_project",
            "Package (cook + build + pak) the project with RunUAT BuildCookRun in the background so it can be profiled as a real game. "
            "Default configuration Development keeps the profilers. action=start | status | cancel. Takes a long time; poll status.",
            {"action": {"type": "string", "enum": ["start", "status", "cancel"]},
             "configuration": {"type": "string", "enum": ["Development", "Test", "DebugGame", "Shipping"], "description": "Default Development"},
             "platform": {"type": "string", "description": "Default: Mac on macOS, Win64 on Windows"},
             "output_dir": {"type": "string", "description": "Archive directory (default <Saved>/MCP/Packaged)"},
             "build_code": {"type": "boolean", "description": "Compile C++ (-build), default true"}},
        ),
        _tool(
            "profile_packaged_build",
            "Run the packaged game with the CSV Profiler (-csvCaptureFrames, exits when done) and analyse the capture: real, editor-free "
            "performance numbers. action=start | status | cancel. With status it returns the analysis once the game exited. "
            "The scene measured is whatever the game does at startup (pass `map`); it does not play the game for you.",
            {"action": {"type": "string", "enum": ["start", "status", "cancel"]},
             "executable": {"type": "string", "description": ".app (Mac) or .exe path; default: found in the last package_project archive"},
             "map": {"type": "string", "description": "Map to open, e.g. /Game/Maps/TestLevel"},
             "frames": {"type": "integer", "description": "Frames to capture (default 1800, 120-36000)"},
             "skip_frames": {"type": "integer", "description": "Warm-up frames ignored in the analysis (default 60)"},
             "res_x": {"type": "integer"}, "res_y": {"type": "integer"},
             "gpu_stats": {"type": "boolean", "description": "Add -csvGpuStats (default true)"},
             "extra_args": {"type": "array", "items": {"type": "string"}, "description": "Extra command line args such as -dx12"}},
        ),
    ]


TOOL_HANDLERS: dict[str, str] = {
    "get_performance_stats": "get_performance_stats",
    "audit_assets": "audit_assets",
    "audit_level": "audit_level",
    "optimize_static_mesh": "optimize_static_mesh",
    "optimize_texture": "optimize_texture",
    "set_project_config": "set_project_config",
    "manage_level_streaming": "manage_level_streaming",
    "build_level": "build_level",
    **{name: name for name in COMPOSITES},
}


async def handle_tool(name: str, arguments: dict[str, Any]) -> list[TextContent | ImageContent]:
    """Handle a performance tool call."""
    command_type = TOOL_HANDLERS.get(name)
    if not command_type:
        return [TextContent(type="text", text=f'{{"success": false, "error": "Unknown tool: {name}"}}')]
    args = arguments or {}

    composite = COMPOSITES.get(name)
    if composite:
        try:
            result = await asyncio.to_thread(composite, args)
        except Exception as exc:  # surface any failure as a tool error instead of killing the MCP server
            return _text({"success": False, "error": f"{type(exc).__name__}: {exc}"})
        return result if isinstance(result, list) else _text(result)

    result = await asyncio.to_thread(_send, command_type, args if args else None)
    return _text(result)
