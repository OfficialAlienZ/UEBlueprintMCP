// Copyright (c) 2025 zolnoor. All rights reserved.

#include "MCPBridge.h"
#include "MCPServer.h"
#include "Actions/EditorAction.h"
#include "Actions/BlueprintActions.h"
#include "Actions/EditorActions.h"
#include "Actions/InspectionActions.h"
#include "Actions/AIActions.h"
#include "Actions/AssetInspectionActions.h"
#include "Actions/ObjectInspectionActions.h"
#include "Actions/PIEActions.h"
#include "Actions/GameplayActions.h"
#include "Actions/ObserveActions.h"
#include "Actions/AutomationActions.h"
#include "MCPLogCapture.h"
#include "Actions/NodeActions.h"
#include "Actions/ProjectActions.h"
#include "Actions/UMGActions.h"
#include "Actions/MaterialActions.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"

// NOTE: SEH crash protection is deferred to Phase 2
// For now, using defensive programming (validation before execution)

UMCPBridge::UMCPBridge()
	: Server(nullptr)
{
}

void UMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Bridge initializing"));

	// Register action handlers
	RegisterActions();

	// Capture the Output Log for get_output_log
	FMCPLogCapture::Get().Start();

	// Start the TCP server
	Server = new FMCPServer(this, DefaultPort);
	if (Server->Start())
	{
		UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Server started on port %d"), DefaultPort);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UEBlueprintMCP: Failed to start server"));
	}
}

void UMCPBridge::Deinitialize()
{
	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Bridge deinitializing"));

	// Stop the server
	if (Server)
	{
		Server->Stop();
		delete Server;
		Server = nullptr;
	}

	// Clear action handlers
	ActionHandlers.Empty();

	FMCPLogCapture::Get().Stop();

	Super::Deinitialize();
}

