// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/StructNodeActions.h"
#include "MCPCommonUtils.h"
#include "MCPInspectionUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/UserDefinedStruct.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_GetDataTableRow.h"
#include "UObject/UObjectIterator.h"

namespace
{
	UScriptStruct* Struct_Resolve(const FString& NameOrPath)
	{
		return FMCPInspectionUtils::FindStruct(NameOrPath);
	}

	TArray<TSharedPtr<FJsonValue>> Struct_DescribePins(const UEdGraphNode* Node)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden || Pin->ParentPin) continue;
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Pin->PinName.ToString());
			Entry->SetStringField(TEXT("display_name"), Node->GetPinDisplayName(Pin).ToString());
			Entry->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			Entry->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString());
			Result.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Result;
	}
}

// ============================================================================
// add_struct_node (Break / Make Struct)
// ============================================================================

bool FAddStructNodeAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString StructName;
	if (!GetRequiredString(Params, TEXT("struct_type"), StructName, OutError)) return false;

	const FString Mode = GetOptionalString(Params, TEXT("mode"), TEXT("break"));
	if (!Mode.Equals(TEXT("break"), ESearchCase::IgnoreCase) && !Mode.Equals(TEXT("make"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Invalid mode '%s'. Use 'break' or 'make'."), *Mode);
		return false;
	}
	return ValidateGraph(Params, Context, OutError);
}

TSharedPtr<FJsonObject> FAddStructNodeAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString StructName = Params->GetStringField(TEXT("struct_type"));
	const bool bMake = GetOptionalString(Params, TEXT("mode"), TEXT("break")).Equals(TEXT("make"), ESearchCase::IgnoreCase);

	UScriptStruct* Struct = Struct_Resolve(StructName);
	if (!Struct)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Struct '%s' not found. Pass a User Defined Struct name/path (S_Item, /Game/Data/S_Item) or a native struct (Vector, /Script/Engine.HitResult)."), *StructName), TEXT("not_found"));
	}

	if (bMake)
	{
		if (!UK2Node_MakeStruct::CanBeMade(Struct))
		{
			return CreateErrorResponse(FString::Printf(TEXT("Struct '%s' cannot be made with a Make Struct node (not BlueprintType, or it has a native Make function: use add_blueprint_function_node)."), *Struct->GetName()), TEXT("validation_failed"));
		}
	}
	else if (Struct->HasMetaData(TEXT("HasNativeBreak")))
	{
		return CreateErrorResponse(FString::Printf(TEXT("Struct '%s' has a native Break function (e.g. BreakVector): use add_blueprint_function_node instead."), *Struct->GetName()), TEXT("validation_failed"));
	}

	UBlueprint* Blueprint = GetTargetBlueprint(Params, Context);
	UEdGraph* TargetGraph = GetTargetGraph(Params, Context);
	const FVector2D Position = GetNodePosition(Params);

	UEdGraphNode* Node = nullptr;
	if (bMake)
	{
		Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_MakeStruct>(
			TargetGraph, Position, EK2NewNodeFlags::None,
			[Struct](UK2Node_MakeStruct* NewNode)
			{
				NewNode->StructType = Struct;
				NewNode->bMadeAfterOverridePinRemoval = true;
			});
	}
	else
	{
		Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_BreakStruct>(
			TargetGraph, Position, EK2NewNodeFlags::None,
			[Struct](UK2Node_BreakStruct* NewNode)
			{
				NewNode->StructType = Struct;
				NewNode->bMadeAfterOverridePinRemoval = true;
			});
	}

	if (!Node)
	{
		return CreateErrorResponse(TEXT("Failed to create the struct node"), TEXT("execution_failed"));
	}

	int32 DataPins = 0;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && !Pin->bHidden) ++DataPins;
	}
	if (DataPins == 0)
	{
		TargetGraph->RemoveNode(Node);
		return CreateErrorResponse(FString::Printf(TEXT("Struct '%s' exposes no Blueprint-visible members; node removed."), *Struct->GetName()), TEXT("validation_failed"));
	}

	MarkBlueprintModified(Blueprint, Context);
	RegisterCreatedNode(Node, Context);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	ResultData->SetStringField(TEXT("struct"), Struct->GetPathName());
	ResultData->SetStringField(TEXT("mode"), bMake ? TEXT("make") : TEXT("break"));
	ResultData->SetArrayField(TEXT("pins"), Struct_DescribePins(Node));
	return CreateSuccessResponse(ResultData);
}

