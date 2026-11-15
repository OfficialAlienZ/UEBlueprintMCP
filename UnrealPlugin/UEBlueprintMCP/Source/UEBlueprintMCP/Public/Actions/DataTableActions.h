// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FSetDataTableRowAction
 *
 * Edit a Data Table row: update some columns, add a row, or delete a row.
 * Counterpart of get_data_table_info (same column names and value formats).
 *
 * Parameters:
 *   - table_name: Data Table name or full asset path (e.g. /Game/Data/CDT_SCPNItems)
 *   - row_name: Row to edit
 *   - values (optional): column name -> value. Only the listed columns change.
 *       Column names are the names authored in the editor (as returned by get_data_table_info).
 *       Text/String: JSON string. Numbers/bools: JSON number/bool. Enums: name or display name.
 *       Structs: nested object (partial updates allowed). Arrays: JSON array (replaces the array).
 *   - create_if_missing (optional, default false): Add the row (default values + values) if it does not exist
 *   - delete_row (optional, default false): Remove the row instead of editing it
 *
 * All values are validated on a copy of the row first, so a failing column leaves the row untouched.
 * Composite Data Tables are read-only.
 */
class UEBLUEPRINTMCP_API FSetDataTableRowAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_data_table_row"); }
};

/**
 * FCreateDataTableAction
 *
 * Create a Data Table asset with a row struct. Counterpart of set_data_table_row (which writes the rows).
 *
 * Parameters:
 *   - name: Asset name, e.g. DT_Example (letters, digits, underscores)
 *   - path (optional, default /Game/Data): Content folder that receives the table (must be under /Game)
 *   - columns (optional): [{"name": "Value", "type": "Float"}, ...]. Creates a User Defined Struct
 *       S_<name without DT_ prefix> (DT_Example -> S_Example) with these fields and uses it as the row struct.
 *   - row_struct (optional): Existing row struct (name or path, e.g. S_Item or /Game/Data/S_Item).
 *
 * Exactly one of columns / row_struct is required. Existing assets are never overwritten.
 * Column types: Boolean, Integer, Int64, Float, String, Name, Text, Vector, Vector2D, Rotator, Transform,
 * an enum or a struct name. Prefix "Array of " (or suffix "[]") makes an array column.
 */
class UEBLUEPRINTMCP_API FCreateDataTableAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("create_data_table"); }
};
