// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class AActor;
class UK2Node_Event;
class UK2Node_CallFunction;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_Self;
class UK2Node_InputAction;
class USCS_Node;
struct FEdGraphPinType;

/**
 * Common utility functions for MCP commands.
 * These are shared across all action handlers.
 */
class UEBLUEPRINTMCP_API FMCPCommonUtils
{
public:
	// =========================================================================
	// JSON Parsing Utilities
	// =========================================================================

	/** Parse FVector from JSON array field [X, Y, Z] */
	static FVector GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);

	/** Parse FRotator from JSON array field [Pitch, Yaw, Roll] */
	static FRotator GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);

	/** Parse FVector2D from JSON array field [X, Y] */
	static FVector2D GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);

	// =========================================================================
	// Blueprint Utilities
	// =========================================================================

	/** Find a Blueprint by name (searches /Game/Blueprints/) */
	static UBlueprint* FindBlueprint(const FString& BlueprintName);

	/** Find or create the event graph for a Blueprint */
	static UEdGraph* FindOrCreateEventGraph(UBlueprint* Blueprint);

	/** Find a function graph by name */
	static UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName);

	/** Find any graph by name (event graph, function graph, etc.) */
	static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);

	/**
	 * Find a graph that is not an event/function graph: macro graphs, event dispatcher
	 * signature graphs, interface graphs and collapsed (composite) sub-graphs.
	 * Sub-graph names are not unique; the first match wins.
	 */
	static UEdGraph* FindSpecialGraphByName(UBlueprint* Blueprint, const FString& GraphName);

	/** Find a component node in a Blueprint (traverses parent hierarchy) */
	static USCS_Node* FindComponentNode(UBlueprint* Blueprint, const FString& ComponentName);

	// =========================================================================
	// Introspection JSON Serialization
	// =========================================================================

	/** Serialize a pin type: category, sub_category, sub_category_object, container, type (display text) */
	static TSharedPtr<FJsonObject> PinTypeToJson(const FEdGraphPinType& PinType);

	/** Serialize a pin: name, direction, type info, default value and (optionally) linked_to connections */
	static TSharedPtr<FJsonObject> PinToJson(const UEdGraphPin* Pin, bool bIncludeConnections);

	/**
	 * Serialize a node: guid, class, title, position, comment, plus kind-specific info
	 * (function name/owner, variable name, event name, macro, cast target).
	 * When bIncludePins is true, a "pins" array with connections is added (unlinked hidden pins are skipped).
	 */
	static TSharedPtr<FJsonObject> NodeToJson(UEdGraphNode* Node, bool bIncludePins);

	// =========================================================================
	// Property Setting Utilities
	// =========================================================================

	/** Set a property on an object from a JSON value */
	static bool SetObjectProperty(UObject* Object, const FString& PropertyName,
		const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage);

	// =========================================================================
	// Graph Node Utilities
	// =========================================================================

	/** Find a pin on a node by name and direction */
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction);

	/** Find an existing event node in a graph */
	static UK2Node_Event* FindExistingEventNode(UEdGraph* Graph, const FString& EventName);

	/** Create an event node in a graph */
	static UK2Node_Event* CreateEventNode(UEdGraph* Graph, const FString& EventName, FVector2D Position);

	/** Create an input action node in a graph */
	static UK2Node_InputAction* CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, FVector2D Position);

	/** Create a function call node in a graph */
	static UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, FVector2D Position);

	/** Create a self reference node in a graph */
	static UK2Node_Self* CreateSelfReferenceNode(UEdGraph* Graph, FVector2D Position);

	/** Create an error response JSON object */
	static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& ErrorMessage);

	// =========================================================================
	// Actor Utilities
	// =========================================================================

	/** Convert an actor to a JSON object with location/rotation/scale */
	static TSharedPtr<FJsonObject> ActorToJsonObject(AActor* Actor);

	/** Convert an actor to a JSON value */
	static TSharedPtr<FJsonValue> ActorToJsonValue(AActor* Actor);
};