void UMCPBridge::RegisterActions()
{
	// =========================================================================
	// Blueprint Actions
	// =========================================================================
	ActionHandlers.Add(TEXT("create_blueprint"), MakeShared<FCreateBlueprintAction>());
	ActionHandlers.Add(TEXT("compile_blueprint"), MakeShared<FCompileBlueprintAction>());
	ActionHandlers.Add(TEXT("add_component_to_blueprint"), MakeShared<FAddComponentToBlueprintAction>());
	ActionHandlers.Add(TEXT("spawn_blueprint_actor"), MakeShared<FSpawnBlueprintActorAction>());
	ActionHandlers.Add(TEXT("set_component_property"), MakeShared<FSetComponentPropertyAction>());
	ActionHandlers.Add(TEXT("set_static_mesh_properties"), MakeShared<FSetStaticMeshPropertiesAction>());
	ActionHandlers.Add(TEXT("set_physics_properties"), MakeShared<FSetPhysicsPropertiesAction>());
	ActionHandlers.Add(TEXT("set_blueprint_property"), MakeShared<FSetBlueprintPropertyAction>());
	ActionHandlers.Add(TEXT("create_colored_material"), MakeShared<FCreateColoredMaterialAction>());

	// =========================================================================
	// Inspection Actions (read-only introspection)
	// =========================================================================
	ActionHandlers.Add(TEXT("get_blueprint_info"), MakeShared<FGetBlueprintInfoAction>());
	ActionHandlers.Add(TEXT("get_behavior_tree_info"), MakeShared<FGetBehaviorTreeInfoAction>());
	ActionHandlers.Add(TEXT("get_enum_info"), MakeShared<FGetEnumInfoAction>());
	ActionHandlers.Add(TEXT("get_curve_info"), MakeShared<FGetCurveInfoAction>());
	ActionHandlers.Add(TEXT("get_data_table_info"), MakeShared<FGetDataTableInfoAction>());
	ActionHandlers.Add(TEXT("get_object_properties"), MakeShared<FGetObjectPropertiesAction>());
	ActionHandlers.Add(TEXT("get_project_config"), MakeShared<FGetProjectConfigAction>());

	// =========================================================================
	// Editor Actions (actors, viewport, save)
	// =========================================================================
	ActionHandlers.Add(TEXT("get_actors_in_level"), MakeShared<FGetActorsInLevelAction>());
	ActionHandlers.Add(TEXT("find_actors_by_name"), MakeShared<FFindActorsByNameAction>());
	ActionHandlers.Add(TEXT("spawn_actor"), MakeShared<FSpawnActorAction>());
	ActionHandlers.Add(TEXT("delete_actor"), MakeShared<FDeleteActorAction>());
	ActionHandlers.Add(TEXT("set_actor_transform"), MakeShared<FSetActorTransformAction>());
	ActionHandlers.Add(TEXT("get_actor_properties"), MakeShared<FGetActorPropertiesAction>());
	ActionHandlers.Add(TEXT("set_actor_property"), MakeShared<FSetActorPropertyAction>());
	ActionHandlers.Add(TEXT("focus_viewport"), MakeShared<FFocusViewportAction>());
	ActionHandlers.Add(TEXT("get_viewport_transform"), MakeShared<FGetViewportTransformAction>());
	ActionHandlers.Add(TEXT("set_viewport_transform"), MakeShared<FSetViewportTransformAction>());
	ActionHandlers.Add(TEXT("save_all"), MakeShared<FSaveAllAction>());

	// =========================================================================
	// PIE / Runtime Feedback Actions
	// =========================================================================
	ActionHandlers.Add(TEXT("start_pie"), MakeShared<FStartPIEAction>());
	ActionHandlers.Add(TEXT("stop_pie"), MakeShared<FStopPIEAction>());
	ActionHandlers.Add(TEXT("get_pie_status"), MakeShared<FGetPIEStatusAction>());
	ActionHandlers.Add(TEXT("get_output_log"), MakeShared<FGetOutputLogAction>());
	ActionHandlers.Add(TEXT("execute_console_command"), MakeShared<FExecuteConsoleCommandAction>());
	ActionHandlers.Add(TEXT("call_actor_function"), MakeShared<FCallActorFunctionAction>());
	ActionHandlers.Add(TEXT("inject_input"), MakeShared<FInjectInputAction>());
	ActionHandlers.Add(TEXT("check_condition"), MakeShared<FCheckConditionAction>());
	ActionHandlers.Add(TEXT("take_screenshot"), MakeShared<FTakeScreenshotAction>());

	// Automation Test Framework (Functional Tests, Project.* tests)
	ActionHandlers.Add(TEXT("list_automation_tests"), MakeShared<FListAutomationTestsAction>());
	ActionHandlers.Add(TEXT("run_automation_tests"), MakeShared<FRunAutomationTestsAction>());
	ActionHandlers.Add(TEXT("get_automation_results"), MakeShared<FGetAutomationResultsAction>());

	// =========================================================================
	// Node Actions - Graph Operations
	// =========================================================================
	ActionHandlers.Add(TEXT("connect_blueprint_nodes"), MakeShared<FConnectBlueprintNodesAction>());
	ActionHandlers.Add(TEXT("find_blueprint_nodes"), MakeShared<FFindBlueprintNodesAction>());
	ActionHandlers.Add(TEXT("delete_blueprint_node"), MakeShared<FDeleteBlueprintNodeAction>());
	ActionHandlers.Add(TEXT("get_node_pins"), MakeShared<FGetNodePinsAction>());
	ActionHandlers.Add(TEXT("get_graph_connections"), MakeShared<FGetGraphConnectionsAction>());
	ActionHandlers.Add(TEXT("convert_to_validated_get"), MakeShared<FSetVariableGetPurityAction>());
	ActionHandlers.Add(TEXT("set_node_position"), MakeShared<FSetNodePositionAction>());

	// =========================================================================
	// Node Actions - Event Nodes
	// =========================================================================
	ActionHandlers.Add(TEXT("add_blueprint_event_node"), MakeShared<FAddBlueprintEventNodeAction>());
	ActionHandlers.Add(TEXT("add_blueprint_input_action_node"), MakeShared<FAddBlueprintInputActionNodeAction>());
#if MCP_WITH_ENHANCED_INPUT
	ActionHandlers.Add(TEXT("add_enhanced_input_action_node"), MakeShared<FAddEnhancedInputActionNodeAction>());
#endif
	ActionHandlers.Add(TEXT("add_blueprint_custom_event"), MakeShared<FAddBlueprintCustomEventAction>());

	// =========================================================================
	// Node Actions - Variable Nodes
	// =========================================================================
	ActionHandlers.Add(TEXT("add_blueprint_variable"), MakeShared<FAddBlueprintVariableAction>());
	ActionHandlers.Add(TEXT("add_blueprint_variable_get"), MakeShared<FAddBlueprintVariableGetAction>());
	ActionHandlers.Add(TEXT("add_blueprint_variable_set"), MakeShared<FAddBlueprintVariableSetAction>());
	ActionHandlers.Add(TEXT("set_node_pin_default"), MakeShared<FSetNodePinDefaultAction>());

	// =========================================================================
	// Node Actions - Function Nodes
	// =========================================================================
	ActionHandlers.Add(TEXT("add_blueprint_function_node"), MakeShared<FAddBlueprintFunctionNodeAction>());
	ActionHandlers.Add(TEXT("add_blueprint_self_reference"), MakeShared<FAddBlueprintSelfReferenceAction>());
	ActionHandlers.Add(TEXT("add_blueprint_get_self_component_reference"), MakeShared<FAddBlueprintGetSelfComponentReferenceAction>());
	ActionHandlers.Add(TEXT("add_blueprint_branch_node"), MakeShared<FAddBlueprintBranchNodeAction>());
	ActionHandlers.Add(TEXT("add_blueprint_cast_node"), MakeShared<FAddBlueprintCastNodeAction>());
	ActionHandlers.Add(TEXT("add_blueprint_get_subsystem_node"), MakeShared<FAddBlueprintGetSubsystemNodeAction>());

	// =========================================================================
	// Node Actions - Blueprint Function Graph
	// =========================================================================
	ActionHandlers.Add(TEXT("create_blueprint_function"), MakeShared<FCreateBlueprintFunctionAction>());

	// =========================================================================
	// Node Actions - Event Dispatchers
	// =========================================================================
	ActionHandlers.Add(TEXT("add_event_dispatcher"), MakeShared<FAddEventDispatcherAction>());
	ActionHandlers.Add(TEXT("call_event_dispatcher"), MakeShared<FCallEventDispatcherAction>());
	ActionHandlers.Add(TEXT("bind_event_dispatcher"), MakeShared<FBindEventDispatcherAction>());

	// =========================================================================
	// Node Actions - Spawn Actor Nodes
	// =========================================================================
	ActionHandlers.Add(TEXT("add_spawn_actor_from_class_node"), MakeShared<FAddSpawnActorFromClassNodeAction>());
	ActionHandlers.Add(TEXT("call_blueprint_function"), MakeShared<FCallBlueprintFunctionAction>());

	// =========================================================================
	// Node Actions - External Object Property Nodes
	// =========================================================================
	ActionHandlers.Add(TEXT("set_object_property"), MakeShared<FSetObjectPropertyAction>());

	// =========================================================================
	// Node Actions - Macro Instance Nodes
	// =========================================================================
	ActionHandlers.Add(TEXT("add_macro_instance_node"), MakeShared<FAddMacroInstanceNodeAction>());

	// =========================================================================
	// Project Actions (Input Mappings, Enhanced Input)
	// =========================================================================
	ActionHandlers.Add(TEXT("create_input_mapping"), MakeShared<FCreateInputMappingAction>());
#if MCP_WITH_ENHANCED_INPUT
	ActionHandlers.Add(TEXT("create_input_action"), MakeShared<FCreateInputActionAction>());
	ActionHandlers.Add(TEXT("create_input_mapping_context"), MakeShared<FCreateInputMappingContextAction>());
	ActionHandlers.Add(TEXT("add_key_mapping_to_context"), MakeShared<FAddKeyMappingToContextAction>());
#endif

	// =========================================================================
	// UMG Actions (Widget Blueprints)
	// =========================================================================
	ActionHandlers.Add(TEXT("create_umg_widget_blueprint"), MakeShared<FCreateUMGWidgetBlueprintAction>());
	ActionHandlers.Add(TEXT("add_text_block_to_widget"), MakeShared<FAddTextBlockToWidgetAction>());
	ActionHandlers.Add(TEXT("add_button_to_widget"), MakeShared<FAddButtonToWidgetAction>());
	ActionHandlers.Add(TEXT("bind_widget_event"), MakeShared<FBindWidgetEventAction>());
	ActionHandlers.Add(TEXT("add_widget_to_viewport"), MakeShared<FAddWidgetToViewportAction>());
	ActionHandlers.Add(TEXT("set_text_block_binding"), MakeShared<FSetTextBlockBindingAction>());

	// =========================================================================
	// Material Actions (Materials, Shaders, Post-Process)
	// =========================================================================
	ActionHandlers.Add(TEXT("create_material"), MakeShared<FCreateMaterialAction>());
	ActionHandlers.Add(TEXT("set_material_property"), MakeShared<FSetMaterialPropertyAction>());
	ActionHandlers.Add(TEXT("add_material_expression"), MakeShared<FAddMaterialExpressionAction>());
	ActionHandlers.Add(TEXT("connect_material_expressions"), MakeShared<FConnectMaterialExpressionsAction>());
	ActionHandlers.Add(TEXT("connect_to_material_output"), MakeShared<FConnectToMaterialOutputAction>());
	ActionHandlers.Add(TEXT("set_material_expression_property"), MakeShared<FSetMaterialExpressionPropertyAction>());
	ActionHandlers.Add(TEXT("compile_material"), MakeShared<FCompileMaterialAction>());
	ActionHandlers.Add(TEXT("create_material_instance"), MakeShared<FCreateMaterialInstanceAction>());
	ActionHandlers.Add(TEXT("create_post_process_volume"), MakeShared<FCreatePostProcessVolumeAction>());

	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Registered %d action handlers"), ActionHandlers.Num());
}

