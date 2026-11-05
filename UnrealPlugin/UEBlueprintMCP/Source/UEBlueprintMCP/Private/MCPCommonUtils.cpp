// Copyright (c) 2025 zolnoor. All rights reserved.

#include "MCPCommonUtils.h"
#include "MCPCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Timeline.h"
#include "K2Node_Self.h"
#include "K2Node_InputAction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/LevelScriptActor.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "EditorAssetLibrary.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/SlateColor.h"
#include "Layout/Margin.h"
#include "Misc/PackageName.h"

// =========================================================================
// JSON Parsing Utilities
// =========================================================================

FVector FMCPCommonUtils::GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
	FVector Result(0.0f, 0.0f, 0.0f);

	if (!JsonObject->HasField(FieldName))
	{
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
	{
		Result.X = (float)(*JsonArray)[0]->AsNumber();
		Result.Y = (float)(*JsonArray)[1]->AsNumber();
		Result.Z = (float)(*JsonArray)[2]->AsNumber();
	}

	return Result;
}

FRotator FMCPCommonUtils::GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
	FRotator Result(0.0f, 0.0f, 0.0f);

	if (!JsonObject->HasField(FieldName))
	{
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
	{
		Result.Pitch = (float)(*JsonArray)[0]->AsNumber();
		Result.Yaw = (float)(*JsonArray)[1]->AsNumber();
		Result.Roll = (float)(*JsonArray)[2]->AsNumber();
	}

	return Result;
}

FVector2D FMCPCommonUtils::GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
	FVector2D Result(0.0f, 0.0f);

	if (!JsonObject->HasField(FieldName))
	{
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 2)
	{
		Result.X = (float)(*JsonArray)[0]->AsNumber();
		Result.Y = (float)(*JsonArray)[1]->AsNumber();
	}

	return Result;
}

// =========================================================================
// Blueprint Utilities
// =========================================================================

namespace
{
	/** Map name of a level: package short name, without a trailing "_C" (the Level Blueprint's generated class is <Map>_C). */
	FString CommonUtils_NormalizeMapName(const FString& MapName)
	{
		FString Name = MapName.TrimStartAndEnd();
		if (Name.EndsWith(TEXT("_C")))
		{
			Name.LeftChopInline(2);
		}
		return Name;
	}

	bool CommonUtils_LevelMatches(const ULevel* Level, const FString& Name)
	{
		if (!Level || !Level->GetOutermost())
		{
			return false;
		}
		const FString PackageName = Level->GetOutermost()->GetName();
		return PackageName.Equals(Name, ESearchCase::IgnoreCase)
			|| FPackageName::GetShortName(PackageName).Equals(Name, ESearchCase::IgnoreCase);
	}
}

UBlueprint* FMCPCommonUtils::FindLevelScriptBlueprint(const FString& MapName)
{
	const FString Name = CommonUtils_NormalizeMapName(MapName);
	const bool bCurrent = Name.IsEmpty() || Name.Equals(TEXT("current"), ESearchCase::IgnoreCase);

	// 1. Levels loaded in the editor world (persistent level and streaming sub-levels).
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (EditorWorld)
	{
		if (bCurrent)
		{
			return EditorWorld->PersistentLevel ? EditorWorld->PersistentLevel->GetLevelScriptBlueprint(true) : nullptr;
		}
		for (ULevel* Level : EditorWorld->GetLevels())
		{
			if (CommonUtils_LevelMatches(Level, Name))
			{
				return Level->GetLevelScriptBlueprint(true);
			}
		}
	}
	if (bCurrent)
	{
		return nullptr;
	}

	// 2. Map asset that is not loaded in the editor: load its package and read the persistent level's script.
	UWorld* MapWorld = nullptr;
	if (Name.StartsWith(TEXT("/")))
	{
		FString ObjectPath = Name;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath = ObjectPath + TEXT(".") + FPackageName::GetShortName(Name);
		}
		MapWorld = LoadObject<UWorld>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
	}
	else
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> AssetList;
		MCPCompat::GetAssetsByClass(AssetRegistryModule.Get(), UWorld::StaticClass(), AssetList);
		for (const FAssetData& AssetData : AssetList)
		{
			if (AssetData.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				MapWorld = Cast<UWorld>(AssetData.GetAsset());
				break;
			}
		}
	}
	return (MapWorld && MapWorld->PersistentLevel) ? MapWorld->PersistentLevel->GetLevelScriptBlueprint(true) : nullptr;
}

