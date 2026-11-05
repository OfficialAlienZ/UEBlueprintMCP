"""
Pure analysis helpers for the performance tools (no Unreal connection needed, unit-testable).

- analyze_csv(): parse an Unreal CSV Profiler capture (`csvprofile`) into statistics, hitches and a
  bottleneck classification with plain-language recommendations.
- parse_gpu_profile(): parse the `profilegpu` hierarchy that Unreal writes to the Output Log.
"""

import csv
import io
import re
from typing import Any

MAIN_COLUMNS = {
    "frame_ms": "FrameTime",
    "game_thread_ms": "GameThreadTime",
    "render_thread_ms": "RenderThreadTime",
    "rhi_thread_ms": "RHIThreadTime",
    "gpu_ms": "GPUTime",
}

# Columns that are timings of the frame itself, not "what the frame spent time on".
_SKIP_FOR_TOP = set(MAIN_COLUMNS.values()) | {
    "EVENTS", "TOTAL FRAME", "Frame", "FrameTime", "DrawCalls", "Timestamp", "RHIT_Time", "RenderThreadTime_CriticalPath",
    "GameThreadTime_CriticalPath",
}


def _percentile(sorted_values: list[float], fraction: float) -> float:
    if not sorted_values:
        return 0.0
    index = min(len(sorted_values) - 1, max(0, int(round(fraction * (len(sorted_values) - 1)))))
    return sorted_values[index]


def column_stats(values: list[float]) -> dict[str, float]:
    """avg / p50 / p95 / p99 / max of a list of numbers (rounded to 3 decimals)."""
    if not values:
        return {"avg": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0}
    ordered = sorted(values)
    return {
        "avg": round(sum(values) / len(values), 3),
        "p50": round(_percentile(ordered, 0.50), 3),
        "p95": round(_percentile(ordered, 0.95), 3),
        "p99": round(_percentile(ordered, 0.99), 3),
        "max": round(ordered[-1], 3),
    }


def _to_float(cell: str) -> float | None:
    try:
        return float(cell)
    except (TypeError, ValueError):
        return None


def parse_csv_text(text: str) -> tuple[dict[str, list[float]], list[dict[str, Any]]]:
    """
    Parse a CSV Profiler file. Returns (columns, events).

    Tolerant of format differences: the header is the first row containing a FrameTime cell, data rows are the
    rows after it whose FrameTime cell is numeric, and everything from the first metadata row ("[Key]" cells)
    on is ignored. Column names are kept as-is (e.g. "Exclusive/GameThread/Slate").
    """
    reader = csv.reader(io.StringIO(text))
    header: list[str] | None = None
    columns: dict[str, list[float]] = {}
    events: list[dict[str, Any]] = []
    frame_col = -1
    event_col = -1
    frame_index = 0

    for row in reader:
        if not row:
            continue
        if header is None:
            if any(cell.strip() == "FrameTime" for cell in row):
                header = [cell.strip() for cell in row]
                frame_col = header.index("FrameTime")
                event_col = header.index("EVENTS") if "EVENTS" in header else -1
                columns = {name: [] for name in header if name and name != "EVENTS"}
            continue
        if row[0].strip().startswith("["):
            break  # metadata block
        if frame_col >= len(row) or _to_float(row[frame_col]) is None:
            continue
        for index, name in enumerate(header):
            if not name or name == "EVENTS" or index >= len(row):
                continue
            value = _to_float(row[index])
            columns[name].append(0.0 if value is None else value)
        if event_col != -1 and event_col < len(row) and row[event_col].strip():
            events.append({"frame": frame_index, "event": row[event_col].strip()})
        frame_index += 1

    if header is None:
        raise ValueError("Not a CSV Profiler capture: no 'FrameTime' column found.")
    return columns, events


