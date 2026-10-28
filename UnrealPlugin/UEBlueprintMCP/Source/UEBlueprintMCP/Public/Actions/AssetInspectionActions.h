// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FGetEnumInfoAction
 *
 * Read-only introspection of an Enum (User Defined Enum asset or native UENUM).
 *
 * Parameters:
 *   - enum_name: Enum asset name / full asset path (e.g. /Game/Data/E_State), or a native enum
 *       name (e.g. ECollisionChannel)
 *
 * Returns the entries (index, name, display_name, value, tooltip, hidden) without the
 * auto-generated _MAX entry.
 */
class UEBLUEPRINTMCP_API FGetEnumInfoAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_enum_info"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FGetCurveInfoAction
 *
 * Read-only introspection of a Curve asset: Float, Vector, Linear Color or Curve Table.
 *
 * Parameters:
 *   - curve_name: Curve asset name or full asset path (e.g. /Game/Curves/C_Speed)
 *
 * Returns the keys of every channel (time/value/interp/tangents) and the extrapolation modes.
 * Curve Tables return one curve per row.
 */
class UEBLUEPRINTMCP_API FGetCurveInfoAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_curve_info"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FGetDataTableInfoAction
 *
 * Read-only introspection of a Data Table asset.
 *
 * Parameters:
 *   - table_name: Data Table name or full asset path (e.g. /Game/Data/DT_Items)
 *   - include_rows (optional, default true): Include the row values
 *   - row_names (optional): Only return these rows
 *   - offset / limit (optional, default 0 / 200): Page through the rows
 *
 * Returns the row struct, its columns (authored name + type) and the rows in table order.
 * Values of User Defined Structs / Enums use the names authored in the editor.
 */
class UEBLUEPRINTMCP_API FGetDataTableInfoAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_data_table_info"); }
	virtual bool RequiresSave() const override { return false; }
};