TSharedRef<FEditorAction>* UMCPBridge::FindAction(const FString& CommandType)
{
	return ActionHandlers.Find(CommandType);
}

namespace
{
	// Actions answer either {"success": true, ...} or {"status": "success", "result": {...}}
	bool IsSuccessResponse(const TSharedPtr<FJsonObject>& Response)
	{
		bool bSuccess = false;
		FString Status;
		return Response.IsValid()
			&& ((Response->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess)
				|| (Response->TryGetStringField(TEXT("status"), Status) && Status == TEXT("success")));
	}

	TSharedPtr<FJsonValue> FindResultField(const TSharedPtr<FJsonObject>& Response, const FString& Field)
	{
		if (TSharedPtr<FJsonValue> Value = Response->TryGetField(Field))
		{
			return Value;
		}
		const TSharedPtr<FJsonObject>* Result;
		return Response->TryGetObjectField(TEXT("result"), Result) ? (*Result)->TryGetField(Field) : nullptr;
	}

	// Replaces string values "$N.field" by that field of the result of command N
	TSharedPtr<FJsonValue> ResolveReferences(const TSharedPtr<FJsonValue>& Value, const TArray<TSharedPtr<FJsonObject>>& Results, FString& OutError)
	{
		FString String;
		if (Value->TryGetString(String) && String.StartsWith(TEXT("$")))
		{
			FString Index, Field;
			if (String.Mid(1).Split(TEXT("."), &Index, &Field) && Index.IsNumeric())
			{
				const int32 ResultIndex = FCString::Atoi(*Index);
				TSharedPtr<FJsonValue> Found = Results.IsValidIndex(ResultIndex) ? FindResultField(Results[ResultIndex], Field) : nullptr;
				if (!Found)
				{
					OutError = FString::Printf(TEXT("Reference '%s' not found"), *String);
					return Value;
				}
				return Found;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Array;
		if (Value->TryGetArray(Array))
		{
			TArray<TSharedPtr<FJsonValue>> NewArray;
			for (const TSharedPtr<FJsonValue>& Item : *Array)
			{
				NewArray.Add(ResolveReferences(Item, Results, OutError));
			}
			return MakeShared<FJsonValueArray>(NewArray);
		}
		const TSharedPtr<FJsonObject>* Object;
		if (Value->TryGetObject(Object))
		{
			TSharedPtr<FJsonObject> NewObject = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
			{
				NewObject->SetField(Pair.Key, ResolveReferences(Pair.Value, Results, OutError));
			}
			return MakeShared<FJsonValueObject>(NewObject);
		}
		return Value;
	}
}

TSharedPtr<FJsonObject> UMCPBridge::ExecuteBatch(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* Commands;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("commands"), Commands))
	{
		return CreateErrorResponse(TEXT("batch needs 'commands': [{\"type\": ..., \"params\": {...}}, ...]"), TEXT("validation_failed"));
	}
	bool bStopOnError = true;
	Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