def classify_bottleneck(frame: dict[str, float], game: dict[str, float], render: dict[str, float], gpu: dict[str, float]) -> tuple[str, str]:
    """
    Returns (bottleneck, explanation) from the average timings (ms).
    The thread (or the GPU) that takes the largest share of the frame time is the limit; when none of them
    comes close to the frame time the frame is waiting (vsync / frame cap / editor throttle).
    """
    frame_ms = frame.get("avg", 0.0)
    candidates = {"game": game.get("avg", 0.0), "render": render.get("avg", 0.0), "gpu": gpu.get("avg", 0.0)}
    if frame_ms <= 0:
        return "unknown", "No frame time data."
    top_name, top_ms = max(candidates.items(), key=lambda item: item[1])
    if top_ms < frame_ms * 0.6:
        return "frame_cap", (
            f"No thread uses more than 60% of the {frame_ms:.1f} ms frame (best: {top_name} {top_ms:.1f} ms). "
            "The game is waiting: vsync, a frame-rate cap (t.MaxFPS), editor throttling of a background window, "
            "or a stall (loading, sync). Measure with the window focused, vsync off (r.VSync 0) and t.MaxFPS 0."
        )
    texts = {
        "game": "The game thread (Blueprint/C++ logic, ticking actors, animation, physics, AI) is the limit.",
        "render": "The render thread (draw call submission, visibility, too many primitives/materials) is the limit.",
        "gpu": "The GPU (shading, shadows, resolution, post processing, overdraw) is the limit.",
    }
    return top_name, f"{texts[top_name]} {top_name} = {top_ms:.1f} ms of a {frame_ms:.1f} ms frame."


RECOMMENDATIONS = {
    "game": [
        "Run audit_level: look at ticking actors and movable meshes; disable Tick on actors that do not need it.",
        "Run audit_assets asset_type=blueprint: Blueprints with Tick enabled and large event graphs are the usual cost.",
        "Use dump_cpu_stats (stat game / stat anim) to see which system is expensive, and capture again after each fix.",
        "Replace per-frame polling with events/timers; reduce AI/perception update rates; lower animation update rates for distant skeletal meshes.",
    ],
    "render": [
        "Run audit_level: many separate static mesh actors with the same mesh should become instanced (ISM/HISM).",
        "Run audit_assets asset_type=material and static_mesh: fewer sections and fewer materials per mesh reduce draw calls.",
        "Add LODs and enable HLOD/merging for far-away geometry (optimize_static_mesh, build_level what=hlod).",
        "Lower r.ViewDistanceScale / use cull distance volumes; check the DrawCalls column in the CSV.",
    ],
    "gpu": [
        "Run profile_gpu to see which pass (BasePass, ShadowDepths, PostProcessing, Lights) costs the most.",
        "Run capture_viewmode shadercomplexity / quadoverdraw (in PIE) to locate expensive materials and overdraw.",
        "Run audit_assets asset_type=material (instruction counts) and texture (oversized textures).",
        "Lower shadow cost: fewer movable shadow-casting lights, r.Shadow.MaxResolution, shorter cull distances; use scalability presets (set_project_config file=Scalability).",
        "Reduce resolution scale (r.ScreenPercentage) or post processing (SSR, AO, bloom, motion blur) if GPU bound at high resolution.",
    ],
    "frame_cap": [
        "The game is not limited by CPU or GPU in this capture. Re-measure with vsync off and t.MaxFPS 0 before optimising.",
    ],
}


