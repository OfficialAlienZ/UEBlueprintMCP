// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FNiagaraParamsAction
 *
 * Read or edit the module inputs of a full (non-lightweight) Niagara system, e.g. Lifetime in
 * Initialize Particle or the speed in Add Velocity. The values live in the "rapid iteration" stores of the
 * system/emitter scripts, named like "Constants.<Emitter>.<Module>.<Input>".
 *
 * Parameters:
 *   - asset_path: Niagara system (e.g. /Game/TopDown/.../NS_BarrelDust)
 *   - filter (optional): case-insensitive substring of the parameter name (e.g. "Lifetime", "Velocity")
 *   - set (optional): object parameter name -> value. The name must match exactly one parameter
 *       (full name or unique substring). Numbers for float/int/bool, arrays for vectors/colors ([x,y,z], [r,g,b,a]).
 *   - limit (optional, default 60): max parameters listed
 *
 * Result: "params" = compact lines "Name=value" (only float/int/bool/vector/color types are shown), "set" = changed lines.
 */
class UEBLUEPRINTMCP_API FNiagaraParamsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("niagara_params"); }
};
