// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FOptimizeStaticMeshAction
 *
 * Applies optimization settings to Static Mesh assets (UE 4.27; LOD / collision generation is not wired for UE5 yet).
 *
 * Parameters:
 *   - mesh: Static mesh asset name or path (/Game/Meshes/SM_Rock). Or `meshes`: array of the same
 *   - lods (optional): array of {percent_triangles (0-1), screen_size (0-1, optional)} applied as LOD1..N
 *       (LOD0 is the source mesh). Replaces the existing generated LODs. `auto_compute_screen_size`
 *       (default true when no screen_size is given) lets the engine pick the swap distances.
 *   - remove_lods (optional): Remove all generated LODs
 *   - lod_group (optional): LOD group name (e.g. LevelArchitecture, SmallProp, LargeProp, Deco, Vehicle) or "None"
 *   - collision (optional): none | box | sphere | capsule | kdop10x | kdop10y | kdop10z | kdop18 | kdop26 | convex
 *       (convex uses hull_count / max_hull_verts / hull_precision)
 *   - collision_complexity (optional): project_default | simple_and_complex | use_simple_as_complex | use_complex_as_simple
 *   - lightmap_resolution (optional), generate_lightmap_uvs (optional), allow_cpu_access (optional),
 *       distance_field (optional bool, bGenerateMeshDistanceField), min_lod (optional)
 *
 * Returns per mesh the before / after LOD count, triangles per LOD and collision shape count.
 */
class UEBLUEPRINTMCP_API FOptimizeStaticMeshAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("optimize_static_mesh"); }
};

/**
 * FOptimizeTextureAction
 *
 * Applies optimization settings to Texture2D assets.
 *
 * Parameters:
 *   - texture: Texture asset name or path. Or `textures`: array of the same (same settings for all)
 *   - max_texture_size (optional): cap in pixels, e.g. 1024 / 2048 (0 removes the cap)
 *   - lod_group (optional): TextureGroup name (World, WorldNormalMap, Character, UI, Effects, ...)
 *   - compression (optional): TextureCompressionSettings name (TC_Default, TC_Normalmap, TC_Masks, TC_Grayscale, TC_Alpha, TC_BC7, ...)
 *   - mip_gen (optional): TextureMipGenSettings name (TMGS_FromTextureGroup, TMGS_NoMipmaps, TMGS_Sharpen0, ...)
 *   - never_stream, srgb (optional bool), lod_bias (optional int)
 *
 * Returns before / after size in MB per texture.
 */
class UEBLUEPRINTMCP_API FOptimizeTextureAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("optimize_texture"); }
};

/**
 * FSetProjectConfigAction
 *
 * Persistently edits the project's Config/Default*.ini (Engine, Game, Input, Editor, Scalability, EditorPerProjectUserSettings,
 * GameUserSettings). Unlike set_object_property this survives editor restarts and is used by packaged builds.
 * A .mcpbak copy of the file is written first. Comments and unrelated lines are preserved.
 *
 * Parameters:
 *   - file: Engine | Game | Input | Editor | Scalability | GameUserSettings | EditorPerProjectUserSettings
 *   - section: e.g. /Script/Engine.RendererSettings, [SystemSettings], ShadowQuality@2
 *   - key + value, or `values`: object of key -> value
 *   - operation (optional): set (default, replace or add) | add (append a +Key=Value array line) | remove (delete the key)
 *   - apply_live (optional, default false): also set the matching console variable in the running editor
 *
 * Returns the written lines and whether a restart is needed.
 */
class UEBLUEPRINTMCP_API FSetProjectConfigAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_project_config"); }
	virtual bool RequiresSave() const override { return false; }
	virtual bool BlocksDuringPIE() const override { return false; }
};

/**
 * FManageLevelStreamingAction
 *
 * Lists and edits the streaming sub-levels of the editor level.
 *
 * Parameters:
 *   - action: list | add | remove | set
 *   - level: package name (/Game/Maps/Sub) for add / remove / set
 *   - streaming_class (add, optional): always_loaded (default) | blueprint (Level Streaming Dynamic)
 *   - should_be_visible / should_be_loaded (set, optional bools)
 */
class UEBLUEPRINTMCP_API FManageLevelStreamingAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("manage_level_streaming"); }
	// add / remove / set save explicitly and are rejected in Validate() while PIE is active; list is read-only.
	virtual bool RequiresSave() const override { return false; }
	virtual bool BlocksDuringPIE() const override { return false; }
};

/**
 * FBuildLevelAction
 *
 * Starts an editor build of the current level, or reports build status.
 *
 * Parameters:
 *   - what: lighting | navmesh | hlod | geometry | status
 *   - quality (lighting only): preview | medium | high | production (default preview)
 *
 * Lighting builds run in the background (poll with what=status). Saves dirty packages first so the build
 * does not show a save prompt. Not allowed while PIE runs.
 */
class UEBLUEPRINTMCP_API FBuildLevelAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("build_level"); }
	virtual bool RequiresSave() const override { return false; }
	virtual bool BlocksDuringPIE() const override { return true; }
};
