// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FCallActorFunctionAction
 *
 * Call a Blueprint function or custom event on an actor (or one of its components) in the running
 * Play-In-Editor world. Works with any project; no C++ or reflection setup is needed in the game.
 *
 * Parameters:
 *   - actor_name (required): Actor name or label in the PIE world, or a runtime alias:
 *       @player_pawn, @player_controller, @player_state, @hud, @game_mode, @game_state, @level_script
 *   - function_name (required): Function or custom event name (case-insensitive)
 *   - component_name (optional): Call the function on this component of the actor instead
 *   - args (optional): Object mapping parameter name -> value. Numbers, bools, strings, enums (by name),
 *       structs (JSON object), arrays, vectors/rotators as [x, y, z], actors by name/label/@alias.
 *       Omitted parameters are zero-initialised (Blueprint default values are not applied)
 *
 * Returns the function's outputs (return_value and out parameters) and the game time of the call.
 * Requires a running PIE session (start_pie).
 */
class UEBLUEPRINTMCP_API FCallActorFunctionAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("call_actor_function"); }
	virtual bool RequiresSave() const override { return false; }

private:
	/** Resolve actor / component and function from Params. */
	bool Resolve(const TSharedPtr<FJsonObject>& Params, UObject*& OutTarget, UFunction*& OutFunction, FString& OutError) const;
};

/**
 * FInjectInputAction
 *
 * Feed key / axis input to a player controller in the running PIE world, exactly as if it came from
 * the keyboard, mouse or a gamepad (it goes through the controller's input stack, so Input Action /
 * Axis events and legacy input bindings fire). Timed inputs run in game time, so they pause with the game.
 * It does not reach UMG widgets (those need real Slate input) and does not need the PIE window focused.
 *
 * Parameters:
 *   - action (optional, default "tap"):
 *       "tap"         press, then release after `duration` seconds (default 0.1)
 *       "press"       press and keep held (released after `duration` if given)
 *       "release"     release a held key
 *       "release_all" release every key held through this command and flush the controller's pressed keys
 *   - key: Key name (required except for "release_all"): "W", "SpaceBar", "LeftMouseButton", "Gamepad_LeftX",
 *       "MouseX", "Escape", ... (Unreal FKey names)
 *   - value (optional, default 1): Analog value / axis value (a per-frame delta for mouse axes).
 *       Axis keys (MouseX, Gamepad_LeftX...) re-send the value every frame while held
 *   - duration (optional): Seconds of game time to hold
 *   - player_index (optional, default 0)
 *
 * Requires a running PIE session (start_pie).
 */
class UEBLUEPRINTMCP_API FInjectInputAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("inject_input"); }
	virtual bool RequiresSave() const override { return false; }
};
