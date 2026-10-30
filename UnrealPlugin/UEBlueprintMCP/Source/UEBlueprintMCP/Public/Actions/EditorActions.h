// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

class AActor;

/**
 * FGetActorsInLevelAction
 * Returns all actors in the current level.
 *
 * Optional "world": "editor" (default) | "pie" (the running game, including runtime-spawned actors) | "auto".
 */
class UEBLUEPRINTMCP_API FGetActorsInLevelAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_actors_in_level"); }
	virtual bool RequiresSave() const override { return false; }
};


/**
 * FFindActorsByNameAction
 * Finds actors matching a name pattern.
 */
class UEBLUEPRINTMCP_API FFindActorsByNameAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("find_actors_by_name"); }
	virtual bool RequiresSave() const override { return false; }
};


/**
 * FSpawnActorAction
 * Spawns a basic actor type in the level.
 */
class UEBLUEPRINTMCP_API FSpawnActorAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("spawn_actor"); }

private:
	UClass* ResolveActorClass(const FString& TypeName) const;
};


/**
 * FDeleteActorAction
 * Deletes an actor from the level.
 */
class UEBLUEPRINTMCP_API FDeleteActorAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("delete_actor"); }
};


/**
 * FSetActorTransformAction
 * Sets the transform (location/rotation/scale) of an actor.
 * Optional "world": "editor" (default) | "pie" (teleport an actor of the running game; nothing is saved).
 * The actor "name" may be an actor name, an editor label or, in PIE, an alias such as @player_pawn.
 */
class UEBLUEPRINTMCP_API FSetActorTransformAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_actor_transform"); }
	// Editing the editor level is blocked during PIE (checked in Validate); world "pie" is allowed.
	virtual bool BlocksDuringPIE() const override { return false; }
};


/**
 * FGetActorPropertiesAction
 * Gets an actor's level placement (components, overrides) and, on request, all its reflected properties.
 *
 * Parameters:
 *   - name: Actor name, label or, with world "pie", an alias (@player_pawn, @player_controller, @player_state,
 *       @hud, @game_mode, @game_state, @level_script). "WorldSettings" = the level's World Settings
 *   - world (optional, default "editor"): "editor" | "pie" (runtime values of the running game) | "auto"
 *   - include_properties (optional, default true): changed component/actor properties
 *   - include_all_properties (optional, default false): add "all_properties" with every UPROPERTY as Export Text
 *   - property_names (optional): only these properties in "all_properties" (implies include_all_properties)
 *   - editable_only (optional, default false): restrict "all_properties" to editor-exposed properties
 */
class UEBLUEPRINTMCP_API FGetActorPropertiesAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_actor_properties"); }
	virtual bool RequiresSave() const override { return false; }
};


/**
 * FSetActorPropertyAction
 * Sets a property on an actor.
 * Optional "world": "editor" (default) | "pie" (change a variable of the running game, e.g. to set up a
 * test scenario; nothing is saved). The actor "name" accepts the same aliases as get_actor_properties.
 */
class UEBLUEPRINTMCP_API FSetActorPropertyAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_actor_property"); }
	// Editing the editor level is blocked during PIE (checked in Validate); world "pie" is allowed.
	virtual bool BlocksDuringPIE() const override { return false; }
};


/**
 * FFocusViewportAction
 * Focuses the viewport on an actor or location.
 */
class UEBLUEPRINTMCP_API FFocusViewportAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("focus_viewport"); }
	virtual bool RequiresSave() const override { return false; }
};


/**
 * FGetViewportTransformAction
 * Gets the current viewport camera location and rotation.
 */
class UEBLUEPRINTMCP_API FGetViewportTransformAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override { return true; }
	virtual FString GetActionName() const override { return TEXT("get_viewport_transform"); }
	virtual bool RequiresSave() const override { return false; }
};


/**
 * FSetViewportTransformAction
 * Sets the viewport camera location and/or rotation.
 */
class UEBLUEPRINTMCP_API FSetViewportTransformAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_viewport_transform"); }
	virtual bool RequiresSave() const override { return false; }
};


/**
 * FSaveAllAction
 * Saves all dirty packages (blueprints, levels, assets).
 */
class UEBLUEPRINTMCP_API FSaveAllAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override { return true; }
	virtual FString GetActionName() const override { return TEXT("save_all"); }
	virtual bool RequiresSave() const override { return false; }
	virtual bool BlocksDuringPIE() const override { return true; }
};
