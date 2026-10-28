// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/AIActions.h"
#include "MCPCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "UObject/UnrealType.h"

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> AI_FJsonArray;

	TSharedPtr<FJsonValue> AI_AsValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	/** Editable properties of Object whose value differs from its class default object. */
	TSharedPtr<FJsonObject> AI_ChangedProperties(UObject* Object, const UClass* ExcludeOwner)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		UObject* Baseline = Object ? Object->GetClass()->GetDefaultObject() : nullptr;
		if (!Baseline)
		{
			return Result;
		}

		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}
			if (ExcludeOwner && Property->GetOwnerClass() == ExcludeOwner)
			{
				continue;
			}
			if (Property->Identical_InContainer(Object, Baseline))
			{
				continue;
			}
			FString Text;
			Property->ExportText_InContainer(0, Text, Object, nullptr, nullptr, PPF_None);
			Result->SetStringField(Property->GetName(), Text);
		}
		return Result;
	}

	/** Export a single named property even when it equals the default. */
	void AI_ExportNamedProperty(UObject* Object, const TCHAR* PropertyName, const TSharedPtr<FJsonObject>& Target)
	{
		const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return;
		}
		FString Text;
		Property->ExportText_InContainer(0, Text, Object, nullptr, nullptr, PPF_None);
		Target->SetStringField(PropertyName, Text);
	}

	/** Class name plus, for Blueprint-defined nodes, the owning Blueprint asset path. */
	void AI_AddClassInfo(const UObject* Object, const TSharedPtr<FJsonObject>& Target)
	{
		const UClass* Class = Object->GetClass();
		Target->SetStringField(TEXT("class"), Class->GetName());
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
		{
			Target->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		}
	}

	const TCHAR* AI_LogicOpToString(EBTDecoratorLogic::Type Op)
	{
		switch (Op)
		{
		case EBTDecoratorLogic::Test: return TEXT("Test");
		case EBTDecoratorLogic::And: return TEXT("And");
		case EBTDecoratorLogic::Or: return TEXT("Or");
		case EBTDecoratorLogic::Not: return TEXT("Not");
		default: return TEXT("Invalid");
		}
	}

	TSharedPtr<FJsonObject> AI_NodeBaseToJson(UBTNode* Node, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		AI_AddClassInfo(Node, Result);
		Result->SetStringField(TEXT("name"), Node->GetNodeName());
		const FString Description = Node->GetStaticDescription();
		if (!Description.IsEmpty())
		{
			Result->SetStringField(TEXT("description"), Description);
		}
		if (bIncludeProperties)
		{
			// NodeName is reported as "name"; everything else that differs from the class defaults is listed here.
			Result->SetObjectField(TEXT("properties"), AI_ChangedProperties(Node, UBTNode::StaticClass()));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> AI_DecoratorToJson(UBTDecorator* Decorator, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> Result = AI_NodeBaseToJson(Decorator, bIncludeProperties);
		Result->SetStringField(TEXT("type"), TEXT("decorator"));
		Result->SetBoolField(TEXT("inverse_condition"), Decorator->IsInversed());
		Result->SetStringField(TEXT("flow_abort_mode"), StaticEnum<EBTFlowAbortMode::Type>()->GetNameStringByValue((int64)Decorator->GetFlowAbortMode()));
		return Result;
	}

	TSharedPtr<FJsonObject> AI_ServiceToJson(UBTService* Service, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> Result = AI_NodeBaseToJson(Service, bIncludeProperties);
		Result->SetStringField(TEXT("type"), TEXT("service"));
		AI_ExportNamedProperty(Service, TEXT("Interval"), Result);
		AI_ExportNamedProperty(Service, TEXT("RandomDeviation"), Result);
		return Result;
	}

	AI_FJsonArray AI_DecoratorsToJson(const TArray<UBTDecorator*>& Decorators, bool bIncludeProperties)
	{
		AI_FJsonArray Result;
		for (UBTDecorator* Decorator : Decorators)
		{
			if (Decorator) Result.Add(AI_AsValue(AI_DecoratorToJson(Decorator, bIncludeProperties)));
		}
		return Result;
	}

	AI_FJsonArray AI_DecoratorOpsToJson(const TArray<FBTDecoratorLogic>& Ops)
	{
		AI_FJsonArray Result;
		for (const FBTDecoratorLogic& Op : Ops)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("op"), AI_LogicOpToString(Op.Operation));
			Entry->SetNumberField(TEXT("number"), Op.Number);
			Result.Add(AI_AsValue(Entry));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> AI_CompositeToJson(UBTCompositeNode* Composite, bool bIncludeProperties, int32 Depth);

	TSharedPtr<FJsonObject> AI_TaskToJson(UBTTaskNode* Task, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> Result = AI_NodeBaseToJson(Task, bIncludeProperties);
		Result->SetStringField(TEXT("type"), TEXT("task"));

		AI_FJsonArray Services;
		for (UBTService* Service : Task->Services)
		{
			if (Service) Services.Add(AI_AsValue(AI_ServiceToJson(Service, bIncludeProperties)));
		}
		if (Services.Num() > 0)
		{
			Result->SetArrayField(TEXT("services"), Services);
		}
		return Result;
	}

	TSharedPtr<FJsonObject> AI_CompositeToJson(UBTCompositeNode* Composite, bool bIncludeProperties, int32 Depth)
	{
		TSharedPtr<FJsonObject> Result = AI_NodeBaseToJson(Composite, bIncludeProperties);
		Result->SetStringField(TEXT("type"), TEXT("composite"));

		AI_FJsonArray Services;
		for (UBTService* Service : Composite->Services)
		{
			if (Service) Services.Add(AI_AsValue(AI_ServiceToJson(Service, bIncludeProperties)));
		}
		Result->SetArrayField(TEXT("services"), Services);

		AI_FJsonArray Children;
		if (Depth > 64)
		{
			Result->SetStringField(TEXT("truncated"), TEXT("max depth reached"));
		}
		else
		{
			for (int32 Index = 0; Index < Composite->Children.Num(); ++Index)
			{
				const FBTCompositeChild& Child = Composite->Children[Index];

				TSharedPtr<FJsonObject> Entry;
				if (Child.ChildComposite)
				{
					Entry = AI_CompositeToJson(Child.ChildComposite, bIncludeProperties, Depth + 1);
				}
				else if (Child.ChildTask)
				{
					Entry = AI_TaskToJson(Child.ChildTask, bIncludeProperties);
				}
				else
				{
					continue;
				}
				Entry->SetNumberField(TEXT("child_index"), Index);
				// Decorators on the connection to this child (conditions guarding it).
				Entry->SetArrayField(TEXT("decorators"), AI_DecoratorsToJson(Child.Decorators, bIncludeProperties));
				if (Child.DecoratorOps.Num() > 0)
				{
					Entry->SetArrayField(TEXT("decorator_logic"), AI_DecoratorOpsToJson(Child.DecoratorOps));
				}
				Children.Add(AI_AsValue(Entry));
			}
		}
		Result->SetArrayField(TEXT("children"), Children);
		return Result;
	}

	FString AI_KeyTypeName(const UBlackboardKeyType* KeyType)
	{
		if (!KeyType)
		{
			return FString();
		}
		FString Name = KeyType->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("BlackboardKeyType_"));
		return Name;
	}

	AI_FJsonArray AI_BlackboardKeysToJson(const TArray<FBlackboardEntry>& Keys, bool bIncludeProperties)
	{
		AI_FJsonArray Result;
		for (const FBlackboardEntry& Key : Keys)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Key.EntryName.ToString());
			Entry->SetStringField(TEXT("type"), AI_KeyTypeName(Key.KeyType));
			if (!Key.EntryCategory.IsNone())
			{
				Entry->SetStringField(TEXT("category"), Key.EntryCategory.ToString());
			}
			Entry->SetBoolField(TEXT("instance_synced"), Key.bInstanceSynced != 0);
			if (bIncludeProperties && Key.KeyType)
			{
				// e.g. BaseClass for Object/Class keys, EnumType for Enum keys.
				Entry->SetObjectField(TEXT("properties"), AI_ChangedProperties(Key.KeyType, nullptr));
			}
			Result.Add(AI_AsValue(Entry));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> AI_BlackboardToJson(UBlackboardData* Blackboard, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Blackboard->GetName());
		Result->SetStringField(TEXT("path"), Blackboard->GetPathName());
		if (Blackboard->Parent)
		{
			Result->SetStringField(TEXT("parent"), Blackboard->Parent->GetPathName());
		}
		Result->SetArrayField(TEXT("keys"), AI_BlackboardKeysToJson(Blackboard->Keys, bIncludeProperties));
		if (Blackboard->Parent)
		{
			Result->SetArrayField(TEXT("inherited_keys"), AI_BlackboardKeysToJson(Blackboard->ParentKeys, bIncludeProperties));
		}
		return Result;
	}

	UObject* AI_ResolveAsset(const FString& AssetName, UClass* Class)
	{
		if (AssetName.StartsWith(TEXT("/")))
		{
			FString ObjectPath = AssetName;
			if (!ObjectPath.Contains(TEXT(".")))
			{
				FString ShortName;
				ObjectPath.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				ObjectPath = ObjectPath + TEXT(".") + ShortName;
			}
			return StaticLoadObject(Class, nullptr, *ObjectPath);
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> AssetList;
		MCPCompat::GetAssetsByClass(AssetRegistryModule.Get(), Class, AssetList);
		for (const FAssetData& AssetData : AssetList)
		{
			if (AssetData.AssetName.ToString() == AssetName)
			{
				return AssetData.GetAsset();
			}
		}
		return nullptr;
	}
}

bool FGetBehaviorTreeInfoAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString AssetName;
	if (!GetRequiredString(Params, TEXT("asset_name"), AssetName, OutError))
	{
		return false;
	}
	if (!AI_ResolveAsset(AssetName, UBehaviorTree::StaticClass()) && !AI_ResolveAsset(AssetName, UBlackboardData::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Behavior Tree or Blackboard '%s' not found. Pass the asset name or a full path such as /Game/AI/BT_Enemy."), *AssetName);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetBehaviorTreeInfoAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString AssetName = GetOptionalString(Params, TEXT("asset_name"));
	const bool bIncludeProperties = GetOptionalBool(Params, TEXT("include_properties"), true);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	UBehaviorTree* Tree = Cast<UBehaviorTree>(AI_ResolveAsset(AssetName, UBehaviorTree::StaticClass()));
	if (!Tree)
	{
		UBlackboardData* Blackboard = Cast<UBlackboardData>(AI_ResolveAsset(AssetName, UBlackboardData::StaticClass()));
		Result->SetStringField(TEXT("asset_type"), TEXT("Blackboard"));
		Result->SetObjectField(TEXT("blackboard"), AI_BlackboardToJson(Blackboard, bIncludeProperties));
		return CreateSuccessResponse(Result);
	}

	Result->SetStringField(TEXT("asset_type"), TEXT("BehaviorTree"));
	Result->SetStringField(TEXT("name"), Tree->GetName());
	Result->SetStringField(TEXT("path"), Tree->GetPathName());

	if (Tree->BlackboardAsset)
	{
		Result->SetObjectField(TEXT("blackboard"), AI_BlackboardToJson(Tree->BlackboardAsset, bIncludeProperties));
	}

	if (Tree->RootDecorators.Num() > 0)
	{
		Result->SetArrayField(TEXT("root_decorators"), AI_DecoratorsToJson(Tree->RootDecorators, bIncludeProperties));
		if (Tree->RootDecoratorOps.Num() > 0)
		{
			Result->SetArrayField(TEXT("root_decorator_logic"), AI_DecoratorOpsToJson(Tree->RootDecoratorOps));
		}
	}

	if (Tree->RootNode)
	{
		Result->SetObjectField(TEXT("root"), AI_CompositeToJson(Tree->RootNode, bIncludeProperties, 0));
	}
	else
	{
		Result->SetStringField(TEXT("note"), TEXT("Behavior Tree has no root node (empty tree, or never saved from the editor graph)."));
	}

	return CreateSuccessResponse(Result);
}
