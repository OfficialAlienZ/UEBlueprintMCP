// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FCheckConditionAction
 *
 * Evaluate ONE condition against the running PIE game and report whether it is met right now.
 * Never blocks: the MCP server's wait_for_condition tool polls this command until it is met or times out.
 * Fails (error) if the parameters are invalid, a property does not exist, or PIE is not running
 * (except for "log_contains" and "pie_stopped"), so a poll loop notices when the game ended.
 *
 * Parameters:
 *   - type (required):
 *       "game_time"       met when the PIE game clock reached `seconds` (absolute game seconds)
 *       "actor_exists"    met when `actor_name` exists in the PIE world (name, label or @alias)
 *       "actor_property"  compare a property of `actor_name` (or its `component_name`):
 *                           `property_name` (dotted path into structs/objects, e.g. "Health" or "Stats.Level"),
 *                           `operator` (== != < <= > >= contains; default ==), `value` (number, bool or string)
 *       "actor_distance"  distance in cm between `actor_name` and `target_actor_name` (or `location` [x,y,z]),
 *                           compared with `value` using `operator` (default <=)
 *       "log_contains"    an Output Log line contains `text`; optional `since_id`, `categories`
 *       "pie_stopped"     met when no PIE session is running
 *   - negate (optional, default false): invert the result
 *
 * Returns met, the evaluated values (e.g. current_value, distance, game_time) and pie_running.
 * A missing actor is not an error: met is false with a reason, since it may spawn later.
 */
class UEBLUEPRINTMCP_API FCheckConditionAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("check_condition"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FTakeScreenshotAction
 *
 * Capture the PIE game view (or the active editor viewport) to a PNG under <Project>/Saved/MCP/Screenshots.
 *
 * Parameters:
 *   - target (optional, default "auto"): "auto" (PIE if running, else editor) | "pie" | "editor"
 *   - max_width (optional, default 960, 0 = full size): downscale so the image is at most this wide
 *   - label (optional): text added to the file name (letters, digits, - and _ only)
 *
 * Returns file (absolute path), width, height, target and game_time (PIE). The MCP server attaches the image.
 * The window must be visible (not minimised) for the viewport to render.
 */
class UEBLUEPRINTMCP_API FTakeScreenshotAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("take_screenshot"); }
	virtual bool RequiresSave() const override { return false; }
};
