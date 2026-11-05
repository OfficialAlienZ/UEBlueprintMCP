# Optimizing an Unreal game with the MCP

For people with no profiling experience. The AI runs the tools; this guide is the workflow it follows.
Rule one: **never optimize before measuring, and always re-measure after each change.**

## The loop

```
1. Measure      get_performance_stats -> capture_performance
2. Classify     bottleneck = game thread | render thread | GPU | frame_cap
3. Find causes  audit_level / audit_assets / profile_gpu / dump_cpu_stats / get_memory_report / capture_viewmode
4. Fix          optimize_texture / optimize_static_mesh / set_project_config / manage_level_streaming / build_level / Blueprint edits
5. Re-measure   capture_performance again (same scene, same length) and compare
6. Confirm      package_project -> profile_packaged_build (editor numbers are not real game numbers)
```

## Which bottleneck?

`capture_performance` returns `bottleneck`, based on the average thread / GPU time versus the frame time:

| Result | Meaning | Next tools |
|---|---|---|
| `game` | Logic, ticking actors, animation, physics, AI (CPU, game thread) | `audit_level` (ticking actors), `audit_assets blueprint`, `dump_cpu_stats` |
| `render` | Too many draw calls / primitives (CPU, render thread) | `audit_level` (instancing candidates), `audit_assets material` / `static_mesh`, LODs, HLOD |
| `gpu` | Pixel/shadow/post-process cost | `profile_gpu`, `capture_viewmode shadercomplexity`, `audit_assets material` / `texture`, scalability settings |
| `frame_cap` | Nothing is busy: vsync, `t.MaxFPS`, unfocused editor window, loading stall | Re-measure with the window focused, `r.VSync 0`, `t.MaxFPS 0` |

Also read `worst_hitches` (stutters): each hitch lists the stat that spiked. Stutter is usually loading (streaming, asset
loads, spawning), garbage collection, shader compilation or a Blueprint doing heavy work once.

## Tool reference

