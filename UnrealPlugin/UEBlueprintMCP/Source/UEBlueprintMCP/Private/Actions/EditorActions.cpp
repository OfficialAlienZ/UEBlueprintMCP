// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/EditorActions.h"
#include "MCPCommonUtils.h"
#include "MCPInspectionUtils.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "Algo/Sort.h"
#include "FileHelpers.h"
#include "UObject/SavePackage.h"


// Helper to find actor by name (stops at the first match)
static AActor* FindActorByName(UWorld* World, const FString& ActorName)
{
	if (!World) return nullptr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetName() == ActorName)
		{
			return Actor;
		}
	}
	return nullptr;
}

namespace
{
	constexpr int32 DefaultActorQueryLimit = 100;
	constexpr int32 MaxActorQueryLimit = 1000;

	// True if the actor's class, or any parent class, matches ClassFilter (case-insensitive).
	// Blueprint classes match with or without the "_C" suffix.
	bool ActorMatchesClass(const AActor* Actor, const FString& ClassFilter)
	{
		for (const UClass* Class = Actor->GetClass(); Class; Class = Class->GetSuperClass())
		{
			const FString ClassName = Class->GetName();
			if (ClassName.Equals(ClassFilter, ESearchCase::IgnoreCase))
			{
				return true;
			}
			if (ClassName.EndsWith(TEXT("_C")) && ClassName.LeftChop(2).Equals(ClassFilter, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	void MergeJsonFields(const TSharedPtr<FJsonObject>& Target, const TSharedPtr<FJsonObject>& Source)
	{
		if (!Target.IsValid() || !Source.IsValid()) return;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
		{
			Target->SetField(Pair.Key, Pair.Value);
		}
	}

	// Shared by get_actors_in_level and find_actors_by_name.
	// Filters, sorts by name (stable pagination), then serializes only the requested page.
	TSharedPtr<FJsonObject> QueryActors(
		UWorld* World,
		const FString& NamePattern,
		const FString& ClassFilter,
		int32 Limit,
		int32 Offset,
		bool bIncludeTransform,
		bool bSummaryOnly,
		bool bIncludeDetails)
	{
		TArray<AActor*> Matches;
		TMap<FString, int32> ClassCounts;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			if (!NamePattern.IsEmpty() && !Actor->GetName().Contains(NamePattern)) continue;
			if (!ClassFilter.IsEmpty() && !ActorMatchesClass(Actor, ClassFilter)) continue;

			if (bSummaryOnly)
			{
				ClassCounts.FindOrAdd(Actor->GetClass()->GetName())++;
			}
			Matches.Add(Actor);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("total_count"), Matches.Num());

		if (bSummaryOnly)
		{
			ClassCounts.ValueSort([](int32 A, int32 B) { return A > B; });
			TArray<TSharedPtr<FJsonValue>> CountArray;
			for (const TPair<FString, int32>& Pair : ClassCounts)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("class"), Pair.Key);
				Entry->SetNumberField(TEXT("count"), Pair.Value);
				CountArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetArrayField(TEXT("class_counts"), CountArray);
			return Result;
		}

		Algo::Sort(Matches, [](const AActor* A, const AActor* B)
		{
			return A->GetName().Compare(B->GetName()) < 0;
		});

		const int32 Start = FMath::Min(Offset, Matches.Num());
		const int32 End = FMath::Min(Start + Limit, Matches.Num());

		TArray<TSharedPtr<FJsonValue>> ActorArray;
		for (int32 i = Start; i < End; ++i)
		{
			AActor* Actor = Matches[i];
			if (bIncludeDetails)
			{
				TSharedPtr<FJsonObject> Entry = FMCPCommonUtils::ActorToJsonObject(Actor);
				MergeJsonFields(Entry, FMCPInspectionUtils::ActorDetailsToJson(Actor, false));
				ActorArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
			else if (bIncludeTransform)
			{
				ActorArray.Add(FMCPCommonUtils::ActorToJsonValue(Actor));
			}
			else
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Actor->GetName());
				Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				ActorArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}

		Result->SetArrayField(TEXT("actors"), ActorArray);
		Result->SetNumberField(TEXT("returned_count"), ActorArray.Num());
		Result->SetNumberField(TEXT("offset"), Start);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetBoolField(TEXT("truncated"), End < Matches.Num());
		return Result;
	}
}


// ============================================================================
// FGetActorsInLevelAction
// ============================================================================

bool FGetActorsInLevelAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	return ValidateWorldParam(Params, OutError);
}

TSharedPtr<FJsonObject> FGetActorsInLevelAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString WorldError;
	UWorld* World = ResolveWorld(Params, WorldError);
	if (!World)
	{
		return CreateErrorResponse(WorldError, TEXT("no_world"));
	}