def analyze_csv(text: str, skip_frames: int = 30, top_n: int = 12) -> dict[str, Any]:
    """Statistics, top costs, hitches, bottleneck and recommendations for a CSV Profiler capture."""
    columns, events = parse_csv_text(text)
    total_frames = len(columns.get("FrameTime", []))
    skip = max(0, min(skip_frames, max(total_frames - 10, 0)))
    trimmed = {name: values[skip:] for name, values in columns.items()}
    frame_count = len(trimmed.get("FrameTime", []))
    if frame_count == 0:
        raise ValueError("The capture contains no frames.")

    main = {key: column_stats(trimmed[col]) for key, col in MAIN_COLUMNS.items() if col in trimmed and any(trimmed[col])}
    frame = main.get("frame_ms", column_stats(trimmed["FrameTime"]))
    bottleneck, explanation = classify_bottleneck(
        frame, main.get("game_thread_ms", {}), main.get("render_thread_ms", {}), main.get("gpu_ms", {})
    )

    result: dict[str, Any] = {
        "frames_total": total_frames,
        "frames_analyzed": frame_count,
        "frames_skipped_warmup": skip,
        "avg_fps": round(1000.0 / frame["avg"], 1) if frame.get("avg") else 0.0,
        "fps_1_percent_low": round(1000.0 / frame["p99"], 1) if frame.get("p99") else 0.0,
        "timings_ms": main,
        "bottleneck": bottleneck,
        "bottleneck_explanation": explanation,
        "recommendations": RECOMMENDATIONS.get(bottleneck, []),
    }

    if "DrawCalls" in trimmed and any(trimmed["DrawCalls"]):
        result["draw_calls"] = column_stats(trimmed["DrawCalls"])

    stat_columns = [
        (name, column_stats(values))
        for name, values in trimmed.items()
        if name not in _SKIP_FOR_TOP and any(values)
    ]
    stat_columns.sort(key=lambda item: item[1]["avg"], reverse=True)
    result["top_stats_by_average"] = [{"stat": name, **stats} for name, stats in stat_columns[:top_n]]
    by_peak = sorted(stat_columns, key=lambda item: item[1]["p99"], reverse=True)
    result["top_stats_by_p99"] = [{"stat": name, **stats} for name, stats in by_peak[:top_n]]

    # Hitches: frames far above the typical frame time.
    median = frame["p50"] or frame["avg"]
    threshold = max(median * 2.5, 33.3)
    frame_values = trimmed["FrameTime"]
    hitches = []
    for index, value in enumerate(frame_values):
        if value >= threshold:
            hitch = {"frame": index + skip, "frame_ms": round(value, 2)}
            for key, col in MAIN_COLUMNS.items():
                if key != "frame_ms" and col in trimmed and index < len(trimmed[col]):
                    hitch[key] = round(trimmed[col][index], 2)
            worst = max(
                ((name, values[index]) for name, values in trimmed.items() if name not in _SKIP_FOR_TOP and index < len(values)),
                key=lambda item: item[1],
                default=None,
            )
            if worst and worst[1] > 0:
                hitch["worst_stat"] = {"stat": worst[0], "ms": round(worst[1], 2)}
            hitches.append(hitch)
    hitches.sort(key=lambda h: h["frame_ms"], reverse=True)
    result["hitch_threshold_ms"] = round(threshold, 1)
    result["hitch_count"] = len(hitches)
    result["worst_hitches"] = hitches[:10]
    if events:
        result["events"] = events[:20]
    return result


_GPU_LINE = re.compile(r"^(\s*)(\d+(?:\.\d+)?)%\s+(\d+(?:\.\d+)?)ms\s+(.*?)\s*$")


def parse_gpu_profile(messages: list[str], top_n: int = 25) -> dict[str, Any]:
    """
    Parse `profilegpu` output lines ("  45.2%  3.42ms   BasePass") into a flat list with depth.
    Returns {} when no hierarchy line is found (the platform may not support GPU timestamps).
    """
    entries: list[dict[str, Any]] = []
    for message in messages:
        match = _GPU_LINE.match(message)
        if not match:
            continue
        indent, percent, ms, name = match.groups()
        entries.append({
            "name": name,
            "percent": float(percent),
            "ms": float(ms),
            "depth": len(indent.replace("\t", "  ")) // 2,
        })
    if not entries:
        return {}
    total = max(entries, key=lambda e: e["ms"])
    non_total = [e for e in entries if e is not total]
    top_level = [e for e in non_total if e["depth"] <= min((x["depth"] for x in non_total), default=0) + 1]
    return {
        "total_gpu_ms": total["ms"],
        "top_level_passes": sorted(top_level, key=lambda e: e["ms"], reverse=True)[:top_n],
        "most_expensive_entries": sorted(non_total, key=lambda e: e["ms"], reverse=True)[:top_n],
        "entry_count": len(entries),
    }