UBlueprint* FMCPCommonUtils::FindBlueprint(const FString& BlueprintName)
{
	if (BlueprintName.IsEmpty()) return nullptr;

	// Level Blueprint: "level:<MapName>", "level:/Game/<Path>/<MapName>" or "level:" / "level:current" for the open map.
	static const FString LevelPrefix(TEXT("level:"));
	if (BlueprintName.StartsWith(LevelPrefix, ESearchCase::IgnoreCase))
	{
		return FindLevelScriptBlueprint(BlueprintName.Mid(LevelPrefix.Len()));
	}

	// Fast path: full asset path ("/Game/Folder/BP_Name" or "/Game/Folder/BP_Name.BP_Name").
	// Unambiguous and skips the Asset Registry scan entirely.
	if (BlueprintName.StartsWith(TEXT("/")))
	{
		FString ObjectPath = BlueprintName;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			FString ShortName;
			ObjectPath.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			ObjectPath = ObjectPath + TEXT(".") + ShortName;
		}
		return LoadObject<UBlueprint>(nullptr, *ObjectPath);
	}

	// Slow path: short name. Search asset registry for all blueprint types (including Widget Blueprints)
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.bRecursiveClasses = true;
	MCPCompat::AddFilterClass(Filter, UBlueprint::StaticClass());

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	const FAssetData* Match = nullptr;
	int32 MatchCount = 0;
	for (const FAssetData& AssetData : AssetList)
	{
		if (AssetData.AssetName.ToString() == BlueprintName)
		{
			if (!Match) Match = &AssetData;
			++MatchCount;
		}
	}

	if (MatchCount > 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("UEBlueprintMCP: Blueprint name '%s' is ambiguous (%d matches); using %s. Pass the full asset path (e.g. /Game/Folder/%s) to disambiguate."),
			*BlueprintName, MatchCount, *Match->PackageName.ToString(), *BlueprintName);
	}

	return Match ? Cast<UBlueprint>(Match->GetAsset()) : nullptr;
}

UEdGraph* FMCPCommonUtils::FindOrCreateEventGraph(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// Try to find the event graph
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph->GetName().Contains(TEXT("EventGraph")))
		{
			return Graph;
		}
	}

	// Create a new event graph if none exists
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, FName(TEXT("EventGraph")),
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
	return NewGraph;
}

UEdGraph* FMCPCommonUtils::FindFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName)
{
	if (!Blueprint || FunctionName.IsEmpty())
	{
		return nullptr;
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph->GetFName() == FName(*FunctionName))
		{
			return Graph;
		}
	}

	return nullptr;
}

UEdGraph* FMCPCommonUtils::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// If no graph name specified, return the event graph (default behavior)
	if (GraphName.IsEmpty())
	{
		return FindOrCreateEventGraph(Blueprint);
	}

	// First check if it's a function graph
	UEdGraph* FunctionGraph = FindFunctionGraph(Blueprint, GraphName);
	if (FunctionGraph)
	{
		return FunctionGraph;
	}

	// Check ubergraph pages (event graphs can have different names)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph->GetFName() == FName(*GraphName) || Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Macros, event dispatcher signatures, interface graphs, collapsed sub-graphs
	return FindSpecialGraphByName(Blueprint, GraphName);
}

UEdGraph* FMCPCommonUtils::FindSpecialGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint || GraphName.IsEmpty())
	{
		return nullptr;
	}

	const FName GraphFName(*GraphName);

	TArray<UEdGraph*> TopLevel;
	TopLevel.Append(Blueprint->MacroGraphs);
	TopLevel.Append(Blueprint->DelegateSignatureGraphs);
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		TopLevel.Append(Interface.Graphs);
	}

	for (UEdGraph* Graph : TopLevel)
	{
		if (Graph && Graph->GetFName() == GraphFName)
		{
			return Graph;
		}
	}

	// Collapsed graphs (composite nodes) nested inside any graph
	TArray<UEdGraph*> Roots;
	Roots.Append(Blueprint->UbergraphPages);
	Roots.Append(Blueprint->FunctionGraphs);
	Roots.Append(TopLevel);
	for (UEdGraph* Root : Roots)
	{
		if (!Root) continue;
		TArray<UEdGraph*> Children;
		Root->GetAllChildrenGraphs(Children);
		for (UEdGraph* Child : Children)
		{
			if (Child && Child->GetFName() == GraphFName)
			{
				return Child;
			}
		}
	}

	return nullptr;
}

