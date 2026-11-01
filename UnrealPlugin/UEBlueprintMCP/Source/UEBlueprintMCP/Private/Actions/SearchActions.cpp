// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/SearchActions.h"
#include "MCPCommonUtils.h"
#include "MCPCompat.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "HAL/PlatformTime.h"

namespace
{
	struct BPSearch_FOptions
	{
		FString Query;
		ESearchCase::Type Case = ESearchCase::IgnoreCase;
		bool bNodes = true;
		bool bPinDefaults = true;
		bool bPins = false;
		bool bVariables = true;
		bool bGraphs = true;

		bool Matches(const FString& Text) const
		{
			return !Text.IsEmpty() && Text.Contains(Query, Case);
		}
	};

	bool BPSearch_ParseScope(const TArray<TSharedPtr<FJsonValue>>& ScopeValues, BPSearch_FOptions& Options, FString& OutError)
	{
		Options.bNodes = Options.bPinDefaults = Options.bPins = Options.bVariables = Options.bGraphs = false;
		for (const TSharedPtr<FJsonValue>& Value : ScopeValues)
		{
			FString Entry;
			if (!Value.IsValid() || !Value->TryGetString(Entry))
			{
				OutError = TEXT("'scope' must be an array of strings");
				return false;
			}
			Entry = Entry.ToLower();
			if (Entry == TEXT("nodes")) Options.bNodes = true;
			else if (Entry == TEXT("pin_defaults")) Options.bPinDefaults = true;
			else if (Entry == TEXT("pins")) Options.bPins = true;
			else if (Entry == TEXT("variables")) Options.bVariables = true;
			else if (Entry == TEXT("graphs")) Options.bGraphs = true;
			else
			{
				OutError = FString::Printf(TEXT("Unknown scope '%s'. Use nodes, pin_defaults, pins, variables, graphs."), *Entry);
				return false;
			}
		}
		if (!Options.bNodes && !Options.bPinDefaults && !Options.bPins && !Options.bVariables && !Options.bGraphs)
		{
			OutError = TEXT("'scope' must contain at least one entry");
			return false;
		}
		return true;
	}

	/** Every graph of a Blueprint, including collapsed sub-graphs. */
	void BPSearch_CollectGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
	{
		TArray<UEdGraph*> Roots;
		Roots.Append(Blueprint->UbergraphPages);
		Roots.Append(Blueprint->FunctionGraphs);
		Roots.Append(Blueprint->MacroGraphs);
		Roots.Append(Blueprint->DelegateSignatureGraphs);
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			Roots.Append(Interface.Graphs);
		}

