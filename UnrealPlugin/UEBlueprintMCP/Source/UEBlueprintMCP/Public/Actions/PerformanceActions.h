// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FGetPerformanceStatsAction
 *
 * Read-only snapshot of the current frame timings and memory, plus the engine / project paths
 * the Python side needs (Saved dir for CSV profiles and memreports, engine dir for RunUAT).
 *
 * Returns: fps, frame_ms, game_thread_ms, render_thread_ms, rhi_thread_ms, gpu_ms, bottleneck
 * (game / render / gpu / rhi / frame_cap), memory (process + texture pool), rhi, feature level, cvars,
 * pie state and the project paths.
 */
class UEBLUEPRINTMCP_API FGetPerformanceStatsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override { return true; }
	virtual FString GetActionName() const override { return TEXT("get_performance_stats"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FAuditAssetsAction
 *
 * Read-only cost audit of assets: finds the assets that are expensive or badly configured.
 *
 * Parameters:
 *   - asset_type: texture | static_mesh | skeletal_mesh | material | blueprint
 *   - path (optional, default /Game): Content folder to scan (recursive)
 *   - name_contains (optional): Only assets whose name contains this text
 *   - limit (optional, default 30): Max entries returned (sorted by cost, worst first)
 *   - max_scan (optional, default 400, max 3000): Max assets loaded and inspected
 *
 * Returns per asset the measured values (size, triangles, instructions, tick settings...), an estimated
 * cost number used for sorting and an `issues` array (severity + message). Summary totals cover every scanned asset.
 */
class UEBLUEPRINTMCP_API FAuditAssetsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("audit_assets"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FAuditLevelAction
 *
 * Read-only audit of a loaded level (world param: editor / pie / auto):
 * actor / component counts, ticking actors, instancing candidates (same mesh placed many times),
 * movable meshes, lights (mobility, shadows, radius), volumes, navigation (RecastNavMesh settings and
 * bounds volumes), streaming levels and HLOD / world settings. Returns `issues` and per-category data.
 */
class UEBLUEPRINTMCP_API FAuditLevelAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("audit_level"); }
	virtual bool RequiresSave() const override { return false; }
	virtual bool BlocksDuringPIE() const override { return false; }
};