	const int32 Limit = FMath::Clamp((int32)GetOptionalNumber(Params, TEXT("limit"), DefaultActorQueryLimit), 1, MaxActorQueryLimit);
	const int32 Offset = FMath::Max(0, (int32)GetOptionalNumber(Params, TEXT("offset"), 0));

	return CreateSuccessResponse(QueryActors(
		World,
		FString(),
		GetOptionalString(Params, TEXT("class_filter")),
		Limit,
		Offset,
		GetOptionalBool(Params, TEXT("include_transform"), false),
		GetOptionalBool(Params, TEXT("summary_only"), false),
		GetOptionalBool(Params, TEXT("include_details"), false)));
}


// ============================================================================
// FFindActorsByNameAction
// ============================================================================

bool FFindActorsByNameAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Pattern;
	return GetRequiredString(Params, TEXT("pattern"), Pattern, OutError) && ValidateWorldParam(Params, OutError);
}

TSharedPtr<FJsonObject> FFindActorsByNameAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString Pattern, Error;
	GetRequiredString(Params, TEXT("pattern"), Pattern, Error);

	UWorld* World = ResolveWorld(Params, Error);
	if (!World)
	{
		return CreateErrorResponse(Error, TEXT("no_world"));
	}

	const int32 Limit = FMath::Clamp((int32)GetOptionalNumber(Params, TEXT("limit"), DefaultActorQueryLimit), 1, MaxActorQueryLimit);
	const int32 Offset = FMath::Max(0, (int32)GetOptionalNumber(Params, TEXT("offset"), 0));

	return CreateSuccessResponse(QueryActors(
		World,
		Pattern,
		GetOptionalString(Params, TEXT("class_filter")),
		Limit,
		Offset,
		GetOptionalBool(Params, TEXT("include_transform"), false),
		GetOptionalBool(Params, TEXT("summary_only"), false),
		GetOptionalBool(Params, TEXT("include_details"), false)));
}


// ============================================================================
// FSpawnActorAction
// ============================================================================

bool FSpawnActorAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Name, Type;
	if (!GetRequiredString(Params, TEXT("name"), Name, OutError)) return false;
	if (!GetRequiredString(Params, TEXT("type"), Type, OutError)) return false;
	return true;
}

