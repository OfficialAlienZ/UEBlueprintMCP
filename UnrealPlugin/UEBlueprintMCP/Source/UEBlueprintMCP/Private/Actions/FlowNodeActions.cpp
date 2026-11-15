// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/FlowNodeActions.h"
#include "MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node_SwitchString.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"

namespace
{
	UEdGraphNode* Flow_FindNodeByGuid(UEdGraph* Graph, const FString& NodeId)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
		return nullptr;
	}
}

// ============================================================================
// add_switch_string_case
// ============================================================================

bool FAddSwitchStringCaseAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString NodeId, CaseValue;
	if (!GetRequiredString(Params, TEXT("node_id"), NodeId, OutError)) return false;
	if (!GetRequiredString(Params, TEXT("case_value"), CaseValue, OutError)) return false;
	if (CaseValue.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("'case_value' must not be blank");
		return false;
	}
	return ValidateGraph(Params, Context, OutError);
}

TSharedPtr<FJsonObject> FAddSwitchStringCaseAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString NodeId = Params->GetStringField(TEXT("node_id"));
	const FString CaseValue = Params->GetStringField(TEXT("case_value"));

	UBlueprint* Blueprint = GetTargetBlueprint(Params, Context);
	UEdGraph* TargetGraph = GetTargetGraph(Params, Context);

	UK2Node_SwitchString* SwitchNode = Cast<UK2Node_SwitchString>(Flow_FindNodeByGuid(TargetGraph, NodeId));
	if (!SwitchNode)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Switch on String node not found with ID: %s. Check node_id and graph_name."), *NodeId), TEXT("not_found"));
	}

	// Case values are compared the same way the switch itself compares them.
	const ESearchCase::Type Sensitivity = SwitchNode->bIsCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
	for (const FName& Existing : SwitchNode->PinNames)
	{
		if (Existing.ToString().Equals(CaseValue, Sensitivity))
		{
			return CreateErrorResponse(FString::Printf(TEXT("Case '%s' already exists on this switch."), *CaseValue), TEXT("validation_failed"));
		}
	}

	const FName CaseName(*CaseValue);
	if (SwitchNode->FindPin(CaseName))
	{
		return CreateErrorResponse(FString::Printf(TEXT("Case '%s' clashes with an existing pin on this switch (e.g. Default or Selection)."), *CaseValue), TEXT("validation_failed"));
	}

	// Add the case to PinNames, then rebuild the pins. Reconstruction recreates every case pin from
	// PinNames and keeps the wires of existing pins, which is what the Details panel does on edit.
	SwitchNode->PinNames.Add(CaseName);
	SwitchNode->ReconstructNode();
	TargetGraph->NotifyGraphChanged();

	if (!SwitchNode->FindPin(CaseName))
	{
		return CreateErrorResponse(TEXT("Case pin was not created"), TEXT("execution_failed"));
	}

	MarkBlueprintModified(Blueprint, Context);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("node_id"), SwitchNode->NodeGuid.ToString());
	ResultData->SetStringField(TEXT("case_pin"), CaseValue);

	TArray<TSharedPtr<FJsonValue>> Cases;
	for (const FName& Name : SwitchNode->PinNames)
	{
		Cases.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}
	ResultData->SetArrayField(TEXT("cases"), Cases);
	return CreateSuccessResponse(ResultData);
}

// ============================================================================
// add_return_node
// ============================================================================

bool FAddReturnNodeAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString GraphName;
	if (!GetRequiredString(Params, TEXT("graph_name"), GraphName, OutError)) return false;
	return ValidateGraph(Params, Context, OutError);
}

TSharedPtr<FJsonObject> FAddReturnNodeAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UBlueprint* Blueprint = GetTargetBlueprint(Params, Context);
	UEdGraph* TargetGraph = GetTargetGraph(Params, Context);

	// Return nodes only make sense in a function graph (the graph must have a function entry).
	bool bIsFunctionGraph = false;
	for (const UEdGraphNode* Node : TargetGraph->Nodes)
	{
		if (Node && Node->IsA<UK2Node_FunctionEntry>())
		{
			bIsFunctionGraph = true;
			break;
		}
	}
	if (!bIsFunctionGraph)
	{
		return CreateErrorResponse(TEXT("add_return_node needs a function graph: pass graph_name of a function"), TEXT("validation_failed"));
	}

	// Spawning runs PostPlacedNewNode, which syncs the signature (output pins) with the function entry.
	UK2Node_FunctionResult* ResultNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_FunctionResult>(
		TargetGraph, GetNodePosition(Params), EK2NewNodeFlags::None);
	if (!ResultNode)
	{
		return CreateErrorResponse(TEXT("Failed to create the Return node"), TEXT("execution_failed"));
	}

	MarkBlueprintModified(Blueprint, Context);
	RegisterCreatedNode(ResultNode, Context);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("node_id"), ResultNode->NodeGuid.ToString());

	TArray<TSharedPtr<FJsonValue>> PinNames;
	for (const UEdGraphPin* Pin : ResultNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && !Pin->bHidden)
		{
			PinNames.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString()));
		}
	}
	ResultData->SetArrayField(TEXT("pins"), PinNames);
	return CreateSuccessResponse(ResultData);
}
