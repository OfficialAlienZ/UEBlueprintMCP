// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FGetBlueprintInfoAction
 *
 * Read-only introspection of a Blueprint asset: everything needed to document it
 * without opening the editor.
 *
 * Parameters:
 *   - blueprint_name (optional): Blueprint name or full asset path (defaults to current Blueprint)
 *   - sections (optional): Array of sections to include. Default: all.
 *       parents, interfaces, graphs, variables, functions, macros, dispatchers,
 *       events, components, class_defaults, widgets
 *   - include_component_properties (optional, default true): Include properties that differ
 *       from the component class defaults (components / inherited components)
 *
 * Always returns summary fields (name, path, blueprint_type, parent_class, status).
 * Each requested section is returned under a key of the same name.
 */
class UEBLUEPRINTMCP_API FGetBlueprintInfoAction : public FBlueprintAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_blueprint_info"); }
	virtual bool RequiresSave() const override { return false; }
};