// ============================================================================
// add_get_data_table_row_node
// ============================================================================

bool FAddGetDataTableRowNodeAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (GetOptionalString(Params, TEXT("row_name")).Len() > 0 && GetOptionalString(Params, TEXT("data_table")).IsEmpty())
	{
		OutError = TEXT("'row_name' requires 'data_table' so the row can be checked");
		return false;
	}
	return ValidateGraph(Params, Context, OutError);
}

TSharedPtr<FJsonObject> FAddGetDataTableRowNodeAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString TableName = GetOptionalString(Params, TEXT("data_table"));
	const FString RowName = GetOptionalString(Params, TEXT("row_name"));

	UDataTable* Table = nullptr;
	if (!TableName.IsEmpty())
	{
		TArray<UClass*> Classes;
		Classes.Add(UDataTable::StaticClass());
		Classes.Add(UCompositeDataTable::StaticClass());
		Table = Cast<UDataTable>(FMCPInspectionUtils::FindAsset(TableName, Classes));
		if (!Table)
		{
			return CreateErrorResponse(FString::Printf(TEXT("Data Table '%s' not found. Pass the asset name or a full path such as /Game/Data/DT_Items."), *TableName), TEXT("not_found"));
		}
		if (!Table->GetRowStruct())
		{
			return CreateErrorResponse(FString::Printf(TEXT("Data Table '%s' has no row struct assigned."), *TableName), TEXT("validation_failed"));
		}
		if (!RowName.IsEmpty() && !Table->GetRowMap().Contains(FName(*RowName)))
		{
			return CreateErrorResponse(FString::Printf(TEXT("Row '%s' not found in '%s'. Use get_data_table_info with include_rows=false to list the row names."), *RowName, *TableName), TEXT("not_found"));
		}
	}

	UBlueprint* Blueprint = GetTargetBlueprint(Params, Context);
	UEdGraph* TargetGraph = GetTargetGraph(Params, Context);

	UK2Node_GetDataTableRow* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_GetDataTableRow>(
		TargetGraph, GetNodePosition(Params), EK2NewNodeFlags::None);
	if (!Node)
	{
		return CreateErrorResponse(TEXT("Failed to create the Get Data Table Row node"), TEXT("execution_failed"));
	}

	if (Table)
	{
		if (UEdGraphPin* TablePin = Node->GetDataTablePin())
		{
			TablePin->DefaultObject = Table;
			// Types the Result pin from the table's row struct and picks a default row.
			Node->PinDefaultValueChanged(TablePin);
		}
		if (!RowName.IsEmpty())
		{
			if (UEdGraphPin* RowPin = Node->GetRowNamePin())
			{
				RowPin->DefaultValue = RowName;
			}
		}
	}

	MarkBlueprintModified(Blueprint, Context);
	RegisterCreatedNode(Node, Context);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	if (const UScriptStruct* RowStruct = Node->GetDataTableRowStructType())
	{
		ResultData->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());
	}
	if (const UEdGraphPin* RowPin = Node->GetRowNamePin())
	{
		ResultData->SetStringField(TEXT("row_name"), RowPin->DefaultValue);
	}
	ResultData->SetArrayField(TEXT("pins"), Struct_DescribePins(Node));
	return CreateSuccessResponse(ResultData);
}