USCS_Node* FMCPCommonUtils::FindComponentNode(UBlueprint* Blueprint, const FString& ComponentName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// Traverse inheritance hierarchy to find component
	UBlueprint* SearchBP = Blueprint;
	while (SearchBP)
	{
		if (SearchBP->SimpleConstructionScript)
		{
			for (USCS_Node* Node : SearchBP->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->GetVariableName().ToString() == ComponentName)
				{
					return Node;
				}
			}
		}

		// Walk up to parent Blueprint
		UClass* ParentClass = SearchBP->ParentClass;
		SearchBP = (ParentClass) ? Cast<UBlueprint>(ParentClass->ClassGeneratedBy) : nullptr;
	}

	return nullptr;
}

// =========================================================================
// Introspection JSON Serialization
// =========================================================================

TSharedPtr<FJsonObject> FMCPCommonUtils::PinTypeToJson(const FEdGraphPinType& PinType)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(PinType).ToString());
	Obj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
	if (PinType.PinSubCategory != NAME_None)
	{
		Obj->SetStringField(TEXT("sub_category"), PinType.PinSubCategory.ToString());
	}
	if (UObject* SubObject = PinType.PinSubCategoryObject.Get())
	{
		Obj->SetStringField(TEXT("sub_category_object"), SubObject->GetName());
	}
	if (PinType.IsArray())
	{
		Obj->SetStringField(TEXT("container"), TEXT("array"));
	}
	else if (PinType.IsSet())
	{
		Obj->SetStringField(TEXT("container"), TEXT("set"));
	}
	else if (PinType.IsMap())
	{
		Obj->SetStringField(TEXT("container"), TEXT("map"));
	}
	return Obj;
}

TSharedPtr<FJsonObject> FMCPCommonUtils::PinToJson(const UEdGraphPin* Pin, bool bIncludeConnections)
{
	TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
	PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
	PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
	PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
	if (Pin->PinType.PinSubCategory != NAME_None)
	{
		PinObj->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
	}
	if (Pin->PinType.PinSubCategoryObject.Get())
	{
		PinObj->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetName());
	}
	PinObj->SetStringField(TEXT("type"), UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString());
	PinObj->SetBoolField(TEXT("is_hidden"), Pin->bHidden);

	if (!Pin->PinFriendlyName.IsEmpty())
	{
		PinObj->SetStringField(TEXT("friendly_name"), Pin->PinFriendlyName.ToString());
	}
	if (!Pin->DefaultValue.IsEmpty())
	{
		PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
	}
	if (Pin->DefaultObject)
	{
		PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
	}
	if (!Pin->DefaultTextValue.IsEmpty())
	{
		PinObj->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
	}

	if (bIncludeConnections)
	{
		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked) continue;
			TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
			if (const UEdGraphNode* LinkedNode = Linked->GetOwningNodeUnchecked())
			{
				LinkObj->SetStringField(TEXT("node_guid"), LinkedNode->NodeGuid.ToString());
				LinkObj->SetStringField(TEXT("node_title"), LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
			LinkObj->SetStringField(TEXT("pin"), Linked->PinName.ToString());
			Links.Add(MakeShared<FJsonValueObject>(LinkObj));
		}
		PinObj->SetArrayField(TEXT("linked_to"), Links);
	}

	return PinObj;
}

