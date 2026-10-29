// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class AActor;
class UObject;
class UClass;
class UStruct;
class UWidgetBlueprint;
class FProperty;
struct FRichCurve;
struct FSimpleCurve;

/**
 * Shared helpers for the read-only introspection actions
 * (get_blueprint_info, get_actor_properties, get_curve_info, get_data_table_info, ...).
 */
class UEBLUEPRINTMCP_API FMCPInspectionUtils
{
public:
	/** Editable properties of Object whose value differs from Baseline (name -> exported text). */
	static TSharedPtr<FJsonObject> ChangedProperties(UObject* Object, UObject* Baseline, const UClass* ExcludeOwner = nullptr);

	/**
	 * Every reflected property (UPROPERTY) of Object as name -> exported text, like the editor's Export Text
	 * (nothing is skipped because it equals the default). Deprecated properties and multicast delegates are omitted;
	 * fixed-size arrays are exported as a JSON array.
	 *
	 * @param NameFilter   If not empty, only these properties (case-insensitive)
	 * @param bEditableOnly Only properties exposed to the editor (CPF_Edit)
	 * @param OutUnknown   Receives the NameFilter entries that match no property of the class
	 */
	static TSharedPtr<FJsonObject> ExportAllProperties(UObject* Object, const TArray<FString>& NameFilter, bool bEditableOnly, TArray<FString>* OutUnknown = nullptr);

	/** Curve as JSON: keys (time/value/interp/tangents) plus pre/post extrapolation. */
	static TSharedPtr<FJsonObject> RichCurveToJson(const FRichCurve& Curve);
	static TSharedPtr<FJsonObject> SimpleCurveToJson(const FSimpleCurve& Curve);

	/**
	 * Find an asset by short name (Asset Registry) or full path (/Game/Folder/Name).
	 * Short-name lookup only matches assets whose class is exactly one of Classes.
	 */
	static UObject* FindAsset(const FString& NameOrPath, const TArray<UClass*>& Classes);

	/**
	 * Value of a reflected property as JSON. Unlike the stock JSON converter this uses the names
	 * authored in the editor for User Defined Structs and the display names of User Defined Enums.
	 */
	static TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr);

	/** Struct instance as JSON keyed by authored field names (see PropertyValueToJson). */
	static TSharedPtr<FJsonObject> StructValueToJson(const UStruct* Struct, const void* StructPtr);

	/** Level placement details of an actor: label, level, folder, tags, attachment, components, overrides. */
	static TSharedPtr<FJsonObject> ActorDetailsToJson(AActor* Actor, bool bIncludeProperties);

	/**
	 * Designer data of a Widget Blueprint: widget tree (hierarchy order, slot settings, changed
	 * properties), property bindings and animations.
	 */
	static TSharedPtr<FJsonObject> WidgetBlueprintToJson(const UWidgetBlueprint* WidgetBlueprint, bool bIncludeProperties);
};