TSharedPtr<FJsonObject> FSpawnActorAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString ActorName, ActorType, Error;
	GetRequiredString(Params, TEXT("name"), ActorName, Error);
	GetRequiredString(Params, TEXT("type"), ActorType, Error);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return CreateErrorResponse(TEXT("No editor world available"), TEXT("no_world"));
	}

	// Resolve actor class
	UClass* ActorClass = ResolveActorClass(ActorType);
	if (!ActorClass)
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("Unknown actor type: %s"), *ActorType),
			TEXT("invalid_type")
		);
	}

	// Delete existing actor with same name
	AActor* Existing = FindActorByName(World, ActorName);
	if (Existing)
	{
		World->DestroyActor(Existing);
	}

	// Parse transform
	FVector Location = FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
	FRotator Rotation = FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
	FVector Scale = Params->HasField(TEXT("scale")) ?
		FMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")) : FVector(1, 1, 1);

	// Spawn
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(*ActorName);
	SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;

	AActor* NewActor = World->SpawnActor<AActor>(ActorClass, Location, Rotation, SpawnParams);
	if (!NewActor)
	{
		return CreateErrorResponse(TEXT("Failed to spawn actor"), TEXT("spawn_failed"));
	}

	NewActor->SetActorScale3D(Scale);
	NewActor->SetActorLabel(*ActorName);
	Context.LastCreatedActorName = ActorName;

	// Mark level dirty so auto-save works
	Context.MarkPackageDirty(World->GetOutermost());

	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Spawned actor '%s' of type '%s'"), *ActorName, *ActorType);

	return CreateSuccessResponse(FMCPCommonUtils::ActorToJsonObject(NewActor));
}

UClass* FSpawnActorAction::ResolveActorClass(const FString& TypeName) const
{
	if (TypeName == TEXT("StaticMeshActor")) return AStaticMeshActor::StaticClass();
	if (TypeName == TEXT("PointLight")) return APointLight::StaticClass();
	if (TypeName == TEXT("SpotLight")) return ASpotLight::StaticClass();
	if (TypeName == TEXT("DirectionalLight")) return ADirectionalLight::StaticClass();
	if (TypeName == TEXT("CameraActor")) return ACameraActor::StaticClass();
	if (TypeName == TEXT("Actor")) return AActor::StaticClass();
	return nullptr;
}


// ============================================================================
// FDeleteActorAction
// ============================================================================

bool FDeleteActorAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Name;
	return GetRequiredString(Params, TEXT("name"), Name, OutError);
}

TSharedPtr<FJsonObject> FDeleteActorAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString ActorName, Error;
	GetRequiredString(Params, TEXT("name"), ActorName, Error);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return CreateErrorResponse(TEXT("No editor world available"), TEXT("no_world"));
	}

	AActor* Actor = FindActorByName(World, ActorName);
	if (!Actor)
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName),
			TEXT("not_found")
		);
	}

	// Store info before deletion
	TSharedPtr<FJsonObject> ActorInfo = FMCPCommonUtils::ActorToJsonObject(Actor);

	// Use editor-proper destruction which handles World Partition external actors
	bool bDestroyed = World->EditorDestroyActor(Actor, true);
	if (!bDestroyed)
	{
		// Fallback to regular destroy
		Actor->Destroy();
	}

	// Mark world dirty
	Context.MarkPackageDirty(World->GetOutermost());

	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Deleted actor '%s'"), *ActorName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("deleted_actor"), ActorInfo);
	return CreateSuccessResponse(Result);
}


// ============================================================================
// FSetActorTransformAction
// ============================================================================

bool FSetActorTransformAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Name;
	return GetRequiredString(Params, TEXT("name"), Name, OutError) && ValidateEditableWorldParam(Params, OutError);
}

