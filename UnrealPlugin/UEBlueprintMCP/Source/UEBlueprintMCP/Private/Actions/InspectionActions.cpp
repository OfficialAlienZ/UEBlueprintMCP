// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/InspectionActions.h"
#include "MCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"
#include "UObject/UnrealType.h"
#include "UObject/CoreNetTypes.h"

namespace
{
	const TCHAR* const AllSections[] =
	{
		TEXT("parents"), TEXT("interfaces"), TEXT("graphs"), TEXT("variables"), TEXT("functions"),
		TEXT("macros"), TEXT("dispatchers"), TEXT("events"), TEXT("components"), TEXT("class_defaults"),
		TEXT("widgets")
	};

	typedef TArray<TSharedPtr<FJsonValue>> FJsonArray;

	TSharedPtr<FJsonValue> AsValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonValue> AsValue(const FString& String)
	{
		return MakeShared<FJsonValueString>(String);
	}

	FString ExportProperty(const FProperty* Property, const void* Container)
	{
		FString Out;
		Property->ExportText_InContainer(0, Out, Container, nullptr, nullptr, PPF_None);
		return Out;
	}

	/** Editable properties of Object whose value differs from Baseline (name -> exported text). */
	TSharedPtr<FJsonObject> ChangedProperties(UObject* Object, UObject* Baseline, const UClass* ExcludeOwner)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Object || !Baseline)
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
			Result->SetStringField(Property->GetName(), ExportProperty(Property, Object));
		}
		return Result;
	}

	/** Parameter description from a function/event/macro pin. */
	TSharedPtr<FJsonObject> ParamToJson(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> Param = FMCPCommonUtils::PinTypeToJson(Pin->PinType);
		Param->SetStringField(TEXT("name"), Pin->PinName.ToString());
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			Param->SetBoolField(TEXT("is_exec"), true);
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			Param->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (Pin->DefaultObject)
		{
			Param->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}
		return Param;
	}

	FJsonArray ParamsFromPins(const UEdGraphNode* Node, EEdGraphPinDirection Direction, bool bIncludeExec)
	{
		FJsonArray Result;
		if (!Node)
		{
			return Result;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != Direction || Pin->bHidden)
			{
				continue;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
			{
				continue;
			}
			if (!bIncludeExec && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			Result.Add(AsValue(ParamToJson(Pin)));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> VariableToJson(const FBPVariableDescription& Var, UClass* GeneratedClass, UObject* DefaultObject)
	{
		TSharedPtr<FJsonObject> Obj = FMCPCommonUtils::PinTypeToJson(Var.VarType);
		// PinTypeToJson's "category" is the pin category; keep it distinct from the editor category.
		Obj->SetStringField(TEXT("pin_category"), Var.VarType.PinCategory.ToString());
		Obj->SetStringField(TEXT("name"), Var.VarName.ToString());
		Obj->SetStringField(TEXT("category"), Var.Category.ToString());
		if (!Var.FriendlyName.IsEmpty() && Var.FriendlyName != Var.VarName.ToString())
		{
			Obj->SetStringField(TEXT("friendly_name"), Var.FriendlyName);
		}

		// Prefer the compiled class default (handles structs/containers); fall back to the declared string.
		FString DefaultValue = Var.DefaultValue;
		if (GeneratedClass && DefaultObject)
		{
			if (const FProperty* Property = GeneratedClass->FindPropertyByName(Var.VarName))
			{
				DefaultValue = ExportProperty(Property, DefaultObject);
			}
		}
		Obj->SetStringField(TEXT("default_value"), DefaultValue);

		const uint64 Flags = Var.PropertyFlags;
		Obj->SetBoolField(TEXT("instance_editable"), (Flags & CPF_Edit) != 0 && (Flags & CPF_DisableEditOnInstance) == 0);
		Obj->SetBoolField(TEXT("blueprint_read_only"), (Flags & CPF_BlueprintReadOnly) != 0);
		Obj->SetBoolField(TEXT("expose_on_spawn"), (Flags & CPF_ExposeOnSpawn) != 0);
		Obj->SetBoolField(TEXT("save_game"), (Flags & CPF_SaveGame) != 0);
		Obj->SetBoolField(TEXT("transient"), (Flags & CPF_Transient) != 0);
		Obj->SetBoolField(TEXT("replicated"), (Flags & CPF_Net) != 0);
		if ((Flags & CPF_Net) != 0)
		{
			if (Var.RepNotifyFunc != NAME_None)
			{
				Obj->SetStringField(TEXT("rep_notify"), Var.RepNotifyFunc.ToString());
			}
			if (const UEnum* ConditionEnum = StaticEnum<ELifetimeCondition>())
			{
				Obj->SetStringField(TEXT("replication_condition"),
					ConditionEnum->GetNameStringByValue((int64)Var.ReplicationCondition.GetValue()));
			}
		}

		if (Var.MetaDataArray.Num() > 0)
		{
			TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
			for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
			{
				Meta->SetStringField(Entry.DataKey.ToString(), Entry.DataValue);
				if (Entry.DataKey == TEXT("tooltip"))
				{
					Obj->SetStringField(TEXT("tooltip"), Entry.DataValue);
				}
			}
			Obj->SetObjectField(TEXT("metadata"), Meta);
		}

		return Obj;
	}

	TSharedPtr<FJsonObject> FunctionGraphToJson(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* Result = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Entry) Entry = Cast<UK2Node_FunctionEntry>(Node);
			if (!Result) Result = Cast<UK2Node_FunctionResult>(Node);
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Graph->GetName());
		Obj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		Obj->SetBoolField(TEXT("is_override"),
			Blueprint->ParentClass && Blueprint->ParentClass->FindFunctionByName(Graph->GetFName()) != nullptr);

		if (Entry)
		{
			const int32 Flags = Entry->GetExtraFlags();
			Obj->SetStringField(TEXT("access"),
				(Flags & FUNC_Private) ? TEXT("private") : ((Flags & FUNC_Protected) ? TEXT("protected") : TEXT("public")));
			Obj->SetBoolField(TEXT("is_pure"), (Flags & FUNC_BlueprintPure) != 0);
			Obj->SetBoolField(TEXT("is_const"), (Flags & FUNC_Const) != 0);
			Obj->SetBoolField(TEXT("is_static"), (Flags & FUNC_Static) != 0);
			Obj->SetStringField(TEXT("category"), Entry->MetaData.Category.ToString());
			if (!Entry->MetaData.ToolTip.IsEmpty())
			{
				Obj->SetStringField(TEXT("tooltip"), Entry->MetaData.ToolTip.ToString());
			}
			if (!Entry->MetaData.Keywords.IsEmpty())
			{
				Obj->SetStringField(TEXT("keywords"), Entry->MetaData.Keywords.ToString());
			}
			Obj->SetBoolField(TEXT("call_in_editor"), Entry->MetaData.bCallInEditor);
			Obj->SetArrayField(TEXT("inputs"), ParamsFromPins(Entry, EGPD_Output, false));

			FJsonArray Locals;
			for (const FBPVariableDescription& Local : Entry->LocalVariables)
			{
				TSharedPtr<FJsonObject> LocalObj = FMCPCommonUtils::PinTypeToJson(Local.VarType);
				LocalObj->SetStringField(TEXT("name"), Local.VarName.ToString());
				LocalObj->SetStringField(TEXT("default_value"), Local.DefaultValue);
				Locals.Add(AsValue(LocalObj));
			}
			Obj->SetArrayField(TEXT("local_variables"), Locals);
		}
		Obj->SetArrayField(TEXT("outputs"), ParamsFromPins(Result, EGPD_Input, false));
		return Obj;
	}

	TSharedPtr<FJsonObject> MacroGraphToJson(UEdGraph* Graph)
	{
		UK2Node_Tunnel* EntryTunnel = nullptr;
		UK2Node_Tunnel* ExitTunnel = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
			if (!Tunnel || Tunnel->GetClass() != UK2Node_Tunnel::StaticClass())
			{
				continue; // skip macro instances, composites, etc.
			}
			if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs) EntryTunnel = Tunnel;
			if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs) ExitTunnel = Tunnel;
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Graph->GetName());
		Obj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		if (EntryTunnel)
		{
			Obj->SetStringField(TEXT("category"), EntryTunnel->MetaData.Category.ToString());
			if (!EntryTunnel->MetaData.ToolTip.IsEmpty())
			{
				Obj->SetStringField(TEXT("tooltip"), EntryTunnel->MetaData.ToolTip.ToString());
			}
		}
		// Entry tunnel exposes the macro inputs as output pins; exit tunnel exposes outputs as input pins.
		Obj->SetArrayField(TEXT("inputs"), ParamsFromPins(EntryTunnel, EGPD_Output, true));
		Obj->SetArrayField(TEXT("outputs"), ParamsFromPins(ExitTunnel, EGPD_Input, true));
		return Obj;
	}

	TSharedPtr<FJsonObject> DispatcherGraphToJson(UEdGraph* Graph)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Graph->GetName());
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				Obj->SetArrayField(TEXT("parameters"), ParamsFromPins(Entry, EGPD_Output, false));
				break;
			}
		}
		return Obj;
	}

	FString GraphKindName(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (Blueprint->UbergraphPages.Contains(Graph)) return TEXT("event_graph");
		if (Blueprint->FunctionGraphs.Contains(Graph)) return TEXT("function");
		if (Blueprint->MacroGraphs.Contains(Graph)) return TEXT("macro");
		if (Blueprint->DelegateSignatureGraphs.Contains(Graph)) return TEXT("dispatcher_signature");
		return TEXT("interface_function");
	}

	TSharedPtr<FJsonObject> GraphSummaryToJson(const UBlueprint* Blueprint, UEdGraph* Graph)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Graph->GetName());
		Obj->SetStringField(TEXT("kind"), GraphKindName(Blueprint, Graph));
		Obj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

		TArray<UEdGraph*> Children;
		Graph->GetAllChildrenGraphs(Children);
		if (Children.Num() > 0)
		{
			FJsonArray Names;
			for (const UEdGraph* Child : Children)
			{
				Names.Add(AsValue(Child->GetName()));
			}
			Obj->SetArrayField(TEXT("collapsed_graphs"), Names);
		}
		return Obj;
	}

	void AppendScsNodes(USCS_Node* Node, const FString& ParentName, bool bIncludeProperties,
		const FString& Owner, FJsonArray& Out)
	{
		if (!Node)
		{
			return;
		}

		const FString Name = Node->GetVariableName().ToString();
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : FString());

		FString EffectiveParent = ParentName;
		if (EffectiveParent.IsEmpty() && Node->ParentComponentOrVariableName != NAME_None)
		{
			EffectiveParent = Node->ParentComponentOrVariableName.ToString();
			Obj->SetBoolField(TEXT("parent_is_native"), Node->bIsParentComponentNative);
		}
		if (!EffectiveParent.IsEmpty())
		{
			Obj->SetStringField(TEXT("parent"), EffectiveParent);
		}
		if (Node->AttachToName != NAME_None)
		{
			Obj->SetStringField(TEXT("attach_socket"), Node->AttachToName.ToString());
		}
		if (!Owner.IsEmpty())
		{
			Obj->SetStringField(TEXT("owner"), Owner);
		}
		if (bIncludeProperties && Node->ComponentTemplate && Node->ComponentClass)
		{
			TSharedPtr<FJsonObject> Props = ChangedProperties(Node->ComponentTemplate, Node->ComponentClass->GetDefaultObject(), nullptr);
			if (Props->Values.Num() > 0)
			{
				Obj->SetObjectField(TEXT("properties"), Props);
			}
		}
		Out.Add(AsValue(Obj));

		for (USCS_Node* Child : Node->GetChildNodes())
		{
			AppendScsNodes(Child, Name, bIncludeProperties, Owner, Out);
		}
	}

	FString NetFlagsToString(uint32 NetFlags, bool& bOutReliable)
	{
		bOutReliable = (NetFlags & FUNC_NetReliable) != 0;
		if (NetFlags & FUNC_NetMulticast) return TEXT("multicast");
		if (NetFlags & FUNC_NetServer) return TEXT("server");
		if (NetFlags & FUNC_NetClient) return TEXT("client");
		return FString();
	}

	const TCHAR* StatusToString(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_BeingCreated: return TEXT("BeingCreated");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		default: return TEXT("Unknown");
		}
	}

	const TCHAR* BlueprintTypeToString(EBlueprintType Type)
	{
		switch (Type)
		{
		case BPTYPE_Normal: return TEXT("Normal");
		case BPTYPE_Const: return TEXT("Const");
		case BPTYPE_MacroLibrary: return TEXT("MacroLibrary");
		case BPTYPE_Interface: return TEXT("Interface");
		case BPTYPE_LevelScript: return TEXT("LevelScript");
		case BPTYPE_FunctionLibrary: return TEXT("FunctionLibrary");
		default: return TEXT("Unknown");
		}
	}
}

