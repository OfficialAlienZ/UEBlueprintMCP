// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/UMGActions.h"
#include "MCPCommonUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "WidgetBlueprint.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// =============================================================================
// FSetTextBlockBindingAction
//
// Creates the same Designer binding as the "Bind" dropdown in the Details panel:
// a pure function on the Widget Blueprint (signature taken from the widget's
// <Property>Delegate) plus an FDelegateEditorBinding entry. No member variable is created.
// =============================================================================

bool FSetTextBlockBindingAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Value;
	if (!GetRequiredString(Params, TEXT("widget_name"), Value, OutError))
	{
		return false;
	}
	if (!GetRequiredString(Params, TEXT("text_block_name"), Value, OutError))
	{
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FSetTextBlockBindingAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	const FString WidgetName = Params->GetStringField(TEXT("text_block_name"));
	FString PropertyName = GetOptionalString(Params, TEXT("binding_type"), TEXT("Text"));
	PropertyName.RemoveFromEnd(TEXT("Delegate"));
	// 'function_name' is the function to bind (created when missing); 'binding_property' is the legacy alias.
	FString FunctionName = GetOptionalString(Params, TEXT("function_name"));
	if (FunctionName.IsEmpty())
	{
		FunctionName = GetOptionalString(Params, TEXT("binding_property"));
	}
	const bool bRemove = GetOptionalBool(Params, TEXT("remove"), false);

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName), TEXT("not_found"));
	}

	UWidget* Widget = WidgetBlueprint->WidgetTree ? WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	if (!Widget)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Widget '%s' not found in '%s'"), *WidgetName, *BlueprintName), TEXT("not_found"));
	}

	// Bindable properties are the ones with a matching "<Name>Delegate" member.
	FDelegateProperty* DelegateProp = FindFProperty<FDelegateProperty>(Widget->GetClass(), FName(*(PropertyName + TEXT("Delegate"))));
	if (!DelegateProp || !DelegateProp->SignatureFunction)
	{
		TArray<FString> Bindable;
		for (TFieldIterator<FDelegateProperty> It(Widget->GetClass()); It; ++It)
		{
			FString Name = It->GetName();
			if (Name.RemoveFromEnd(TEXT("Delegate")))
			{
				Bindable.Add(Name);
			}
		}
		return CreateErrorResponse(FString::Printf(TEXT("Widget '%s' (%s) has no bindable property '%s'. Bindable: %s"),
			*WidgetName, *Widget->GetClass()->GetName(), *PropertyName, *FString::Join(Bindable, TEXT(", "))), TEXT("validation_failed"));
	}

	// An existing binding for this widget/property is replaced (the Designer allows one per property).
	FDelegateEditorBinding Binding;
	Binding.ObjectName = Widget->GetName();
	Binding.PropertyName = FName(*PropertyName);

	if (bRemove)
	{
		WidgetBlueprint->Modify();
		if (WidgetBlueprint->Bindings.Remove(Binding) == 0)
		{
			return CreateErrorResponse(FString::Printf(TEXT("'%s.%s' has no binding to remove"), *WidgetName, *PropertyName), TEXT("not_found"));
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		TSharedPtr<FJsonObject> Removed = MakeShared<FJsonObject>();
		Removed->SetStringField(TEXT("widget"), WidgetName);
		Removed->SetStringField(TEXT("property"), PropertyName);
		Removed->SetBoolField(TEXT("removed"), true);
		return CreateSuccessResponse(Removed);
	}

	WidgetBlueprint->Modify();

	// Reuse a function of that name when it exists, otherwise create a pure one with the delegate's signature.
	UEdGraph* FunctionGraph = nullptr;
	if (!FunctionName.IsEmpty())
	{
		for (UEdGraph* Graph : WidgetBlueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == FName(*FunctionName))
			{
				FunctionGraph = Graph;
				break;
			}
		}
	}

	bool bCreated = false;
	if (!FunctionGraph)
	{
		const FString BaseName = FunctionName.IsEmpty() ? FString::Printf(TEXT("Get_%s_%s"), *WidgetName, *PropertyName) : FunctionName;
		FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
			WidgetBlueprint,
			FBlueprintEditorUtils::FindUniqueKismetName(WidgetBlueprint, BaseName),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!FunctionGraph)
		{
			return CreateErrorResponse(TEXT("Failed to create binding function graph"), TEXT("execution_failed"));
		}
		FBlueprintEditorUtils::AddFunctionGraph(WidgetBlueprint, FunctionGraph, /*bIsUserCreated*/ true, DelegateProp->SignatureFunction);
		bCreated = true;
	}

	UFunction* Function = WidgetBlueprint->SkeletonGeneratedClass
		? WidgetBlueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionGraph->GetFName())
		: nullptr;
	if (!Function)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Function '%s' was not found on the Widget Blueprint class"), *FunctionGraph->GetName()), TEXT("not_found"));
	}
	if (!bCreated && !Function->IsSignatureCompatibleWith(DelegateProp->SignatureFunction, UFunction::GetDefaultIgnoredSignatureCompatibilityFlags() | CPF_ReturnParm))
	{
		return CreateErrorResponse(FString::Printf(TEXT("Function '%s' does not match the signature of %s.%s (no inputs, one return value of the property type)"),
			*FunctionGraph->GetName(), *Widget->GetClass()->GetName(), *PropertyName), TEXT("validation_failed"));
	}

	TArray<FFieldVariant> BindingChain;
	BindingChain.Add(FFieldVariant(Function));
	Binding.SourcePath = FEditorPropertyPath(BindingChain);
	Binding.FunctionName = Function->GetFName();
	UBlueprint::GetGuidFromClassByFieldName<UFunction>(Function->GetOwnerClass(), Function->GetFName(), Binding.MemberGuid);
	Binding.Kind = EBindingKind::Function;

	WidgetBlueprint->Bindings.Remove(Binding);
	WidgetBlueprint->Bindings.AddUnique(Binding);

	if (bCreated)
	{
		if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(FunctionGraph->GetSchema()))
		{
			K2Schema->AddExtraFunctionFlags(FunctionGraph, FUNC_BlueprintPure);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("widget"), WidgetName);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
	Result->SetBoolField(TEXT("created_function"), bCreated);
	for (UEdGraphNode* Node : FunctionGraph->Nodes)
	{
		if (Cast<UK2Node_FunctionEntry>(Node))
		{
			Result->SetStringField(TEXT("entry_node_id"), Node->NodeGuid.ToString());
		}
		else if (Cast<UK2Node_FunctionResult>(Node))
		{
			Result->SetStringField(TEXT("result_node_id"), Node->NodeGuid.ToString());
		}
	}
	if (bCreated)
	{
		Result->SetStringField(TEXT("note"), TEXT("The function body is empty (returns the default value). Add nodes with graph_name set to function_name and wire them into the result node's 'ReturnValue' pin."));
	}
	return CreateSuccessResponse(Result);
}