TSharedPtr<FJsonObject> FSetActorTransformAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString ActorName, Error;
	GetRequiredString(Params, TEXT("name"), ActorName, Error);

	bool bIsPIE = false;
	UWorld* World = ResolveWorld(Params, Error, &bIsPIE);
	if (!World)
	{
		return CreateErrorResponse(Error, TEXT("no_world"));
	}

	AActor* Actor = FMCPCommonUtils::FindActor(World, ActorName);
	if (!Actor)
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName),
			TEXT("not_found")
		);
	}

	// Update transform
	FTransform Transform = Actor->GetTransform();

	if (Params->HasField(TEXT("location")))
	{
		Transform.SetLocation(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
	}
	if (Params->HasField(TEXT("rotation")))
	{
		Transform.SetRotation(FQuat(FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"))));
	}
	if (Params->HasField(TEXT("scale")))
	{
		Transform.SetScale3D(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
	}

	Actor->SetActorTransform(Transform);

	// Mark level dirty so auto-save works (the PIE world is a throwaway copy)
	if (!bIsPIE)
	{
		Context.MarkPackageDirty(World->GetOutermost());
	}

	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Set transform on actor '%s'"), *ActorName);

	return CreateSuccessResponse(FMCPCommonUtils::ActorToJsonObject(Actor));
}


// ============================================================================
// FGetActorPropertiesAction
// ============================================================================

bool FGetActorPropertiesAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Name;
	return GetRequiredString(Params, TEXT("name"), Name, OutError) && ValidateWorldParam(Params, OutError);
}

TSharedPtr<FJsonObject> FGetActorPropertiesAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString ActorName, Error;
	GetRequiredString(Params, TEXT("name"), ActorName, Error);

	UWorld* World = ResolveWorld(Params, Error);
	if (!World)
	{
		return CreateErrorResponse(Error, TEXT("no_world"));
	}

	AActor* Actor = FMCPCommonUtils::FindActor(World, ActorName);
	if (!Actor)
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName),
			TEXT("not_found")
		);
	}

	TSharedPtr<FJsonObject> Result = FMCPCommonUtils::ActorToJsonObject(Actor);
	MergeJsonFields(Result, FMCPInspectionUtils::ActorDetailsToJson(Actor, GetOptionalBool(Params, TEXT("include_properties"), true)));

	// Every reflected property (Export Text), e.g. WorldSettings.DefaultGameMode.
	TArray<FString> PropertyNames;
	if (const TArray<TSharedPtr<FJsonValue>>* Names = GetOptionalArray(Params, TEXT("property_names")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Names)
		{
			PropertyNames.Add(Value->AsString());
		}
	}
	if (GetOptionalBool(Params, TEXT("include_all_properties"), false) || PropertyNames.Num() > 0)
	{
		TArray<FString> Unknown;
		Result->SetObjectField(TEXT("all_properties"),
			FMCPInspectionUtils::ExportAllProperties(Actor, PropertyNames, GetOptionalBool(Params, TEXT("editable_only"), false), &Unknown));
		if (Unknown.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> UnknownValues;
			for (const FString& Name : Unknown)
			{
				UnknownValues.Add(MakeShared<FJsonValueString>(Name));
			}
			Result->SetArrayField(TEXT("unknown_properties"), UnknownValues);
		}
	}
	return CreateSuccessResponse(Result);
}


// ============================================================================
// FSetActorPropertyAction
// ============================================================================

bool FSetActorPropertyAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Name, PropertyName;
	if (!GetRequiredString(Params, TEXT("name"), Name, OutError)) return false;
	if (!GetRequiredString(Params, TEXT("property_name"), PropertyName, OutError)) return false;
	if (!Params->HasField(TEXT("property_value")))
	{
		OutError = TEXT("Missing 'property_value' parameter");
		return false;
	}
	return ValidateEditableWorldParam(Params, OutError);
}

TSharedPtr<FJsonObject> FSetActorPropertyAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString ActorName, PropertyName, Error;
	GetRequiredString(Params, TEXT("name"), ActorName, Error);
	GetRequiredString(Params, TEXT("property_name"), PropertyName, Error);

	bool bIsPIE = false;
	UWorld* World = ResolveWorld(Params, Error, &bIsPIE);
	if (!World)
	{
		return CreateErrorResponse(Error, TEXT("no_world"));
	}

	AActor* Actor = FMCPCommonUtils::FindActor(World, ActorName);
	if (!Actor)
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("Actor not found: %s"), *ActorName),
			TEXT("not_found")
		);
	}

	TSharedPtr<FJsonValue> JsonValue = Params->Values.FindRef(TEXT("property_value"));

	FString ErrorMessage;
	if (!FMCPCommonUtils::SetObjectProperty(Actor, PropertyName, JsonValue, ErrorMessage))
	{
		return CreateErrorResponse(ErrorMessage, TEXT("property_set_failed"));
	}

	// Mark level dirty so auto-save works (the PIE world is a throwaway copy)
	if (!bIsPIE)
	{
		Context.MarkPackageDirty(World->GetOutermost());
	}

	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Set property '%s' on actor '%s'"), *PropertyName, *ActorName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), ActorName);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetBoolField(TEXT("success"), true);
	return CreateSuccessResponse(Result);
}