	TArray<TSharedPtr<FJsonObject>> Results;
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 Failed = 0;
	for (const TSharedPtr<FJsonValue>& CommandValue : *Commands)
	{
		const TSharedPtr<FJsonObject>* Command;
		FString Type;
		TSharedPtr<FJsonObject> Response;
		if (!CommandValue->TryGetObject(Command) || !(*Command)->TryGetStringField(TEXT("type"), Type) || Type == TEXT("batch"))
		{
			Response = CreateErrorResponse(TEXT("Each command needs a 'type' (no nested batch)"), TEXT("validation_failed"));
		}
		else
		{
			const TSharedPtr<FJsonObject>* CommandParams;
			TSharedPtr<FJsonObject> Resolved = MakeShared<FJsonObject>();
			FString Error;
			if ((*Command)->TryGetObjectField(TEXT("params"), CommandParams))
			{
				Resolved = ResolveReferences(MakeShared<FJsonValueObject>(*CommandParams), Results, Error)->AsObject();
			}
			Response = Error.IsEmpty() ? ExecuteCommandSafe(Type, Resolved) : CreateErrorResponse(Error, TEXT("validation_failed"));
		}

		Results.Add(Response);
		ResultsArray.Add(MakeShared<FJsonValueObject>(Response));
		if (!IsSuccessResponse(Response))
		{
			++Failed;
			if (bStopOnError) break;
		}
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetNumberField(TEXT("executed"), Results.Num());
	ResultData->SetNumberField(TEXT("failed"), Failed);
	ResultData->SetArrayField(TEXT("results"), ResultsArray);
	if (Failed == 0)
	{
		return CreateSuccessResponse(ResultData);
	}
	TSharedPtr<FJsonObject> Response = CreateErrorResponse(FString::Printf(TEXT("%d command(s) failed, see results"), Failed), TEXT("batch_failed"));
	Response->SetObjectField(TEXT("result"), ResultData);
	return Response;
}

TSharedPtr<FJsonObject> UMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("batch"))
	{
		return ExecuteBatch(Params);
	}

	// =========================================================================
	// Action Handlers (modular actions - check these first)
	// =========================================================================
	TSharedRef<FEditorAction>* ActionPtr = FindAction(CommandType);
	if (ActionPtr)
	{
		return (*ActionPtr)->Execute(Params, Context);
	}

	// =========================================================================
	// Unknown Command (all handlers should be registered as actions now)
	// =========================================================================
	return CreateErrorResponse(
		FString::Printf(TEXT("Unknown command type: %s"), *CommandType),
		TEXT("unknown_command")
	);
}

TSharedPtr<FJsonObject> UMCPBridge::ExecuteCommandSafe(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	// TODO Phase 2: Add SEH crash protection here
	// For now, rely on defensive programming (validation in actions)
	return ExecuteCommandInternal(CommandType, Params);
}

TSharedPtr<FJsonObject> UMCPBridge::ExecuteCommandInternal(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteCommand(CommandType, Params);
}

TSharedPtr<FJsonObject> UMCPBridge::CreateSuccessResponse(const TSharedPtr<FJsonObject>& ResultData)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("status"), TEXT("success"));

	if (ResultData.IsValid())
	{
		Response->SetObjectField(TEXT("result"), ResultData);
	}
	else
	{
		Response->SetObjectField(TEXT("result"), MakeShared<FJsonObject>());
	}

	return Response;
}

TSharedPtr<FJsonObject> UMCPBridge::CreateErrorResponse(const FString& ErrorMessage, const FString& ErrorType)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("status"), TEXT("error"));
	Response->SetStringField(TEXT("error"), ErrorMessage);
	Response->SetStringField(TEXT("error_type"), ErrorType);

	return Response;
}