TSharedPtr<FJsonObject> FMCPCommonUtils::NodeToJson(UEdGraphNode* Node, bool bIncludePins)
{
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
	NodeObj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	NodeObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	NodeObj->SetNumberField(TEXT("node_x"), Node->NodePosX);
	NodeObj->SetNumberField(TEXT("node_y"), Node->NodePosY);
	if (!Node->NodeComment.IsEmpty())
	{
		NodeObj->SetStringField(TEXT("node_comment"), Node->NodeComment);
	}
	if (Node->GetDesiredEnabledState() != ENodeEnabledState::Enabled)
	{
		NodeObj->SetBoolField(TEXT("is_disabled"), true);
	}

	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		NodeObj->SetStringField(TEXT("function_name"), CallNode->FunctionReference.GetMemberName().ToString());
		NodeObj->SetBoolField(TEXT("is_self_context"), CallNode->FunctionReference.IsSelfContext());
		NodeObj->SetBoolField(TEXT("is_pure"), CallNode->IsNodePure());

		UFunction* Function = CallNode->GetTargetFunction();
		NodeObj->SetBoolField(TEXT("function_resolved"), Function != nullptr);
		UClass* OwnerClass = Function ? Function->GetOwnerClass() : nullptr;
		if (!OwnerClass && !CallNode->FunctionReference.IsSelfContext())
		{
			OwnerClass = CallNode->FunctionReference.GetMemberParentClass();
		}
		if (OwnerClass)
		{
			NodeObj->SetStringField(TEXT("function_owner"), OwnerClass->GetName());
			NodeObj->SetStringField(TEXT("function_owner_path"), OwnerClass->GetPathName());
		}
	}
	else if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
	{
		NodeObj->SetStringField(TEXT("variable_name"), VarNode->GetVarName().ToString());
		if (!VarNode->VariableReference.IsSelfContext())
		{
			if (UClass* SourceClass = VarNode->GetVariableSourceClass())
			{
				NodeObj->SetStringField(TEXT("variable_owner"), SourceClass->GetName());
			}
		}
	}
	else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
	{
		NodeObj->SetStringField(TEXT("event_name"), CustomEvent->CustomFunctionName.ToString());
	}
	else if (UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
	{
		// Component delegate event (e.g. OnComponentBeginOverlap of a SphereCollision component).
		NodeObj->SetStringField(TEXT("event_name"), BoundEvent->CustomFunctionName.ToString());
		NodeObj->SetStringField(TEXT("component_name"), BoundEvent->ComponentPropertyName.ToString());
		NodeObj->SetStringField(TEXT("delegate_name"), BoundEvent->DelegatePropertyName.ToString());
		if (BoundEvent->DelegateOwnerClass)
		{
			NodeObj->SetStringField(TEXT("delegate_owner"), BoundEvent->DelegateOwnerClass->GetName());
		}
	}
	else if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		NodeObj->SetStringField(TEXT("event_name"), EventNode->EventReference.GetMemberName().ToString());
	}
	else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
		{
			NodeObj->SetStringField(TEXT("macro_name"), MacroGraph->GetName());
		}
		if (UBlueprint* MacroBP = MacroNode->GetSourceBlueprint())
		{
			NodeObj->SetStringField(TEXT("macro_blueprint"), MacroBP->GetPathName());
		}
	}
	else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		if (CastNode->TargetType)
		{
			NodeObj->SetStringField(TEXT("cast_target"), CastNode->TargetType->GetName());
		}
	}
	else if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
	{
		// Curves/keys are available via get_blueprint_info sections=["timelines"].
		NodeObj->SetStringField(TEXT("timeline_name"), TimelineNode->TimelineName.ToString());
	}

	// Class-valued input pins (Get Actor Of Class, Spawn Actor From Class, Create Widget, ...):
	// reported even without pins so the class a node works on is visible in the basic dump.
	TSharedPtr<FJsonObject> ClassPins = MakeShared<FJsonObject>();
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input)
		{
			continue;
		}
		const FName Category = Pin->PinType.PinCategory;
		if (Category != UEdGraphSchema_K2::PC_Class && Category != UEdGraphSchema_K2::PC_SoftClass)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetBoolField(TEXT("is_linked"), Pin->LinkedTo.Num() > 0);
		if (Pin->DefaultObject)
		{
			Entry->SetStringField(TEXT("class"), Pin->DefaultObject->GetName());
			Entry->SetStringField(TEXT("class_path"), Pin->DefaultObject->GetPathName());
		}
		else if (!Pin->DefaultValue.IsEmpty())
		{
			// Soft class references keep their path in the default value.
			Entry->SetStringField(TEXT("class_path"), Pin->DefaultValue);
		}
		if (const UObject* BaseClass = Pin->PinType.PinSubCategoryObject.Get())
		{
			Entry->SetStringField(TEXT("base_class"), BaseClass->GetName());
		}
		ClassPins->SetObjectField(Pin->PinName.ToString(), Entry);
	}
	if (ClassPins->Values.Num() > 0)
	{
		NodeObj->SetObjectField(TEXT("class_pins"), ClassPins);
	}

	if (bIncludePins)
	{
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->bHidden && Pin->LinkedTo.Num() == 0) continue;
			PinsArray.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, true)));
		}
		NodeObj->SetArrayField(TEXT("pins"), PinsArray);
	}

	return NodeObj;
}