// ============================================================================
// FFocusViewportAction
// ============================================================================

bool FFocusViewportAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	bool HasTarget = Params->HasField(TEXT("target"));
	bool HasLocation = Params->HasField(TEXT("location"));

	if (!HasTarget && !HasLocation)
	{
		OutError = TEXT("Either 'target' or 'location' must be provided");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FFocusViewportAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetActiveViewport())
	{
		ViewportClient = (FLevelEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
	}

	if (!ViewportClient)
	{
		return CreateErrorResponse(TEXT("Failed to get active viewport"), TEXT("no_viewport"));
	}

	float Distance = GetOptionalNumber(Params, TEXT("distance"), 1000.0f);
	FVector TargetLocation(0, 0, 0);

	if (Params->HasField(TEXT("target")))
	{
		FString TargetActorName = Params->GetStringField(TEXT("target"));
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		AActor* TargetActor = FindActorByName(World, TargetActorName);

		if (!TargetActor)
		{
			return CreateErrorResponse(
				FString::Printf(TEXT("Actor not found: %s"), *TargetActorName),
				TEXT("not_found")
			);
		}
		TargetLocation = TargetActor->GetActorLocation();
	}
	else
	{
		TargetLocation = FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
	}

	ViewportClient->SetViewLocation(TargetLocation - FVector(Distance, 0, 0));

	if (Params->HasField(TEXT("orientation")))
	{
		ViewportClient->SetViewRotation(FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("orientation")));
	}

	ViewportClient->Invalidate();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return CreateSuccessResponse(Result);
}


// ============================================================================
// FGetViewportTransformAction
// ============================================================================

TSharedPtr<FJsonObject> FGetViewportTransformAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetActiveViewport())
	{
		ViewportClient = (FLevelEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
	}

	if (!ViewportClient)
	{
		return CreateErrorResponse(TEXT("Failed to get active viewport"), TEXT("no_viewport"));
	}

	FVector Location = ViewportClient->GetViewLocation();
	FRotator Rotation = ViewportClient->GetViewRotation();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> LocationArray;
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
	Result->SetArrayField(TEXT("location"), LocationArray);

	TArray<TSharedPtr<FJsonValue>> RotationArray;
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
	Result->SetArrayField(TEXT("rotation"), RotationArray);

	return CreateSuccessResponse(Result);
}


// ============================================================================
// FSetViewportTransformAction
// ============================================================================

