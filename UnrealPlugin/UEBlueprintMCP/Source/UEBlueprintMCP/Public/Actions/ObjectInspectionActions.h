// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FGetObjectPropertiesAction
 *
 * Read-only export of every reflected property (UPROPERTY) of any object, as the editor's Export Text.
 * Counterpart of the property setters; reaches what has no Blueprint asset of its own
 * (World Settings, class defaults, components, settings objects).
 *
 * Parameters (exactly one target):
 *   - object_path: full object path, e.g. /Script/EngineSettings.Default__GameMapsSettings or
 *       /Game/Data/DA_Config (asset). The shortcut "world_settings" is the open level's World Settings
 *       (DefaultGameMode, ...).
 *   - actor_name: actor in the level (name or label); component_name (optional) selects one of its components.
 *       With world "pie" the runtime aliases @player_pawn, @player_controller, @player_state, @hud,
 *       @game_mode, @game_state and @level_script are also accepted
 *   - blueprint_name: Blueprint (name, path or "level:<Map>"); its class default object. component_name
 *       (optional) selects a Blueprint component template instead
 *   - world (optional, default "editor"): "editor" | "pie" | "auto". Level of actor_name / world_settings:
 *       "pie" reads the live values of the running game
 *   - property_names (optional): only these properties
 *   - editable_only (optional, default false): only properties exposed to the editor
 */
class UEBLUEPRINTMCP_API FGetObjectPropertiesAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_object_properties"); }
	virtual bool RequiresSave() const override { return false; }

private:
	UObject* ResolveTarget(const TSharedPtr<FJsonObject>& Params, FString& OutError) const;
};

/**
 * FGetProjectConfigAction
 *
 * Read-only access to the project's ini configuration (e.g. the default map, which is a project
 * setting and not an asset).
 *
 * Parameters:
 *   - file: Engine, Game, Input, Editor, EditorPerProjectUserSettings, GameUserSettings or Scalability
 *   - section (optional): e.g. /Script/EngineSettings.GameMapsSettings. Omitted = list the section names
 *   - key (optional): e.g. GameDefaultMap. Omitted = every key of the section
 *   - layer (optional): "merged" (default; the config as the editor sees it, all layers applied) or
 *       "project_default" (the raw Config/Default<File>.ini of the project, with +/-/. operators)
 *   - contains (optional): when listing sections, only names containing this text (case-insensitive)
 */
class UEBLUEPRINTMCP_API FGetProjectConfigAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_project_config"); }
	virtual bool RequiresSave() const override { return false; }
};