bool FGetBlueprintInfoAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (const TArray<TSharedPtr<FJsonValue>>* Sections = GetOptionalArray(Params, TEXT("sections")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Sections)
		{
			const FString Section = Value->AsString().ToLower();
			bool bKnown = false;
			for (const TCHAR* Known : AllSections)
			{
				if (Section == Known) { bKnown = true; break; }
			}
			if (!bKnown)
			{
				OutError = FString::Printf(TEXT("Unknown section '%s'. Valid sections: parents, interfaces, graphs, variables, functions, macros, dispatchers, events, components, class_defaults, widgets"), *Section);
				return false;
			}
		}
	}
	return ValidateBlueprint(Params, Context, OutError);
}

TSharedPtr<FJsonObject> FGetBlueprintInfoAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UBlueprint* Blueprint = GetTargetBlueprint(Params, Context);

	TSet<FString> Requested;
	if (const TArray<TSharedPtr<FJsonValue>>* Sections = GetOptionalArray(Params, TEXT("sections")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Sections)
		{
			Requested.Add(Value->AsString().ToLower());
		}
	}
	auto Want = [&Requested](const TCHAR* Section) { return Requested.Num() == 0 || Requested.Contains(Section); };
	const bool bComponentProps = GetOptionalBool(Params, TEXT("include_component_properties"), true);

	UClass* GeneratedClass = Blueprint->GeneratedClass;
	UObject* DefaultObject = GeneratedClass ? GeneratedClass->GetDefaultObject(false) : nullptr;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// ---- Summary (always) ----
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("blueprint_type"), BlueprintTypeToString(Blueprint->BlueprintType));
	Result->SetStringField(TEXT("status"), StatusToString(Blueprint->Status));
	if (Blueprint->ParentClass)
	{
		Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
		Result->SetStringField(TEXT("parent_class_path"), Blueprint->ParentClass->GetPathName());
	}
	if (GeneratedClass)
	{
		Result->SetStringField(TEXT("generated_class"), GeneratedClass->GetName());
	}
	if (!Blueprint->BlueprintDescription.IsEmpty())
	{
		Result->SetStringField(TEXT("description"), Blueprint->BlueprintDescription);
	}
	if (!Blueprint->BlueprintDisplayName.IsEmpty())
	{
		Result->SetStringField(TEXT("display_name"), Blueprint->BlueprintDisplayName);
	}

	// ---- Parent chain ----
	if (Want(TEXT("parents")))
	{
		FJsonArray Chain;
		for (UClass* Class = Blueprint->ParentClass; Class; Class = Class->GetSuperClass())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Class->GetName());
			Entry->SetStringField(TEXT("path"), Class->GetPathName());
			UBlueprint* ParentBP = Cast<UBlueprint>(Class->ClassGeneratedBy);
			Entry->SetBoolField(TEXT("is_blueprint"), ParentBP != nullptr);
			if (ParentBP)
			{
				Entry->SetStringField(TEXT("blueprint_path"), ParentBP->GetPathName());

				FJsonArray Functions, Macros, Variables, Dispatchers;
				for (const UEdGraph* Graph : ParentBP->FunctionGraphs) if (Graph) Functions.Add(AsValue(Graph->GetName()));
				for (const UEdGraph* Graph : ParentBP->MacroGraphs) if (Graph) Macros.Add(AsValue(Graph->GetName()));
				for (const UEdGraph* Graph : ParentBP->DelegateSignatureGraphs) if (Graph) Dispatchers.Add(AsValue(Graph->GetName()));
				for (const FBPVariableDescription& Var : ParentBP->NewVariables) Variables.Add(AsValue(Var.VarName.ToString()));
				Entry->SetArrayField(TEXT("functions"), Functions);
				Entry->SetArrayField(TEXT("macros"), Macros);
				Entry->SetArrayField(TEXT("dispatchers"), Dispatchers);
				Entry->SetArrayField(TEXT("variables"), Variables);
			}
			Chain.Add(AsValue(Entry));
		}
		Result->SetArrayField(TEXT("parents"), Chain);
	}

	// ---- Interfaces ----
	if (Want(TEXT("interfaces")))
	{
		FJsonArray Interfaces;
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			if (Description.Interface)
			{
				Entry->SetStringField(TEXT("name"), Description.Interface->GetName());
				Entry->SetStringField(TEXT("path"), Description.Interface->GetPathName());
			}
			FJsonArray GraphNames;
			for (const UEdGraph* Graph : Description.Graphs) if (Graph) GraphNames.Add(AsValue(Graph->GetName()));
			Entry->SetArrayField(TEXT("graphs"), GraphNames);
			Interfaces.Add(AsValue(Entry));
		}
		Result->SetArrayField(TEXT("interfaces"), Interfaces);
	}

	// ---- Graph index ----
	if (Want(TEXT("graphs")))
	{
		TArray<UEdGraph*> AllGraphs;
		AllGraphs.Append(Blueprint->UbergraphPages);
		AllGraphs.Append(Blueprint->FunctionGraphs);
		AllGraphs.Append(Blueprint->MacroGraphs);
		AllGraphs.Append(Blueprint->DelegateSignatureGraphs);
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			AllGraphs.Append(Description.Graphs);
		}

		FJsonArray Graphs;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph) Graphs.Add(AsValue(GraphSummaryToJson(Blueprint, Graph)));
		}
		Result->SetArrayField(TEXT("graphs"), Graphs);
	}

	// ---- Variables ----
	if (Want(TEXT("variables")))
	{
		FJsonArray Variables;
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				continue; // reported under "dispatchers"
			}
			Variables.Add(AsValue(VariableToJson(Var, GeneratedClass, DefaultObject)));
		}
		Result->SetArrayField(TEXT("variables"), Variables);
		Result->SetBoolField(TEXT("defaults_from_compiled_class"), DefaultObject != nullptr && Blueprint->Status != BS_Dirty);
	}

	// ---- Functions ----
	if (Want(TEXT("functions")))
	{
		FJsonArray Functions;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph) Functions.Add(AsValue(FunctionGraphToJson(Blueprint, Graph)));
		}
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : Description.Graphs)
			{
				if (!Graph) continue;
				TSharedPtr<FJsonObject> Entry = FunctionGraphToJson(Blueprint, Graph);
				if (Description.Interface)
				{
					Entry->SetStringField(TEXT("interface"), Description.Interface->GetName());
				}
				Functions.Add(AsValue(Entry));
			}
		}
		Result->SetArrayField(TEXT("functions"), Functions);
	}

	// ---- Macros ----
	if (Want(TEXT("macros")))
	{
		FJsonArray Macros;
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph) Macros.Add(AsValue(MacroGraphToJson(Graph)));
		}
		Result->SetArrayField(TEXT("macros"), Macros);
	}

	// ---- Event dispatchers ----
	if (Want(TEXT("dispatchers")))
	{
		FJsonArray Dispatchers;
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			if (Graph) Dispatchers.Add(AsValue(DispatcherGraphToJson(Graph)));
		}
		Result->SetArrayField(TEXT("dispatchers"), Dispatchers);
	}

	// ---- Events (event nodes and custom events; custom events are callable like functions) ----
	if (Want(TEXT("events")))
	{
		FJsonArray Events;
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
				if (!EventNode) continue;

				TSharedPtr<FJsonObject> Entry = FMCPCommonUtils::NodeToJson(EventNode, false);
				Entry->SetStringField(TEXT("graph"), Graph->GetName());
				Entry->SetArrayField(TEXT("inputs"), ParamsFromPins(EventNode, EGPD_Output, false));

				const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(EventNode);
				Entry->SetBoolField(TEXT("is_custom"), CustomEvent != nullptr);
				if (CustomEvent)
				{
					bool bReliable = false;
					const FString Replication = NetFlagsToString(CustomEvent->GetNetFlags(), bReliable);
					if (!Replication.IsEmpty())
					{
						Entry->SetStringField(TEXT("replication"), Replication);
						Entry->SetBoolField(TEXT("reliable"), bReliable);
					}
				}
				Events.Add(AsValue(Entry));
			}
		}
		Result->SetArrayField(TEXT("events"), Events);
	}

	// ---- Components ----
	if (Want(TEXT("components")))
	{
		FJsonArray Components;
		if (Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Root : Blueprint->SimpleConstructionScript->GetRootNodes())
			{
				AppendScsNodes(Root, FString(), bComponentProps, FString(), Components);
			}
		}
		Result->SetArrayField(TEXT("components"), Components);

		// Inherited: SCS components of parent Blueprints, then native components from the parent C++ class.
		FJsonArray Inherited;
		for (UClass* Class = Blueprint->ParentClass; Class; Class = Class->GetSuperClass())
		{
			const UBlueprint* ParentBP = Cast<UBlueprint>(Class->ClassGeneratedBy);
			if (ParentBP && ParentBP->SimpleConstructionScript)
			{
				for (USCS_Node* Root : ParentBP->SimpleConstructionScript->GetRootNodes())
				{
					AppendScsNodes(Root, FString(), bComponentProps, ParentBP->GetName(), Inherited);
				}
			}
		}
		if (AActor* ActorDefault = Cast<AActor>(DefaultObject))
		{
			TArray<UActorComponent*> NativeComponents;
			ActorDefault->GetComponents(NativeComponents);
			for (UActorComponent* Component : NativeComponents)
			{
				if (!Component) continue;
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Component->GetName());
				Entry->SetStringField(TEXT("class"), Component->GetClass()->GetName());
				Entry->SetStringField(TEXT("owner"), TEXT("native"));
				if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
				{
					if (Scene->GetAttachParent())
					{
						Entry->SetStringField(TEXT("parent"), Scene->GetAttachParent()->GetName());
					}
				}
				if (bComponentProps)
				{
					TSharedPtr<FJsonObject> Props = ChangedProperties(Component, Component->GetClass()->GetDefaultObject(), nullptr);
					if (Props->Values.Num() > 0)
					{
						Entry->SetObjectField(TEXT("properties"), Props);
					}
				}
				Inherited.Add(AsValue(Entry));
			}
		}
		Result->SetArrayField(TEXT("inherited_components"), Inherited);
	}

	// ---- Class defaults overridden from the parent class ----
	if (Want(TEXT("class_defaults")))
	{
		TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
		if (DefaultObject && Blueprint->ParentClass)
		{
			Defaults = ChangedProperties(DefaultObject, Blueprint->ParentClass->GetDefaultObject(), GeneratedClass);
		}
		Result->SetObjectField(TEXT("class_defaults"), Defaults);
	}

	// ---- Widget tree (Widget Blueprints only) ----
	if (Want(TEXT("widgets")))
	{
		if (const UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint))
		{
			FJsonArray Widgets;
			if (WidgetBP->WidgetTree)
			{
				TArray<UWidget*> AllWidgets;
				WidgetBP->WidgetTree->GetAllWidgets(AllWidgets);
				for (const UWidget* Widget : AllWidgets)
				{
					if (!Widget) continue;
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Widget->GetName());
					Entry->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
					if (const UPanelWidget* Parent = Widget->GetParent())
					{
						Entry->SetStringField(TEXT("parent"), Parent->GetName());
					}
					Entry->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
					Widgets.Add(AsValue(Entry));
				}
			}
			Result->SetArrayField(TEXT("widgets"), Widgets);

			FJsonArray Animations;
			for (const UWidgetAnimation* Animation : WidgetBP->Animations)
			{
				if (Animation) Animations.Add(AsValue(Animation->GetName()));
			}
			Result->SetArrayField(TEXT("widget_animations"), Animations);
		}
	}

	return CreateSuccessResponse(Result);
}