// =========================================================================
// Property Setting Utilities
// =========================================================================

bool FMCPCommonUtils::SetObjectProperty(UObject* Object, const FString& PropertyName,
	const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
	if (!Object)
	{
		OutErrorMessage = TEXT("Invalid object");
		return false;
	}

	// PropertyName may be a dotted path into nested structs / sub-objects, e.g. "RenderTransform.Scale",
	// "Font.Size", "Slot.LayoutData.Anchors.Minimum" or "Brush.ResourceObject".
	TArray<FString> PathParts;
	PropertyName.ParseIntoArray(PathParts, TEXT("."), true);
	if (PathParts.Num() == 0)
	{
		OutErrorMessage = TEXT("Empty property name");
		return false;
	}

	UStruct* CurrentStruct = Object->GetClass();
	void* Container = Object;
	UObject* OwnerObject = Object;
	FProperty* Property = nullptr;
	void* PropertyAddr = nullptr;
	for (int32 PartIndex = 0; PartIndex < PathParts.Num(); ++PartIndex)
	{
		Property = CurrentStruct->FindPropertyByName(*PathParts[PartIndex]);
		if (!Property)
		{
			OutErrorMessage = FString::Printf(TEXT("Property not found: %s (no '%s' on %s)"),
				*PropertyName, *PathParts[PartIndex], *CurrentStruct->GetName());
			return false;
		}
		PropertyAddr = Property->ContainerPtrToValuePtr<void>(Container);
		if (PartIndex == PathParts.Num() - 1)
		{
			break;
		}

		if (FStructProperty* InnerStruct = CastField<FStructProperty>(Property))
		{
			CurrentStruct = InnerStruct->Struct;
			Container = PropertyAddr;
		}
		else if (FObjectProperty* InnerObject = CastField<FObjectProperty>(Property))
		{
			UObject* SubObject = InnerObject->GetObjectPropertyValue(PropertyAddr);
			if (!SubObject)
			{
				OutErrorMessage = FString::Printf(TEXT("Cannot set %s: '%s' is null"), *PropertyName, *PathParts[PartIndex]);
				return false;
			}
			SubObject->Modify();
			CurrentStruct = SubObject->GetClass();
			Container = SubObject;
			OwnerObject = SubObject;
		}
		else
		{
			OutErrorMessage = FString::Printf(TEXT("Cannot descend into '%s' (%s) in path %s: not a struct or object property"),
				*PathParts[PartIndex], *Property->GetCPPType(), *PropertyName);
			return false;
		}
	}

	// Handle different property types
	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		BoolProp->SetPropertyValue(PropertyAddr, Value->AsBool());
		return true;
	}
	else if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		IntProp->SetPropertyValue(PropertyAddr, static_cast<int32>(Value->AsNumber()));
		return true;
	}
	else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		FloatProp->SetPropertyValue(PropertyAddr, Value->AsNumber());
		return true;
	}
	else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
	{
		DoubleProp->SetPropertyValue(PropertyAddr, Value->AsNumber());
		return true;
	}
	else if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		StrProp->SetPropertyValue(PropertyAddr, Value->AsString());
		return true;
	}
	else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		UEnum* EnumDef = EnumProp->GetEnum();
		FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();

		if (EnumDef && UnderlyingProp)
		{
			if (Value->Type == EJson::Number)
			{
				UnderlyingProp->SetIntPropertyValue(PropertyAddr, static_cast<int64>(Value->AsNumber()));
				return true;
			}
			else if (Value->Type == EJson::String)
			{
				FString EnumValueName = Value->AsString();
				if (EnumValueName.Contains(TEXT("::")))
				{
					EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
				}

				int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
				if (EnumValue != INDEX_NONE)
				{
					UnderlyingProp->SetIntPropertyValue(PropertyAddr, EnumValue);
					return true;
				}
				OutErrorMessage = FString::Printf(TEXT("Invalid enum value: %s"), *EnumValueName);
				return false;
			}
		}
	}
	else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		// Clients that only send strings may pass JSON text such as "[1,0,0,1]" or "{\"R\":1}".
		TSharedPtr<FJsonValue> StructValue = Value;
		if (Value->Type == EJson::String)
		{
			const FString Trimmed = Value->AsString().TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("[")) || Trimmed.StartsWith(TEXT("{")))
			{
				TSharedPtr<FJsonValue> Parsed;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
				if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
				{
					StructValue = Parsed;
				}
			}
		}

		if (StructValue->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = StructValue->AsArray();
			if (StructProp->Struct == TBaseStructure<FVector>::Get() && Arr.Num() >= 3)
			{
				FVector* VecPtr = (FVector*)PropertyAddr;
				VecPtr->X = Arr[0]->AsNumber();
				VecPtr->Y = Arr[1]->AsNumber();
				VecPtr->Z = Arr[2]->AsNumber();
				return true;
			}
			if (StructProp->Struct == TBaseStructure<FRotator>::Get() && Arr.Num() >= 3)
			{
				FRotator* RotPtr = (FRotator*)PropertyAddr;
				RotPtr->Pitch = Arr[0]->AsNumber();
				RotPtr->Yaw = Arr[1]->AsNumber();
				RotPtr->Roll = Arr[2]->AsNumber();
				return true;
			}
			if (StructProp->Struct == TBaseStructure<FVector2D>::Get() && Arr.Num() >= 2)
			{
				FVector2D* Vec2Ptr = (FVector2D*)PropertyAddr;
				Vec2Ptr->X = Arr[0]->AsNumber();
				Vec2Ptr->Y = Arr[1]->AsNumber();
				return true;
			}
			// FMargin (Slot.Padding, Slot.LayoutData.Offsets, ...): [Left, Top, Right, Bottom]
			if (StructProp->Struct == TBaseStructure<FMargin>::Get() && Arr.Num() >= 4)
			{
				*(FMargin*)PropertyAddr = FMargin((float)Arr[0]->AsNumber(), (float)Arr[1]->AsNumber(), (float)Arr[2]->AsNumber(), (float)Arr[3]->AsNumber());
				return true;
			}
			// FLinearColor and FSlateColor (TextBlock ColorAndOpacity, ...): [R, G, B] or [R, G, B, A], 0..1
			const bool bLinearColor = StructProp->Struct == TBaseStructure<FLinearColor>::Get();
			const bool bSlateColor = StructProp->Struct->GetFName() == TEXT("SlateColor");
			if ((bLinearColor || bSlateColor) && Arr.Num() >= 3)
			{
				const FLinearColor Color(
					(float)Arr[0]->AsNumber(),
					(float)Arr[1]->AsNumber(),
					(float)Arr[2]->AsNumber(),
					Arr.Num() >= 4 ? (float)Arr[3]->AsNumber() : 1.0f);
				if (bLinearColor)
				{
					*(FLinearColor*)PropertyAddr = Color;
				}
				else
				{
					*(FSlateColor*)PropertyAddr = FSlateColor(Color);
				}
				return true;
			}
		}
		else if (StructValue->Type == EJson::Object)
		{
			if (FJsonObjectConverter::JsonObjectToUStruct(StructValue->AsObject().ToSharedRef(), StructProp->Struct, PropertyAddr, 0, 0))
			{
				return true;
			}
			OutErrorMessage = FString::Printf(TEXT("Could not convert JSON object to struct %s for: %s"), *StructProp->Struct->GetName(), *PropertyName);
			return false;
		}
	}
	else if (CastField<FTextProperty>(Property) && Value->Type == EJson::String)
	{
		*(FText*)PropertyAddr = FText::FromString(Value->AsString());
		return true;
	}
	else if (CastField<FNameProperty>(Property) && Value->Type == EJson::String)
	{
		*(FName*)PropertyAddr = FName(*Value->AsString());
		return true;
	}
	else if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
	{
		FString ClassPath = Value->AsString();
		UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath);

		if (!LoadedClass)
		{
			// Try constructing path from Blueprint name
			FString BlueprintPath = FString::Printf(TEXT("/Game/Blueprints/%s.%s_C"), *ClassPath, *ClassPath);
			LoadedClass = LoadObject<UClass>(nullptr, *BlueprintPath);
		}

		if (LoadedClass)
		{
			ClassProp->SetPropertyValue(PropertyAddr, LoadedClass);
			return true;
		}

		OutErrorMessage = FString::Printf(TEXT("Could not load class: %s"), *ClassPath);
		return false;
	}
	else if (FObjectProperty* ObjectProp = CastField<FObjectProperty>(Property))
	{
		// Asset reference by path (e.g. Font.FontObject, Brush.ResourceObject); empty string clears it.
		// Instanced sub-objects such as Slot are edited through dotted paths, not replaced.
		if (Value->Type == EJson::String)
		{
			const FString AssetPath = Value->AsString().TrimStartAndEnd();
			if (AssetPath.IsEmpty() || AssetPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				ObjectProp->SetObjectPropertyValue(PropertyAddr, nullptr);
				return true;
			}

			UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath);
			if (!Loaded && !AssetPath.Contains(TEXT(".")))
			{
				// "/Game/UI/T_Icon" -> "/Game/UI/T_Icon.T_Icon"
				Loaded = LoadObject<UObject>(nullptr, *FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath)));
			}
			if (!Loaded)
			{
				OutErrorMessage = FString::Printf(TEXT("Could not load asset for %s: %s"), *PropertyName, *AssetPath);
				return false;
			}
			if (!Loaded->IsA(ObjectProp->PropertyClass))
			{
				OutErrorMessage = FString::Printf(TEXT("Asset %s is a %s, but %s expects %s"),
					*AssetPath, *Loaded->GetClass()->GetName(), *PropertyName, *ObjectProp->PropertyClass->GetName());
				return false;
			}
			ObjectProp->SetObjectPropertyValue(PropertyAddr, Loaded);
			return true;
		}
	}

	// Generic fallback: Unreal export text, e.g. "(R=1.0,G=0.0,B=0.0,A=1.0)" or
	// "(SpecifiedColor=(R=1,G=0,B=0,A=1),ColorUseRule=UseColor_Specified)"
	if (Value->Type == EJson::String)
	{
		if (MCPCompat::ImportPropertyText(Property, Value->AsString(), PropertyAddr, OwnerObject))
		{
			return true;
		}
		OutErrorMessage = FString::Printf(TEXT("Could not parse value for %s (%s) as export text."), *PropertyName, *Property->GetCPPType());
		return false;
	}

	OutErrorMessage = FString::Printf(TEXT("Unsupported property type for: %s (%s)"), *PropertyName, *Property->GetCPPType());
	return false;
}

