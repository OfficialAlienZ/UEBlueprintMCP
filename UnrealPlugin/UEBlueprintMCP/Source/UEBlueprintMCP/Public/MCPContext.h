// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

/**
 * FMCPEditorContext
 *
 * Tracks the current editing context across MCP commands.
 * This allows commands to reference the "current" blueprint/graph
 * without specifying it each time.
 */
struct UEBLUEPRINTMCP_API FMCPEditorContext
{
public:
	FMCPEditorContext();
	~FMCPEditorContext();
	FMCPEditorContext(const FMCPEditorContext&) = delete;
	FMCPEditorContext& operator=(const FMCPEditorContext&) = delete;

	// =========================================================================
	// Current Focus
	// =========================================================================

	/** Currently active Blueprint (weak reference to allow GC) */
	TWeakObjectPtr<UBlueprint> CurrentBlueprint;

	/** Name of the current graph (event graph, function graph, etc.) */
	FName CurrentGraphName;

	/** Currently active world */
	TWeakObjectPtr<UWorld> CurrentWorld;

	// =========================================================================
	// Material Editor Context
	// =========================================================================

	/** Currently active Material */
	TWeakObjectPtr<UMaterial> CurrentMaterial;

	/** Map of node names to expressions (for connecting by name) */
	TMap<FString, TWeakObjectPtr<UMaterialExpression>> MaterialNodeMap;

	/** Name of the last created material expression node */
	FString LastCreatedMaterialNodeName;

	// =========================================================================
	// Recently Created Objects (for command chaining)
	// =========================================================================

	/** GUID of the last created node */
	FGuid LastCreatedNodeId;

	/** Name of the last created actor */
	FString LastCreatedActorName;

	/** Name of the last created widget */
	FString LastCreatedWidgetName;

	// =========================================================================
	// Dirty Tracking
	// =========================================================================

	/** Packages that have been modified and need saving */
	TSet<UPackage*> DirtyPackages;

private:
	/** Handle of the pending deferred-save ticker (invalid when no save is pending) */
	FDelegateHandle SaveTickerHandle;

	/** FPlatformTime::Seconds() when the oldest unsaved request was made */
	double PendingSaveSince = 0.0;

public:

	// =========================================================================
	// Methods
	// =========================================================================

	/** Set the current Blueprint focus */
	void SetCurrentBlueprint(UBlueprint* BP);

	/** Set the current graph by name */
	void SetCurrentGraph(const FName& GraphName);

	/** Get the current graph (event graph if none specified) */
	UEdGraph* GetCurrentGraph() const;

	/** Get the event graph for the current Blueprint */
	UEdGraph* GetEventGraph() const;

	/** Mark a package as dirty (needs saving) */
	void MarkPackageDirty(UPackage* Package);

	/** Save all dirty packages immediately */
	void SaveDirtyPackages();

	/**
	 * Schedule a save shortly after the last request (debounced). Rapid command bursts
	 * collapse into one save instead of one save (and Blueprint compile) per command.
	 */
	void RequestSave();

	/** Save now if a deferred save is pending. Call before operations that need disk state (e.g. PIE). */
	void FlushPendingSave();

	/** Clear the context (reset to defaults) */
	void Clear();

	/** Convert context to JSON for Python inspection */
	TSharedPtr<FJsonObject> ToJson() const;

	// =========================================================================
	// Material Context Methods
	// =========================================================================

	/** Set the current Material focus */
	void SetCurrentMaterial(UMaterial* Mat);

	/** Register a created expression by name for later connection */
	void RegisterMaterialNode(const FString& NodeName, UMaterialExpression* Expr);

	/** Get expression by registered name */
	UMaterialExpression* GetMaterialNode(const FString& NodeName) const;

	/** Clear material nodes map (when switching materials) */
	void ClearMaterialNodes();

	/** Get Material by name, or use current if name is empty */
	UMaterial* GetMaterialByNameOrCurrent(const FString& MaterialName) const;

	// =========================================================================
	// Convenience Methods
	// =========================================================================

	/** Get Blueprint by name, or use current if name is empty */
	UBlueprint* GetBlueprintByNameOrCurrent(const FString& BlueprintName) const;

	/** Get graph by name, or use current/event graph if name is empty */
	UEdGraph* GetGraphByNameOrCurrent(const FString& GraphName) const;

	/** Resolve $last_node to actual node ID */
	FGuid ResolveNodeId(const FString& NodeIdOrAlias) const;
};