def compare_analyses(before: dict[str, Any], after: dict[str, Any], label_before: str = "before", label_after: str = "after") -> dict[str, Any]:
    """
    Compare two analyze_csv() results (before/after an optimisation, or editor vs packaged build).
    Negative delta = the second capture is faster.
    """
    def delta(a: float, b: float) -> dict[str, float]:
        return {label_before: round(a, 2), label_after: round(b, 2), "delta_ms": round(b - a, 2),
                "delta_percent": round((b - a) / a * 100.0, 1) if a else 0.0}

    timings: dict[str, Any] = {}
    for key in sorted(set(before.get("timings_ms", {})) & set(after.get("timings_ms", {}))):
        a, b = before["timings_ms"][key], after["timings_ms"][key]
        timings[key] = {"avg": delta(a["avg"], b["avg"]), "p95": delta(a["p95"], b["p95"]), "p99": delta(a["p99"], b["p99"])}

    result: dict[str, Any] = {
        "timings_ms": timings,
        "avg_fps": delta(before.get("avg_fps", 0.0), after.get("avg_fps", 0.0)),
        "hitch_count": {label_before: before.get("hitch_count", 0), label_after: after.get("hitch_count", 0)},
        "bottleneck": {label_before: before.get("bottleneck"), label_after: after.get("bottleneck")},
    }
    if "draw_calls" in before and "draw_calls" in after:
        result["draw_calls_avg"] = delta(before["draw_calls"]["avg"], after["draw_calls"]["avg"])

    # Per-stat changes, largest absolute change in average cost first.
    old = {s["stat"]: s["avg"] for s in before.get("top_stats_by_average", [])}
    new = {s["stat"]: s["avg"] for s in after.get("top_stats_by_average", [])}
    changes = [{"stat": name, label_before: round(old.get(name, 0.0), 3), label_after: round(new.get(name, 0.0), 3),
                "delta_ms": round(new.get(name, 0.0) - old.get(name, 0.0), 3)} for name in set(old) | set(new)]
    changes.sort(key=lambda c: abs(c["delta_ms"]), reverse=True)
    result["stat_changes"] = changes[:12]

    frame = timings.get("frame_ms", {}).get("avg")
    if frame:
        pct = frame["delta_percent"]
        if abs(pct) < 3.0:
            verdict = "No meaningful frame time change (within 3%, which is about the run-to-run noise)."
        elif pct < 0:
            verdict = f"Faster: average frame time dropped {abs(pct):.1f}%."
        else:
            verdict = f"Slower: average frame time rose {pct:.1f}%."
        result["verdict"] = verdict
    return result


# --- C++ source scanning (CPU hotspots in project code) ----------------------

# (label, regex, why it matters)
CPP_PATTERNS: list[tuple[str, str, str]] = [
    ("tick_enabled", r"bCanEverTick\s*=\s*true|SetActorTickEnabled\s*\(\s*true|SetComponentTickEnabled\s*\(\s*true",
     "Actor/component ticks every frame; use a TickInterval, timers or events if it does not need to."),
    ("actor_scan", r"\bTActorIterator\b|GetAllActorsOfClass|GetAllActorsWithTag|GetAllActorsWithInterface",
     "Scans every actor in the world; cache the result or use a registry instead of scanning per frame."),
    ("object_scan", r"\bTObjectIterator\b|\bGetObjectsOfClass\b",
     "Walks every UObject; very expensive outside of one-off tools."),
    ("sync_load", r"\bStaticLoadObject\b|\bLoadObject\s*<|\bLoadSynchronous\b|\bStaticLoadClass\b|\bLoadClass\s*<|FlushAsyncLoading",
     "Synchronous asset load: causes hitches. Use FStreamableManager async loads or preload."),
    ("component_lookup", r"\bFindComponentByClass\b|\bGetComponentByClass\b|\bGetComponents\s*<",
     "Component search; cache the pointer in BeginPlay."),
    ("trace", r"\b(LineTrace|SweepMulti|SweepSingle|OverlapMulti|OverlapAny)\w*\s*\(",
     "Physics query; several per frame per actor add up (check query count, use channels/filters)."),
    ("spawn_destroy", r"\bSpawnActor\b|\bDestroyActor\b|\bNewObject\s*<",
     "Spawning/destroying in hot code causes allocation + GC spikes; pool actors/objects."),
    ("string_build", r"\bFString::Printf\b|\bTEXT\s*\(.*\)\s*\+|FName\s*\(\s*\*FString",
     "String/FName construction allocates; avoid it in per-frame code."),
]

_FUNCTION_LINE = re.compile(r"^[A-Za-z_][\w:<>\*&\s,]*?\b(\w+)::(~?\w+)\s*\(")
_TICKISH = re.compile(r"^(Tick|TickComponent|TickActor|Update\w*|OnTick\w*|Process\w*)$")