		TSet<UEdGraph*> Seen;
		for (UEdGraph* Root : Roots)
		{
			if (!Root || Seen.Contains(Root)) continue;
			Seen.Add(Root);
			OutGraphs.Add(Root);

			TArray<UEdGraph*> Children;
			Root->GetAllChildrenGraphs(Children);
			for (UEdGraph* Child : Children)
			{
				if (Child && !Seen.Contains(Child))
				{
					Seen.Add(Child);
					OutGraphs.Add(Child);
				}
			}
		}
	}

	/** Collect which aspects of a node match the query. */
	void BPSearch_MatchNode(UEdGraphNode* Node, const BPSearch_FOptions& Options, TArray<FString>& OutKinds)
	{
		if (Options.bNodes)
		{
			if (Options.Matches(Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()))
			{
				OutKinds.Add(TEXT("title"));
			}
			if (Options.Matches(Node->NodeComment))
			{
				OutKinds.Add(TEXT("comment"));
			}

			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (Options.Matches(CallNode->FunctionReference.GetMemberName().ToString()))
				{
					OutKinds.Add(TEXT("function"));
				}
				UFunction* Function = CallNode->GetTargetFunction();
				UClass* OwnerClass = Function ? Function->GetOwnerClass() : nullptr;
				if (!OwnerClass && !CallNode->FunctionReference.IsSelfContext())
				{
					OwnerClass = CallNode->FunctionReference.GetMemberParentClass();
				}
				if (OwnerClass && Options.Matches(OwnerClass->GetName()))
				{
					OutKinds.Add(TEXT("function_owner"));
				}
			}
			else if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
			{
				if (Options.Matches(VarNode->GetVarName().ToString()))
				{
					OutKinds.Add(TEXT("variable"));
				}
				if (!VarNode->VariableReference.IsSelfContext())
				{
					UClass* SourceClass = VarNode->GetVariableSourceClass();
					if (SourceClass && Options.Matches(SourceClass->GetName()))
					{
						OutKinds.Add(TEXT("variable_owner"));
					}
				}
			}
			else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				if (Options.Matches(CustomEvent->CustomFunctionName.ToString()))
				{
					OutKinds.Add(TEXT("event"));
				}
			}
			else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				if (Options.Matches(EventNode->EventReference.GetMemberName().ToString()))
				{
					OutKinds.Add(TEXT("event"));
				}
			}
			else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
			{
				UEdGraph* MacroGraph = MacroNode->GetMacroGraph();
				if (MacroGraph && Options.Matches(MacroGraph->GetName()))
				{
					OutKinds.Add(TEXT("macro"));
				}
			}
			else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				if (CastNode->TargetType && Options.Matches(CastNode->TargetType->GetName()))
				{
					OutKinds.Add(TEXT("cast_target"));
				}
			}
		}

		if (!Options.bPinDefaults && !Options.bPins)
		{
			return;
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;

			if (Options.bPins && !Pin->bHidden)
			{
				if (Options.Matches(Pin->PinName.ToString()))
				{
					OutKinds.AddUnique(FString::Printf(TEXT("pin:%s"), *Pin->PinName.ToString()));
				}
				const UObject* TypeObject = Pin->PinType.PinSubCategoryObject.Get();
				if (TypeObject && Options.Matches(TypeObject->GetName()))
				{
					OutKinds.AddUnique(FString::Printf(TEXT("pin_type:%s"), *Pin->PinName.ToString()));
				}
			}

			if (Options.bPinDefaults && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0)
			{
				const bool bMatch = Options.Matches(Pin->DefaultValue)
					|| Options.Matches(Pin->DefaultTextValue.ToString())
					|| (Pin->DefaultObject && Options.Matches(Pin->DefaultObject->GetName()));
				if (bMatch)
				{
					OutKinds.AddUnique(FString::Printf(TEXT("pin_default:%s"), *Pin->PinName.ToString()));
				}
			}
		}
	}
}

