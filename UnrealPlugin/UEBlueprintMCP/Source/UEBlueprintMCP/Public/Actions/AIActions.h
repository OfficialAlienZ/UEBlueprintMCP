// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FGetBehaviorTreeInfoAction
 *
 * Read-only introspection of a Behavior Tree (or Blackboard) asset.
 *
 * Parameters:
 *   - asset_name: Behavior Tree or Blackboard name, or full asset path (e.g. /Game/AI/BT_Enemy)
 *   - include_properties (optional, default true): Include node properties that differ from
 *       the node class defaults (and decorator abort mode / inverse flag)
 *
 * Returns for a Behavior Tree: the Blackboard (keys with types), root decorators and the
 * full node tree. Each node carries its class, display name, static description, decorators,
 * services and ordered children (left to right = execution order). For a Blackboard asset
 * only the blackboard section is returned.
 */
class UEBLUEPRINTMCP_API FGetBehaviorTreeInfoAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_behavior_tree_info"); }
	virtual bool RequiresSave() const override { return false; }
};