// =========================================================================
// Graph Node Utilities
// =========================================================================

UEdGraphPin* FMCPCommonUtils::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}

	// Exact match first
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
		{
			return Pin;
		}
	}

	// Case-insensitive match
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) &&
			(Direction == EGPD_MAX || Pin->Direction == Direction))
		{
			return Pin;
		}
	}

	return nullptr;
}

UK2Node_Event* FMCPCommonUtils::FindExistingEventNode(UEdGraph* Graph, const FString& EventName)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
		if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
		{
			return EventNode;
		}
	}

	return nullptr;
}

UK2Node_Event* FMCPCommonUtils::CreateEventNode(UEdGraph* Graph, const FString& EventName, FVector2D Position)
{
	if (!Graph)
	{
		return nullptr;
	}

	// Get the blueprint for context
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!Blueprint)
	{
		return nullptr;
	}

	// Create the event node
	UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
	if (!EventNode)
	{
		return nullptr;
	}

	// Find the event function in AActor or parent class
	UClass* OwnerClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;
	if (OwnerClass)
	{
		UFunction* EventFunc = OwnerClass->FindFunctionByName(FName(*EventName), EIncludeSuperFlag::IncludeSuper);
		if (EventFunc)
		{
			EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
		}
		else
		{
			// Try with Receive prefix for implementable events
			FString ReceiveEventName = TEXT("Receive") + EventName.Replace(TEXT("Receive"), TEXT(""));
			EventFunc = OwnerClass->FindFunctionByName(FName(*ReceiveEventName), EIncludeSuperFlag::IncludeSuper);
			if (EventFunc)
			{
				EventNode->EventReference.SetFromField<UFunction>(EventFunc, false);
			}
		}
	}

	EventNode->NodePosX = Position.X;
	EventNode->NodePosY = Position.Y;

	Graph->AddNode(EventNode);
	EventNode->CreateNewGuid();
	EventNode->PostPlacedNewNode();
	EventNode->AllocateDefaultPins();

	return EventNode;
}

