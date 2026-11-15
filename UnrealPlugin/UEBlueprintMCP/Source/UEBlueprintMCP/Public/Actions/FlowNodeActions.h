// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FAddSwitchStringCaseAction
 *
 * Add a case to an existing Switch on String node (K2Node_SwitchString). The case value is stored in
 * the node's PinNames and becomes the name of a new exec output pin. Existing pins and their wires are
 * kept (the node is reconstructed in place).
 *
 * Parameters:
 *   - blueprint_name: Target Blueprint
 *   - node_id: GUID of the Switch on String node
 *   - case_value: Case string. Must not match an existing case or pin
 *       (case-insensitive unless the switch is case sensitive)
 *   - graph_name (optional): Function graph name (defaults to event graph)
 *
 * Returns node_id, case_pin (the new pin name) and cases (all case values in order).
 */
class UEBLUEPRINTMCP_API FAddSwitchStringCaseAction : public FBlueprintNodeAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("add_switch_string_case"); }
};


/**
 * FAddReturnNodeAction
 *
 * Add a Return Node (K2Node_FunctionResult) to a function graph. The node takes the function's output
 * pins. Set the output values with set_node_pin_default (object pins take asset paths)
 * and wire the exec input from the previous node with connect_blueprint_nodes.
 *
 * Parameters:
 *   - blueprint_name: Target Blueprint
 *   - graph_name: Function graph name (required: Return nodes only exist in function graphs)
 *   - node_position (optional)
 *
 * Returns node_id and pins (input pin names).
 */
class UEBLUEPRINTMCP_API FAddReturnNodeAction : public FBlueprintNodeAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("add_return_node"); }
};