def scan_cpp_source(files: list[tuple[str, str]], max_hits_per_pattern: int = 25) -> dict[str, Any]:
    """
    Heuristic scan of C++ text for performance-sensitive patterns. `files` = (relative path, content).
    Each hit reports the enclosing function (nearest preceding `Class::Function(` line at column 0, so only
    meaningful for .cpp files) and whether that function looks like per-frame code. Hits inside Tick-like
    functions are listed first. This points the AI at code to read; it does not prove a hotspot.
    """
    compiled = [(label, re.compile(rx), why) for label, rx, why in CPP_PATTERNS]
    hits: dict[str, list[dict[str, Any]]] = {label: [] for label, _, _ in compiled}
    totals: dict[str, int] = {label: 0 for label, _, _ in compiled}
    for path, content in files:
        lines = content.splitlines()
        current_function = ""
        for number, line in enumerate(lines, 1):
            match = _FUNCTION_LINE.match(line)
            if match and not line.rstrip().endswith(";"):
                current_function = f"{match.group(1)}::{match.group(2)}"
            stripped = line.strip()
            if not stripped or stripped.startswith("//"):
                continue
            for label, rx, _ in compiled:
                if rx.search(line):
                    totals[label] += 1
                    function_name = current_function.split("::")[-1] if current_function else ""
                    hits[label].append({
                        "file": path, "line": number, "function": current_function or None,
                        "per_frame": bool(function_name and _TICKISH.match(function_name)),
                        "code": stripped[:160],
                    })
    patterns = []
    for label, _, why in compiled:
        ordered = sorted(hits[label], key=lambda h: not h["per_frame"])
        patterns.append({
            "pattern": label, "why": why, "total_hits": totals[label],
            "in_tick_like_functions": sum(1 for h in hits[label] if h["per_frame"]),
            "hits": ordered[:max_hits_per_pattern],
        })
    patterns.sort(key=lambda p: (p["in_tick_like_functions"], p["total_hits"]), reverse=True)
    return {"files_scanned": len(files), "patterns": [p for p in patterns if p["total_hits"]]}


# --- Unreal Insights (.utrace) CSV exports (UE 5.1+ UnrealInsights command line) ---------------------------

_FRAME_TIMER = re.compile(r"^(FEngineLoop::Tick|FEngineLoop_Tick|EngineLoop::Tick|Frame)$")


def _read_csv_rows(path: str) -> "tuple[list[str], Any]":
    handle = open(path, newline="", encoding="utf-8", errors="replace")
    reader = csv.reader(handle)
    header = next(reader, [])
    return [h.strip() for h in header], _closing_rows(handle, reader)


def _closing_rows(handle: Any, reader: Any):
    try:
        yield from reader
    finally:
        handle.close()


def _thread_group(name: str) -> str:
    lowered = name.lower()
    if lowered.startswith("gamethread"):
        return "game"
    if lowered.startswith("renderthread"):
        return "render"
    if lowered.startswith("rhithread"):
        return "rhi"
    if lowered.startswith("gpu"):
        return "gpu"
    return "other"