UK2Node_InputAction* FMCPCommonUtils::CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, FVector2D Position)
{
	if (!Graph)
	{
		return nullptr;
	}

	UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
	if (!InputActionNode)
	{
		return nullptr;
	}

	InputActionNode->InputActionName = FName(*ActionName);
	InputActionNode->NodePosX = Position.X;
	InputActionNode->NodePosY = Position.Y;

	Graph->AddNode(InputActionNode);
	InputActionNode->CreateNewGuid();
	InputActionNode->PostPlacedNewNode();
	InputActionNode->AllocateDefaultPins();

	return InputActionNode;
}

UK2Node_CallFunction* FMCPCommonUtils::CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, FVector2D Position)
{
	if (!Graph || !Function)
	{
		return nullptr;
	}

	UK2Node_CallFunction* FunctionNode = NewObject<UK2Node_CallFunction>(Graph);
	if (!FunctionNode)
	{
		return nullptr;
	}

	FunctionNode->SetFromFunction(Function);
	FunctionNode->NodePosX = Position.X;
	FunctionNode->NodePosY = Position.Y;

	Graph->AddNode(FunctionNode);
	FunctionNode->CreateNewGuid();
	FunctionNode->PostPlacedNewNode();
	FunctionNode->AllocateDefaultPins();

	return FunctionNode;
}