| Need | Tool |
|---|---|
| Current fps, thread times, memory, RHI, cvars | `get_performance_stats` |
| Recorded capture with percentiles, top stats, hitches, bottleneck, advice | `capture_performance` (live) / `analyze_performance_csv` (existing file) |
| What the GPU spends time on (passes) | `profile_gpu` |
| What the CPU spends time on (stat tree) | `dump_cpu_stats` (raw `stat dumpave/dumpframe/dumphitches`) |
| Expensive materials / overdraw / LOD / light overlap, as screenshots | `capture_viewmode` (needs PIE) |
| What uses memory | `get_memory_report` |
| Unreal Insights trace | `trace_control` (record), then `analyze_trace` (UE 5.1+ with a UnrealInsights binary; headless CSV export aggregated to slowest frames and top timers per thread). Not available on 4.27 |
| Did the fix work? Editor vs packaged gap? | `compare_performance` (two CSV files, with deltas and a verdict) |
| Physics cost (simulating bodies, collision, events, substeps) | `profile_physics` |
| Which project C++ is behind a CPU hotspot | `scan_project_cpp` (pattern scan + symbol lookup in Source/ and Plugins/*/Source/) |
| Expensive or badly configured assets | `audit_assets` (texture, static_mesh, skeletal_mesh, material, blueprint) |
| Level problems (ticking, instancing, lights, navmesh, streaming) | `audit_level` |
| Shrink textures / change compression | `optimize_texture` |
| LODs, collision, lightmap res, distance fields | `optimize_static_mesh` (UE 4.27) |
| Persistent settings (rendering cvars, scalability, physics) | `set_project_config` (+ `get_project_config` to read) |
| Split a map, stream sub-levels | `manage_level_streaming` |
| Build lighting / navmesh / HLOD | `build_level` (poll `what=status` for lighting) |
| Real game numbers | `package_project` then `profile_packaged_build` |

## Typical fixes by finding

- **Textures over 2048 on small props, wrong compression, no streaming**: `audit_assets texture`, then `optimize_texture`
  (`max_texture_size`, `compression` TC_Normalmap / TC_Masks, `mip_gen` TMGS_NoMipmaps for UI).
- **High triangle count without LODs, tiny-object complex collision**: `audit_assets static_mesh`, then `optimize_static_mesh`
  (`lods` [{percent_triangles: 0.5}, {percent_triangles: 0.25}], `collision` box / convex, `collision_complexity` use_simple_as_complex).
- **Hundreds of actors with the same mesh**: `audit_level` lists `instancing_candidates`; convert to an Instanced Static Mesh
  component (in the editor: Merge Actors / ISM).
- **Many movable shadow-casting lights**: make them Static / Stationary, or disable shadows (`set_actor_property`).
- **Many ticking actors / Blueprints with Tick**: disable Start With Tick Enabled, use timers/events, lower tick interval.
- **GPU bound at high resolution**: lower `r.ScreenPercentage`, shadow resolution, post-process features
  (`set_project_config file=Engine section=/Script/Engine.RendererSettings`, or a Scalability preset).
- **Big world**: `manage_level_streaming` + `build_level what=hlod`.

## Rules of thumb for the AI

1. One change at a time, then `capture_performance` again with the same `seconds` and the same scene state.
2. Always prefer the cheap read-only audits first; tell the user before running bulk `optimize_*` calls on many assets
   (they recompress / rebuild, and edits are saved automatically).
3. Do not chase editor-only costs: measure in PIE, confirm in a packaged Development build.
4. Report numbers before/after, not just "should be faster".
5. A 60 fps target is 16.7 ms per frame, 30 fps is 33.3 ms, 90 fps (VR) is 11.1 ms.

## Packaged builds

`package_project action=start` runs `RunUAT BuildCookRun` in the background (default configuration Development, which keeps
the CSV profiler). Poll `action=status`; the log is under `<Project>/Saved/MCP/`. Then `profile_packaged_build action=start`
launches the game with `-csvCaptureFrames=N -ExitAfterCsvProfiling`, and `action=status` returns the same analysis as
`capture_performance`. The game only measures what it does at startup (pass `map` to choose the level); it does not play the
game itself. Shipping builds have no CSV profiler.

## Limits (be honest with the user)

- Unreal Insights `.utrace` files are read by `analyze_trace` only where a UnrealInsights binary with the `TimingInsights.Export*` command-line exporters exists (UE 5.1+; pass `insights_path` or set `UEBLUEPRINTMCP_INSIGHTS` for a binary outside the engine). UE 4.27 ships Insights (GUI and `-OpenTraceFile`/`-AutoQuit`) but without those exporters: open the trace in the GUI, or use the CSV/stat/GPU tools. Memory/counter exports need UE 5.5+ and are not covered; use `get_memory_report`.
- Editor numbers include editor overhead and are not the numbers players get; the final check is a packaged build.
  Quantify the gap with `compare_performance` (`label_before=editor`, `label_after=packaged`) on two captures of the same scene.
- `profile_gpu` needs GPU timestamp support from the RHI; on Metal it may return no hierarchy (raw lines are returned instead).
- `optimize_static_mesh` LOD/collision generation uses the 4.27 `UEditorStaticMeshLibrary`; on UE5 it returns an error.
- C++ hot spots: `scan_project_cpp` finds suspicious code (per-frame actor scans, sync loads, traces, spawns, enabled ticks) and
  locates the functions that `dump_cpu_stats` / `capture_performance` name. It only reads text: it is not a profiler, and the
  MCP cannot edit or rebuild game C++. Fix the code with a normal editor and rebuild, then re-measure.
- Physics: `profile_physics` gives raw physics stat dumps, the project's PhysicsSettings and a checklist; the stat group names
  differ per engine version, so empty dumps are possible. Tuning is done through `optimize_static_mesh` collision,
  `set_project_config` and Blueprint/level edits.
- Thread-time values in `get_performance_stats` can read 0 until `stat unit` has run once.
- Lighting builds run in the background and may take long; `build_level` never blocks the editor.
- The CSV format, `profilegpu` log format and packaged-build launch are parsed tolerantly but depend on the engine version.