bool FSetViewportTransformAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("location")) && !Params->HasField(TEXT("rotation")))
	{
		OutError = TEXT("At least 'location' or 'rotation' must be provided");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FSetViewportTransformAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetActiveViewport())
	{
		ViewportClient = (FLevelEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
	}

	if (!ViewportClient)
	{
		return CreateErrorResponse(TEXT("Failed to get active viewport"), TEXT("no_viewport"));
	}

	if (Params->HasField(TEXT("location")))
	{
		ViewportClient->SetViewLocation(FMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
	}
	if (Params->HasField(TEXT("rotation")))
	{
		ViewportClient->SetViewRotation(FMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation")));
	}

	ViewportClient->Invalidate();

	// Return new state
	FVector Location = ViewportClient->GetViewLocation();
	FRotator Rotation = ViewportClient->GetViewRotation();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> LocationArray;
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
	Result->SetArrayField(TEXT("location"), LocationArray);

	TArray<TSharedPtr<FJsonValue>> RotationArray;
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
	Result->SetArrayField(TEXT("rotation"), RotationArray);

	return CreateSuccessResponse(Result);
}


// ============================================================================
// FSaveAllAction
// ============================================================================

TSharedPtr<FJsonObject> FSaveAllAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	bool bOnlyMaps = GetOptionalBool(Params, TEXT("only_maps"), false);

	int32 SavedCount = 0;
	TArray<FString> SavedPackages;

	if (bOnlyMaps)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World)
		{
			UPackage* WorldPackage = World->GetOutermost();
			if (WorldPackage && WorldPackage->IsDirty())
			{
				FString PackageFilename;
				if (FPackageName::TryConvertLongPackageNameToFilename(
					WorldPackage->GetName(), PackageFilename, FPackageName::GetMapPackageExtension()))
				{
					if (MCPCompat::SavePackageToDisk(WorldPackage, World, PackageFilename, RF_Standalone))
					{
						SavedCount++;
						SavedPackages.Add(WorldPackage->GetName());
					}
				}
			}
		}
	}
	else
	{
		TArray<UPackage*> DirtyPackages;
		MCPCompat::GetDirtyPackages(DirtyPackages);

		for (UPackage* Package : DirtyPackages)
		{
			if (!Package) continue;

			FString PackageFilename;
			FString PackageName = Package->GetName();
			bool bIsMap = Package->ContainsMap();
			FString Extension = bIsMap ?
				FPackageName::GetMapPackageExtension() :
				FPackageName::GetAssetPackageExtension();

			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, Extension))
			{
				UObject* AssetToSave = bIsMap ? Package->FindAssetInPackage() : nullptr;

				if (MCPCompat::SavePackageToDisk(Package, AssetToSave, PackageFilename, RF_Standalone))
				{
					SavedCount++;
					SavedPackages.Add(PackageName);
					UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP SaveAll: Saved %s"), *PackageName);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("saved_count"), SavedCount);

	TArray<TSharedPtr<FJsonValue>> PackagesArray;
	for (const FString& PkgName : SavedPackages)
	{
		PackagesArray.Add(MakeShared<FJsonValueString>(PkgName));
	}
	Result->SetArrayField(TEXT("saved_packages"), PackagesArray);

	return CreateSuccessResponse(Result);
}


// =========================================================================
// FSetAssetPropertyAction
// =========================================================================

bool FSetAssetPropertyAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Value;
	if (!GetRequiredString(Params, TEXT("object_path"), Value, OutError)) return false;
	if (!GetRequiredString(Params, TEXT("property_name"), Value, OutError)) return false;
	if (!Params->HasField(TEXT("property_value")))
	{
		OutError = TEXT("Missing 'property_value'");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FSetAssetPropertyAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString ObjectPath = Params->GetStringField(TEXT("object_path"));
	const FString PropertyName = Params->GetStringField(TEXT("property_name"));

	UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath);
	if (!Object)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Object not found: %s"), *ObjectPath), TEXT("not_found"));
	}
	FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Property not found: %s"), *PropertyName), TEXT("not_found"));
	}

	// Structs take Unreal text with only the fields to change, e.g. NaniteSettings = (bEnabled=True)
	Object->Modify();
	Object->PreEditChange(Property);
	FString Error;
	if (!FMCPCommonUtils::SetObjectProperty(Object, PropertyName, Params->TryGetField(TEXT("property_value")), Error))
	{
		return CreateErrorResponse(Error, TEXT("property_set_failed"));
	}
	// Lets the asset react as if edited in its editor (e.g. a Static Mesh rebuilds its Nanite data)
	FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(ChangedEvent);
	Object->MarkPackageDirty();
	Context.MarkPackageDirty(Object->GetOutermost());

	FString NewValue;
	Property->ExportTextItem_Direct(NewValue, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("object"), Object->GetPathName());
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("value"), NewValue);
	return CreateSuccessResponse(Result);
}