UK2Node_Self* FMCPCommonUtils::CreateSelfReferenceNode(UEdGraph* Graph, FVector2D Position)
{
	if (!Graph)
	{
		return nullptr;
	}

	UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
	if (!SelfNode)
	{
		return nullptr;
	}

	SelfNode->NodePosX = Position.X;
	SelfNode->NodePosY = Position.Y;

	Graph->AddNode(SelfNode);
	SelfNode->CreateNewGuid();
	SelfNode->PostPlacedNewNode();
	SelfNode->AllocateDefaultPins();

	return SelfNode;
}

TSharedPtr<FJsonObject> FMCPCommonUtils::CreateErrorResponse(const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), false);
	Response->SetStringField(TEXT("error"), ErrorMessage);
	return Response;
}

// =========================================================================
// Actor Utilities
// =========================================================================

AActor* FMCPCommonUtils::FindActor(UWorld* World, const FString& NameOrAlias)
{
	if (!World || NameOrAlias.IsEmpty())
	{
		return nullptr;
	}

	if (NameOrAlias.StartsWith(TEXT("@")))
	{
		if (!World->IsGameWorld())
		{
			return nullptr;
		}
		const FString Alias = NameOrAlias.Mid(1).ToLower();
		APlayerController* PlayerController = UGameplayStatics::GetPlayerController(World, 0);

		if (Alias == TEXT("player_controller")) return PlayerController;
		if (Alias == TEXT("player_pawn")) return PlayerController ? PlayerController->GetPawn() : nullptr;
		if (Alias == TEXT("player_state")) return PlayerController ? PlayerController->PlayerState : nullptr;
		if (Alias == TEXT("hud")) return PlayerController ? PlayerController->GetHUD() : nullptr;
		if (Alias == TEXT("game_mode")) return World->GetAuthGameMode();
		if (Alias == TEXT("game_state")) return World->GetGameState();
		if (Alias == TEXT("level_script")) return World->GetLevelScriptActor();
		return nullptr;
	}

	AActor* LabelMatch = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		if (Actor->GetName() == NameOrAlias)
		{
			return Actor;
		}
		if (!LabelMatch && Actor->GetActorLabel() == NameOrAlias)
		{
			LabelMatch = Actor;
		}
	}
	return LabelMatch;
}

TSharedPtr<FJsonObject> FMCPCommonUtils::ActorToJsonObject(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
	ActorObject->SetStringField(TEXT("name"), Actor->GetName());
	ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

	FVector Location = Actor->GetActorLocation();
	TArray<TSharedPtr<FJsonValue>> LocationArray;
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
	ActorObject->SetArrayField(TEXT("location"), LocationArray);

	FRotator Rotation = Actor->GetActorRotation();
	TArray<TSharedPtr<FJsonValue>> RotationArray;
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
	ActorObject->SetArrayField(TEXT("rotation"), RotationArray);

	FVector Scale = Actor->GetActorScale3D();
	TArray<TSharedPtr<FJsonValue>> ScaleArray;
	ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
	ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
	ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
	ActorObject->SetArrayField(TEXT("scale"), ScaleArray);

	return ActorObject;
}

TSharedPtr<FJsonValue> FMCPCommonUtils::ActorToJsonValue(AActor* Actor)
{
	TSharedPtr<FJsonObject> Obj = ActorToJsonObject(Actor);
	if (Obj.IsValid())
	{
		return MakeShared<FJsonValueObject>(Obj);
	}
	return MakeShared<FJsonValueNull>();
}
