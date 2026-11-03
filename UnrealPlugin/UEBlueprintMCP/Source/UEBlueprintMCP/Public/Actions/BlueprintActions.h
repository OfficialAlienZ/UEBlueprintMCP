// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

// Forward declarations
class UBlueprint;
class UBlueprintFactory;

/**
 * FCreateBlueprintAction
 *
 * Creates a new Blueprint asset with specified parent class.
 * Handles existing Blueprint cleanup and parent class resolution.
 *
 * Parameters:
 *   - name (required): Plain name (created in /Game/Blueprints/) or full asset path (/Game/Folder/BP_Name)
 *   - parent_class (optional): Class name (Actor, Pawn, ...), class path (/Script/Module.Class) or Blueprint (short name or /Game/Folder/BP_Name)
 *
 * Returns:
 *   - name: Created Blueprint name
 *   - path: Asset path
 */
class UEBLUEPRINTMCP_API FCreateBlueprintAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("create_blueprint"); }

public:
	/**
	 * Resolve a parent class from a native name, class path (/Script/Module.Class) or Blueprint
	 * (short name or asset path). Returns nullptr if unresolved. Shared with set_blueprint_property (reparent).
	 */
	static UClass* ResolveParentClass(const FString& ParentClassName);

private:
	/** Clean up existing Blueprint with same name */
	void CleanupExistingBlueprint(const FString& BlueprintName, const FString& PackagePath) const;
};


/**
 * FCompileBlueprintAction
 *
 * Compiles a Blueprint and reports errors/warnings.
 *
 * Parameters:
 *   - blueprint_name (required): Name of the Blueprint to compile
 *
 * Returns:
 *   - name: Blueprint name
 *   - success: Whether compilation succeeded
 *   - status: Compilation status string
 *   - error_count: Number of errors
 *   - warning_count: Number of warnings
 *   - errors: Array of error details (if any)
 *   - warnings: Array of warning details (if any)
 */
class UEBLUEPRINTMCP_API FCompileBlueprintAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("compile_blueprint"); }
	virtual bool RequiresSave() const override { return false; } // We save explicitly on success
	virtual bool BlocksDuringPIE() const override { return true; }

private:
	/** Collect compilation messages from nodes */
	void CollectCompilationMessages(UBlueprint* Blueprint,
		TArray<TSharedPtr<FJsonValue>>& OutErrors,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings) const;
};


/**
 * FAddComponentToBlueprintAction
 *
 * Adds a component to a Blueprint's component hierarchy.
 *
 * Parameters:
 *   - blueprint_name (required): Name of the target Blueprint
 *   - component_type (required): Type of component (StaticMeshComponent, etc.)
 *   - component_name (required): Name for the new component
 *   - location (optional): [X, Y, Z] relative location
 *   - rotation (optional): [Pitch, Yaw, Roll] relative rotation
 *   - scale (optional): [X, Y, Z] relative scale
 *
 * Returns:
 *   - component_name: Created component name
 *   - component_type: Component type
 */
class UEBLUEPRINTMCP_API FAddComponentToBlueprintAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("add_component_to_blueprint"); }

private:
	/** Resolve component class from type name */
	UClass* ResolveComponentClass(const FString& ComponentTypeName) const;

	/** Parse vector from JSON params */
	FVector GetVectorFromParams(const TSharedPtr<FJsonObject>& Params, const FString& FieldName) const;

	/** Parse rotator from JSON params */
	FRotator GetRotatorFromParams(const TSharedPtr<FJsonObject>& Params, const FString& FieldName) const;
};


/**
 * FSpawnBlueprintActorAction
 *
 * Spawns an instance of a Blueprint in the level.
 *
 * Parameters:
 *   - blueprint_name (required): Name of the Blueprint to spawn
 *   - actor_name (required): Name for the spawned actor
 *   - location (optional): [X, Y, Z] world location
 *   - rotation (optional): [Pitch, Yaw, Roll] world rotation
 *
 * Returns:
 *   - name: Actor name
 *   - class: Actor class
 *   - location: [X, Y, Z]
 *   - rotation: [Pitch, Yaw, Roll]
 */
