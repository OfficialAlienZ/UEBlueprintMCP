// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FAddStructNodeAction
 *
 * Add a Break Struct or Make Struct node (K2Node_BreakStruct / K2Node_MakeStruct).
 * These are not function calls, so add_blueprint_function_node cannot create them.
 *
 * Parameters:
 *   - blueprint_name: Target Blueprint
 *   - struct_type: User Defined Struct asset name or path (S_Item, /Game/Data/S_Item) or native struct
 *       (Vector, FVector, /Script/Engine.HitResult)
 *   - mode (optional, default "break"): "break" or "make"
 *   - graph_name, node_position (optional)
 *
 * Returns node_id plus the node's pins (name, display name, direction, type). Pin names of User Defined
 * Structs carry a GUID suffix: use the returned "name" values with connect_blueprint_nodes.
 */
class UEBLUEPRINTMCP_API FAddStructNodeAction : public FBlueprintNodeAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("add_struct_node"); }
};


/**
 * FAddGetDataTableRowNodeAction
 *
 * Add a "Get Data Table Row" node (K2Node_GetDataTableRow). Its Result pin takes the row struct type
 * of the Data Table, so it cannot be created as a plain function call.
 *
 * Parameters:
 *   - blueprint_name: Target Blueprint
 *   - data_table (optional): Data Table asset name or path. Sets the DataTable pin default, which
 *       also types the Result pin. Omit to wire the DataTable pin from a variable/other node.
 *   - row_name (optional): Row to look up (must exist in data_table). Defaults to the first row.
 *   - graph_name, node_position (optional)
 *
 * Pins: execute, DataTable, RowName, then, RowNotFound, OutRow.
 */
class UEBLUEPRINTMCP_API FAddGetDataTableRowNodeAction : public FBlueprintNodeAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("add_get_data_table_row_node"); }
};