bool FFindInBlueprintsAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Query;
	if (!GetRequiredString(Params, TEXT("query"), Query, OutError)) return false;

	const FString Path = GetOptionalString(Params, TEXT("path"), TEXT("/Game"));
	if (!Path.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(TEXT("'path' must be a package path starting with '/' (e.g. /Game/Blueprints), got '%s'"), *Path);
		return false;
	}

	if (const TArray<TSharedPtr<FJsonValue>>* Scope = GetOptionalArray(Params, TEXT("scope")))
	{
		BPSearch_FOptions Dummy;
		if (!BPSearch_ParseScope(*Scope, Dummy, OutError)) return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FFindInBlueprintsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const double StartTime = FPlatformTime::Seconds();

	BPSearch_FOptions Options;
	Options.Query = Params->GetStringField(TEXT("query"));
	if (GetOptionalBool(Params, TEXT("case_sensitive"), false))
	{
		Options.Case = ESearchCase::CaseSensitive;
	}
	if (const TArray<TSharedPtr<FJsonValue>>* Scope = GetOptionalArray(Params, TEXT("scope")))
	{
		FString Error;
		BPSearch_ParseScope(*Scope, Options, Error);
	}

	const FString Path = GetOptionalString(Params, TEXT("path"), TEXT("/Game"));
	const int32 Limit = FMath::Max(1, (int32)GetOptionalNumber(Params, TEXT("limit"), 50));
	const int32 Offset = FMath::Max(0, (int32)GetOptionalNumber(Params, TEXT("offset"), 0));
	const double Budget = FMath::Clamp(GetOptionalNumber(Params, TEXT("time_budget_seconds"), 10.0), 1.0, 18.0);

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(FName(*Path));
	MCPCompat::AddFilterClass(Filter, UBlueprint::StaticClass());

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 TotalMatches = 0;
	int32 Scanned = 0;
	int32 LoadFailed = 0;
	int32 Index = Offset;

	for (; Index < Assets.Num(); ++Index)
	{
		if (Scanned > 0 && (TotalMatches >= Limit || FPlatformTime::Seconds() - StartTime >= Budget))
		{
			break;
		}

		UBlueprint* Blueprint = Cast<UBlueprint>(Assets[Index].GetAsset());
		++Scanned;
		if (!Blueprint)
		{
			++LoadFailed;
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> NodeMatches;
		TArray<TSharedPtr<FJsonValue>> GraphMatches;
		TArray<TSharedPtr<FJsonValue>> VariableMatches;

		TArray<UEdGraph*> Graphs;
		BPSearch_CollectGraphs(Blueprint, Graphs);

		for (UEdGraph* Graph : Graphs)
		{
			if (Options.bGraphs && Options.Matches(Graph->GetName()))
			{
				GraphMatches.Add(MakeShared<FJsonValueString>(Graph->GetName()));
			}

			if (!Options.bNodes && !Options.bPinDefaults && !Options.bPins)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				TArray<FString> Kinds;
				BPSearch_MatchNode(Node, Options, Kinds);
				if (Kinds.Num() == 0) continue;

				TSharedPtr<FJsonObject> NodeJson = FMCPCommonUtils::NodeToJson(Node, false);
				NodeJson->SetStringField(TEXT("graph"), Graph->GetName());
				TArray<TSharedPtr<FJsonValue>> KindValues;
				for (const FString& Kind : Kinds)
				{
					KindValues.Add(MakeShared<FJsonValueString>(Kind));
				}
				NodeJson->SetArrayField(TEXT("matched_on"), KindValues);
				NodeMatches.Add(MakeShared<FJsonValueObject>(NodeJson));
			}
		}

		if (Options.bVariables)
		{
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				const UObject* TypeObject = Var.VarType.PinSubCategoryObject.Get();
				const bool bNameMatch = Options.Matches(Var.VarName.ToString());
				const bool bTypeMatch = TypeObject && Options.Matches(TypeObject->GetName());
				if (!bNameMatch && !bTypeMatch) continue;

				TSharedPtr<FJsonObject> VarJson = MakeShared<FJsonObject>();
				VarJson->SetStringField(TEXT("name"), Var.VarName.ToString());
				FString TypeText;
				if (TSharedPtr<FJsonObject> TypeJson = FMCPCommonUtils::PinTypeToJson(Var.VarType))
				{
					TypeJson->TryGetStringField(TEXT("type"), TypeText);
				}
				VarJson->SetStringField(TEXT("type"), TypeText);
				VarJson->SetStringField(TEXT("matched_on"), bNameMatch ? TEXT("name") : TEXT("type"));
				VariableMatches.Add(MakeShared<FJsonValueObject>(VarJson));
			}
		}

		const int32 BlueprintMatches = NodeMatches.Num() + GraphMatches.Num() + VariableMatches.Num();
		if (BlueprintMatches == 0) continue;

		TotalMatches += BlueprintMatches;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("blueprint"), Assets[Index].PackageName.ToString());
		Entry->SetStringField(TEXT("name"), Assets[Index].AssetName.ToString());
		Entry->SetNumberField(TEXT("match_count"), BlueprintMatches);
		if (GraphMatches.Num() > 0) Entry->SetArrayField(TEXT("graphs"), GraphMatches);
		if (VariableMatches.Num() > 0) Entry->SetArrayField(TEXT("variables"), VariableMatches);
		if (NodeMatches.Num() > 0) Entry->SetArrayField(TEXT("nodes"), NodeMatches);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	const bool bComplete = Index >= Assets.Num();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("query"), Options.Query);
	ResultData->SetStringField(TEXT("path"), Path);
	ResultData->SetNumberField(TEXT("total_blueprints"), Assets.Num());
	ResultData->SetNumberField(TEXT("scanned"), Scanned);
	ResultData->SetNumberField(TEXT("blueprints_with_matches"), Results.Num());
	ResultData->SetNumberField(TEXT("match_count"), TotalMatches);
	ResultData->SetBoolField(TEXT("complete"), bComplete);
	if (!bComplete)
	{
		ResultData->SetNumberField(TEXT("next_offset"), Index);
	}
	if (LoadFailed > 0)
	{
		ResultData->SetNumberField(TEXT("load_failed"), LoadFailed);
	}
	if (AssetRegistry.IsLoadingAssets())
	{
		ResultData->SetStringField(TEXT("warning"), TEXT("The Asset Registry is still scanning; results may be incomplete."));
	}
	ResultData->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - StartTime);
	ResultData->SetArrayField(TEXT("results"), Results);
	return CreateSuccessResponse(ResultData);
}