class UEBLUEPRINTMCP_API FSpawnBlueprintActorAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("spawn_blueprint_actor"); }

private:
	/** Parse vector from JSON params */
	FVector GetVectorFromParams(const TSharedPtr<FJsonObject>& Params, const FString& FieldName) const;

	/** Parse rotator from JSON params */
	FRotator GetRotatorFromParams(const TSharedPtr<FJsonObject>& Params, const FString& FieldName) const;

	/** Convert actor to JSON response */
	TSharedPtr<FJsonObject> ActorToJson(AActor* Actor) const;
};


/**
 * FSetComponentPropertyAction
 *
 * Sets a property on a component in a Blueprint, or on a widget in a Widget Blueprint
 * (e.g. property_name "bIsVariable" = true to tick "Is Variable" in the Designer).
 *
 * Parameters:
 *   - blueprint_name (required): Name of the Blueprint
 *   - component_name (required): Name of the component (or widget)
 *   - property_name (required): Property to set
 *   - property_value (required): Value to set
 *
 * Returns:
 *   - component: Component name
 *   - property: Property name
 *   - success: Whether property was set
 */
class UEBLUEPRINTMCP_API FSetComponentPropertyAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_component_property"); }
};


/**
 * FSetStaticMeshPropertiesAction
 *
 * Sets mesh, material, and overlay material on a StaticMeshComponent.
 *
 * Parameters:
 *   - blueprint_name (required): Name of the Blueprint
 *   - component_name (required): Name of the StaticMeshComponent
 *   - static_mesh (optional): Path to static mesh asset
 *   - material (optional): Path to material asset
 *   - overlay_material (optional): Path to overlay material asset (for outline effects, etc.)
 *
 * Returns:
 *   - component: Component name
 */
class UEBLUEPRINTMCP_API FSetStaticMeshPropertiesAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_static_mesh_properties"); }
};


/**
 * FSetPhysicsPropertiesAction
 *
 * Sets physics properties on a primitive component.
 *
 * Parameters:
 *   - blueprint_name (required): Name of the Blueprint
 *   - component_name (required): Name of the component
 *   - simulate_physics (optional): Enable physics simulation
 *   - mass (optional): Mass in kg
 *   - linear_damping (optional): Linear damping
 *   - angular_damping (optional): Angular damping
 *
 * Returns:
 *   - component: Component name
 */
class UEBLUEPRINTMCP_API FSetPhysicsPropertiesAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_physics_properties"); }
};


/**
 * FSetBlueprintPropertyAction
 *
 * Sets a property on a Blueprint's class default object and/or reparents the Blueprint.
 *
 * Parameters:
 *   - blueprint_name (required): Name of the Blueprint
 *   - property_name: Property to set (required unless parent_class is given)
 *   - property_value: Value to set (required with property_name)
 *   - parent_class (optional): New parent class (native name, /Script/... path, or Blueprint name/path).
 *     Reparents, refreshes nodes and compiles.
 *
 * Returns:
 *   - property: Property name
 *   - old_parent_class / parent_class / compiled_without_errors (when reparenting)
 *   - success: Whether the operation succeeded
 */
class UEBLUEPRINTMCP_API FSetBlueprintPropertyAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_blueprint_property"); }
};


/**
 * FCreateColoredMaterialAction
 *
 * Creates a simple colored material asset.
 *
 * Parameters:
 *   - material_name (required): Name for the material
 *   - color (optional): [R, G, B] color values (0.0-1.0)
 *
 * Returns:
 *   - name: Material name
 *   - path: Asset path
 *   - success: Whether material was created
 */
class UEBLUEPRINTMCP_API FCreateColoredMaterialAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("create_colored_material"); }
};

/**
 * Remove something from a Blueprint: a component (SCS), a variable (and its get/set nodes), a function graph, an input
 * of a function, or a widget of a Widget Blueprint's designer tree. Compiles and saves.
 */
class UEBLUEPRINTMCP_API FRemoveFromBlueprintAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("remove_from_blueprint"); }
};

/**
 * Delete assets by path. Refuses (listing them) when other assets still reference one, unless force is true.
 */
class UEBLUEPRINTMCP_API FDeleteAssetAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("delete_asset"); }
};