def analyze_trace_exports(threads_csv: str, timers_csv: str, events_csv: str, top_n: int = 15, slow_frames: int = 5,
                          max_events: int = 20_000_000) -> dict[str, Any]:
    """
    Aggregate the CSV files that UnrealInsights writes (TimingInsights.ExportThreads / ExportTimers / ExportTimingEvents)
    into something small enough for an LLM: per thread group the timers with the largest total (inclusive and exclusive)
    time, per-thread busy time, and the slowest frames with what the game thread spent them on.
    Streams the events file; exclusive time comes from a per-thread stack (children precede nothing: events are expected
    in start-time order per thread, as the exporter writes them).
    """
    thread_names: dict[str, str] = {}
    header, rows = _read_csv_rows(threads_csv)
    id_col, name_col = header.index("Id"), header.index("Name")
    for row in rows:
        if len(row) > max(id_col, name_col):
            thread_names[row[id_col].strip()] = row[name_col].strip()

    timer_names: dict[str, str] = {}
    header, rows = _read_csv_rows(timers_csv)
    id_col, name_col = header.index("Id"), header.index("Name")
    for row in rows:
        if len(row) > max(id_col, name_col):
            timer_names[row[id_col].strip()] = row[name_col].strip()

    header, rows = _read_csv_rows(events_csv)
    try:
        c_thread, c_timer = header.index("ThreadId"), header.index("TimerId")
        c_start, c_end, c_depth = header.index("StartTime"), header.index("EndTime"), header.index("Depth")
    except ValueError as exc:
        raise ValueError(f"The timing events CSV lacks a required column ({exc}). Expected ThreadId,TimerId,StartTime,EndTime,Depth.") from exc
    needed = max(c_thread, c_timer, c_start, c_end, c_depth)

    # group -> timer -> [count, inclusive, exclusive, max]
    agg: dict[str, dict[str, list[float]]] = {}
    busy: dict[str, float] = {}
    stacks: dict[str, list[list[Any]]] = {}  # thread -> [[depth, end, timer, duration, child_total]]
    frames: list[dict[str, Any]] = []
    frame_acc: dict[str, float] = {}
    events = 0
    truncated = False
    first_start, last_end = None, 0.0

    def finish(thread: str, group: str, entry: list[Any], parent: list[Any] | None) -> None:
        depth, _end, timer, duration, child_total = entry
        excl = max(duration - child_total, 0.0)
        name = timer_names.get(timer, f"timer#{timer}")
        stat = agg.setdefault(group, {}).setdefault(name, [0, 0.0, 0.0, 0.0])
        stat[0] += 1
        stat[1] += duration
        stat[2] += excl
        stat[3] = max(stat[3], duration)
        if parent is not None:
            parent[4] += duration
        elif depth == 0:
            busy[thread] = busy.get(thread, 0.0) + duration
        if group == "game":
            if _FRAME_TIMER.match(name):
                top = sorted(frame_acc.items(), key=lambda kv: kv[1], reverse=True)[:8]
                frames.append({"duration_ms": round(duration * 1000.0, 2), "start_s": round(entry[1] - duration, 3),
                               "top_timers_ms": [{"timer": n, "exclusive_ms": round(v * 1000.0, 2)} for n, v in top]})
                frame_acc.clear()
            else:
                frame_acc[name] = frame_acc.get(name, 0.0) + excl

    for row in rows:
        if len(row) <= needed:
            continue
        events += 1
        if events > max_events:
            truncated = True
            break
        try:
            start, end, depth = float(row[c_start]), float(row[c_end]), int(row[c_depth])
        except ValueError:
            continue
        thread = row[c_thread].strip()
        group = _thread_group(thread_names.get(thread, thread))
        if first_start is None or start < first_start:
            first_start = start
        last_end = max(last_end, end)
        stack = stacks.setdefault(thread, [])
        while stack and (stack[-1][0] >= depth or stack[-1][1] <= start):
            done = stack.pop()
            finish(thread, group, done, stack[-1] if stack else None)
        stack.append([depth, end, row[c_timer].strip(), end - start, 0.0])
    for thread, stack in stacks.items():
        group = _thread_group(thread_names.get(thread, thread))
        while stack:
            done = stack.pop()
            finish(thread, group, done, stack[-1] if stack else None)

    capture_seconds = (last_end - first_start) if first_start is not None else 0.0

    def timers_for(group: str, key: int) -> list[dict[str, Any]]:
        ranked = sorted(agg.get(group, {}).items(), key=lambda kv: kv[1][key], reverse=True)[:top_n]
        return [{"timer": name, "count": int(s[0]), "inclusive_ms": round(s[1] * 1000.0, 2), "exclusive_ms": round(s[2] * 1000.0, 2),
                 "avg_ms": round(s[1] * 1000.0 / s[0], 3), "max_ms": round(s[3] * 1000.0, 2)} for name, s in ranked]

    result: dict[str, Any] = {
        "capture_seconds": round(capture_seconds, 2),
        "events_read": min(events, max_events),
        "truncated": truncated,
        "thread_busy_seconds": {thread_names.get(t, t): round(v, 2) for t, v in sorted(busy.items(), key=lambda kv: kv[1], reverse=True)[:12]},
        "threads": {},
    }
    for group in ("game", "render", "rhi", "gpu", "other"):
        if group in agg:
            # Exclusive time shows where the time is really spent; inclusive shows the call tree top.
            result["threads"][group] = {"top_by_exclusive_time": timers_for(group, 2)}
            if group in ("game", "render"):
                result["threads"][group]["top_by_inclusive_time"] = timers_for(group, 1)
    if frames:
        durations = sorted(f["duration_ms"] for f in frames)
        result["frames"] = {
            "count": len(frames),
            "avg_ms": round(sum(durations) / len(durations), 2),
            "p95_ms": durations[int(len(durations) * 0.95) - 1 if len(durations) >= 20 else -1],
            "max_ms": durations[-1],
            "slowest": sorted(frames, key=lambda f: f["duration_ms"], reverse=True)[:slow_frames],
        }
    else:
        result["frames"] = {"note": "No frame timer (FEngineLoop::Tick) found on the game thread; enable the cpu and frame trace channels."}
    return result
