// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/PerformanceActions.h"
#include "Actions/PIEActions.h"
#include "MCPCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "RHI.h"
#include "RenderCore.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "UnrealEngine.h"

// Declared only function-locally in UnrealEngine.h
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;


// audit_level (split from PerformanceActions.cpp to keep files small).

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> Lvl_FJsonArray;

	TSharedPtr<FJsonValue> Lvl_AsValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}
	void Lvl_AddIssue(Lvl_FJsonArray& Issues, const TCHAR* Severity, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(Lvl_AsValue(Issue));
	}
	/** Reflected property value as export text ("" if the class has no such property). */
	FString Lvl_ReadPropertyText(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return FString();
		}
		const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return FString();
		}
		FString Text;
		Property->ExportTextItem(Text, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
		return Text;
	}
	template <typename EnumType>
	FString Lvl_EnumName(int64 Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			return Enum->GetNameStringByValue(Value);
		}
		return FString::FromInt(static_cast<int32>(Value));
	}
	TSharedPtr<FJsonObject> Lvl_SortedCounts(TMap<FString, int32>& Counts, int32 Limit)
	{
		Counts.ValueSort([](int32 A, int32 B) { return A > B; });
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		int32 Index = 0;
		for (const TPair<FString, int32>& Pair : Counts)
		{
			if (Index++ >= Limit)
			{
				break;
			}
			Result->SetNumberField(Pair.Key, Pair.Value);
		}
		return Result;
	}
	int32 Lvl_CountSeverity(const Lvl_FJsonArray& Issues, const TCHAR* Severity)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Value : Issues)
		{
			FString Found;
			if (Value->AsObject()->TryGetStringField(TEXT("severity"), Found) && Found == Severity)
			{
				++Count;
			}
		}
		return Count;
	}
}

// ============================================================================
// audit_level
// ============================================================================

bool FAuditLevelAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	return ValidateWorldParam(Params, OutError);
}

TSharedPtr<FJsonObject> FAuditLevelAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString Error;
	bool bIsPIE = false;
	UWorld* World = ResolveWorld(Params, Error, &bIsPIE);
	if (!World)
	{
		return CreateErrorResponse(Error, TEXT("not_found"));
	}
	const int32 InstancingThreshold = FMath::Max(2, static_cast<int32>(GetOptionalNumber(Params, TEXT("instancing_threshold"), 10)));

	Lvl_FJsonArray Issues;
	int32 ActorCount = 0;
	int32 TickingActors = 0;
	int32 ComponentCount = 0;
	int32 TickingComponents = 0;
	int32 StaticMeshComponents = 0;
	int32 InstancedMeshComponents = 0;
	int32 MovableMeshes = 0;
	int32 SkinnedAlwaysTick = 0;
	int32 SkinnedMeshes = 0;
	int32 DirectionalLights = 0, PointLights = 0, SpotLights = 0, MovableLights = 0, ShadowCastingMovableLights = 0;
	int32 NavBoundsVolumes = 0, ReflectionCaptures = 0, PostProcessVolumes = 0, ParticleSystems = 0, LightmassVolumes = 0;
	TMap<FString, int32> ClassCounts;
	TMap<FString, int32> TickingClassCounts;
	TMap<FString, int32> MeshUsage;
	Lvl_FJsonArray LargeLights;
	Lvl_FJsonArray NavMeshes;
	Lvl_FJsonArray NavVolumes;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		++ActorCount;
		const FString ClassName = Actor->GetClass()->GetName();
		ClassCounts.FindOrAdd(ClassName)++;
		if (Actor->PrimaryActorTick.bCanEverTick && Actor->PrimaryActorTick.bStartWithTickEnabled)
		{
			++TickingActors;
			TickingClassCounts.FindOrAdd(ClassName)++;
		}
		if (ClassName.Contains(TEXT("NavMeshBoundsVolume")))
		{
			++NavBoundsVolumes;
			const FVector Size = Actor->GetComponentsBoundingBox().GetSize();
			TSharedPtr<FJsonObject> Volume = MakeShared<FJsonObject>();
			Volume->SetStringField(TEXT("name"), Actor->GetName());
			Volume->SetNumberField(TEXT("size_x_m"), Size.X / 100.0);
			Volume->SetNumberField(TEXT("size_y_m"), Size.Y / 100.0);
			Volume->SetNumberField(TEXT("size_z_m"), Size.Z / 100.0);
			NavVolumes.Add(Lvl_AsValue(Volume));
			if (Size.X > 200000.0 || Size.Y > 200000.0)
			{
				Lvl_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("NavMeshBoundsVolume '%s' is %.0f x %.0f m: huge navmesh bounds cost build time and memory. Shrink it to the walkable area."), *Actor->GetName(), Size.X / 100.0, Size.Y / 100.0));
			}
		}
		else if (ClassName == TEXT("RecastNavMesh") || ClassName.StartsWith(TEXT("RecastNavMesh")))
		{
			TSharedPtr<FJsonObject> Nav = MakeShared<FJsonObject>();
			Nav->SetStringField(TEXT("name"), Actor->GetName());
			static const TCHAR* NavProperties[] = { TEXT("CellSize"), TEXT("CellHeight"), TEXT("TileSizeUU"), TEXT("AgentRadius"), TEXT("AgentHeight"), TEXT("AgentMaxSlope"), TEXT("AgentMaxStepHeight"), TEXT("bFixedTilePoolSize"), TEXT("TilePoolSize"), TEXT("MaxSimultaneousTileGenerationJobsCount"), TEXT("bDistinctlyDrawTilesBeingBuilt") };
			for (const TCHAR* PropertyName : NavProperties)
			{
				const FString Value = Lvl_ReadPropertyText(Actor, PropertyName);
				if (!Value.IsEmpty())
				{
					Nav->SetStringField(PropertyName, Value);
				}
			}
			NavMeshes.Add(Lvl_AsValue(Nav));
		}
		else if (ClassName.Contains(TEXT("ReflectionCapture")))
		{
			++ReflectionCaptures;
		}
		else if (ClassName.Contains(TEXT("PostProcessVolume")))
		{
			++PostProcessVolumes;
		}
		else if (ClassName.Contains(TEXT("LightmassImportanceVolume")))
		{
			++LightmassVolumes;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			++ComponentCount;
			if (Component->PrimaryComponentTick.bCanEverTick && Component->PrimaryComponentTick.bStartWithTickEnabled)
			{
				++TickingComponents;
			}
			const FString ComponentClass = Component->GetClass()->GetName();
			if (ComponentClass.Contains(TEXT("ParticleSystemComponent")) || ComponentClass.Contains(TEXT("NiagaraComponent")))
			{
				++ParticleSystems;
			}

			if (const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
			{
				++StaticMeshComponents;
				if (MeshComponent->IsA<UInstancedStaticMeshComponent>())
				{
					++InstancedMeshComponents;
				}
				else if (const UStaticMesh* Mesh = MeshComponent->GetStaticMesh())
				{
					MeshUsage.FindOrAdd(Mesh->GetPathName())++;
				}
				if (MeshComponent->Mobility == EComponentMobility::Movable)
				{
					++MovableMeshes;
				}
			}
			else if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
			{
				++SkinnedMeshes;
				if (Skinned->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones)
				{
					++SkinnedAlwaysTick;
				}
			}
			else if (const ULightComponent* Light = Cast<ULightComponent>(Component))
			{
				const bool bMovable = Light->Mobility == EComponentMobility::Movable;
				if (Light->IsA(UDirectionalLightComponent::StaticClass()))
				{
					++DirectionalLights;
				}
				else if (ComponentClass.Contains(TEXT("SpotLight")))
				{
					++SpotLights;
				}
				else
				{
					++PointLights;
				}
				if (bMovable)
				{
					++MovableLights;
					if (Light->CastShadows && Light->CastDynamicShadows)
					{
						++ShadowCastingMovableLights;
					}
				}
				if (const UPointLightComponent* Point = Cast<UPointLightComponent>(Light))
				{
					if (Point->AttenuationRadius > 3000.0f && LargeLights.Num() < 10)
					{
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("actor"), Actor->GetName());
						Entry->SetNumberField(TEXT("attenuation_radius"), Point->AttenuationRadius);
						Entry->SetStringField(TEXT("mobility"), Lvl_EnumName<EComponentMobility::Type>(Light->Mobility));
						LargeLights.Add(Lvl_AsValue(Entry));
					}
				}
			}
		}
	}

	// ---- Instancing candidates ----
	Lvl_FJsonArray InstancingCandidates;
	MeshUsage.ValueSort([](int32 A, int32 B) { return A > B; });
	int32 CandidateInstances = 0;
	for (const TPair<FString, int32>& Pair : MeshUsage)
	{
		if (Pair.Value < InstancingThreshold)
		{
			break;
		}
		CandidateInstances += Pair.Value;
		if (InstancingCandidates.Num() < 15)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("mesh"), Pair.Key);
			Entry->SetNumberField(TEXT("placed_count"), Pair.Value);
			InstancingCandidates.Add(Lvl_AsValue(Entry));
		}
	}

	// ---- Streaming levels / world settings ----
	Lvl_FJsonArray Streaming;
	for (const ULevelStreaming* Level : World->GetStreamingLevels())
	{
		if (!Level)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("level"), Level->GetWorldAssetPackageName());
		Entry->SetStringField(TEXT("class"), Level->GetClass()->GetName());
		Entry->SetBoolField(TEXT("loaded"), Level->IsLevelLoaded());
		Entry->SetBoolField(TEXT("visible"), Level->IsLevelVisible());
		Entry->SetBoolField(TEXT("should_be_loaded"), Level->ShouldBeLoaded());
		Entry->SetBoolField(TEXT("should_be_visible"), Level->ShouldBeVisible());
		Streaming.Add(Lvl_AsValue(Entry));
	}

	TSharedPtr<FJsonObject> WorldInfo = MakeShared<FJsonObject>();
	WorldInfo->SetStringField(TEXT("name"), World->GetMapName());
	if (const AWorldSettings* Settings = World->GetWorldSettings())
	{
		const FString HLOD = Lvl_ReadPropertyText(const_cast<AWorldSettings*>(Settings), TEXT("bEnableHierarchicalLODSystem"));
		if (!HLOD.IsEmpty())
		{
			WorldInfo->SetBoolField(TEXT("hlod_enabled"), HLOD.Equals(TEXT("True"), ESearchCase::IgnoreCase));
		}
		const FString Composition = Lvl_ReadPropertyText(const_cast<AWorldSettings*>(Settings), TEXT("bEnableWorldComposition"));
		if (!Composition.IsEmpty())
		{
			WorldInfo->SetBoolField(TEXT("world_composition"), Composition.Equals(TEXT("True"), ESearchCase::IgnoreCase));
		}
	}

	// ---- Issues ----
	if (TickingActors > 200)
	{
		Lvl_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d actors tick every frame. Disable Start with Tick Enabled on actors that do not need it (see ticking_classes)."), TickingActors));
	}
	if (CandidateInstances > 0)
	{
		Lvl_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d static mesh actors share meshes placed %d+ times: merge them into Instanced / Hierarchical Instanced Static Mesh components to cut draw calls and component overhead."), CandidateInstances, InstancingThreshold));
	}
	if (ShadowCastingMovableLights > 4)
	{
		Lvl_AddIssue(Issues, TEXT("error"), FString::Printf(TEXT("%d movable lights cast dynamic shadows: each costs several shadow passes. Make most of them Static / Stationary or turn shadows off."), ShadowCastingMovableLights));
	}
	else if (MovableLights > 8)
	{
		Lvl_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d movable lights. Prefer Static / Stationary."), MovableLights));
	}
	if (MovableMeshes > 50)
	{
		Lvl_AddIssue(Issues, TEXT("info"), FString::Printf(TEXT("%d movable static mesh components: static ones get cheaper culling, baked lighting and batching. Set Mobility to Static where nothing moves."), MovableMeshes));
	}
	if (SkinnedAlwaysTick > 0)
	{
		Lvl_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d skinned mesh components animate even when not visible (AlwaysTickPoseAndRefreshBones). Use OnlyTickMontagesWhenNotRendered / OnlyTickPoseWhenRendered."), SkinnedAlwaysTick));
	}
	if (ReflectionCaptures > 50)
	{
		Lvl_AddIssue(Issues, TEXT("info"), FString::Printf(TEXT("%d reflection captures."), ReflectionCaptures));
	}
	if (ActorCount > 5000 && Streaming.Num() == 0)
	{
		Lvl_AddIssue(Issues, TEXT("info"), FString::Printf(TEXT("%d actors in one persistent level with no streaming levels: consider splitting the map into streamed sub-levels."), ActorCount));
	}
	if (!bIsPIE && NavBoundsVolumes == 0 && NavMeshes.Num() > 0)
	{
		Lvl_AddIssue(Issues, TEXT("info"), TEXT("A RecastNavMesh exists without NavMeshBoundsVolume actors: nothing will be built."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), bIsPIE ? TEXT("pie") : TEXT("editor"));
	Result->SetObjectField(TEXT("world_info"), WorldInfo);
	Result->SetNumberField(TEXT("actor_count"), ActorCount);
	Result->SetNumberField(TEXT("component_count"), ComponentCount);
	Result->SetNumberField(TEXT("ticking_actors"), TickingActors);
	Result->SetNumberField(TEXT("ticking_components"), TickingComponents);
	Result->SetObjectField(TEXT("top_classes"), Lvl_SortedCounts(ClassCounts, 15));
	Result->SetObjectField(TEXT("ticking_classes"), Lvl_SortedCounts(TickingClassCounts, 15));

	TSharedPtr<FJsonObject> Meshes = MakeShared<FJsonObject>();
	Meshes->SetNumberField(TEXT("static_mesh_components"), StaticMeshComponents);
	Meshes->SetNumberField(TEXT("instanced_components"), InstancedMeshComponents);
	Meshes->SetNumberField(TEXT("movable"), MovableMeshes);
	Meshes->SetNumberField(TEXT("skinned_mesh_components"), SkinnedMeshes);
	Meshes->SetNumberField(TEXT("skinned_always_tick"), SkinnedAlwaysTick);
	Meshes->SetArrayField(TEXT("instancing_candidates"), InstancingCandidates);
	Result->SetObjectField(TEXT("meshes"), Meshes);

	TSharedPtr<FJsonObject> Lights = MakeShared<FJsonObject>();
	Lights->SetNumberField(TEXT("directional"), DirectionalLights);
	Lights->SetNumberField(TEXT("point"), PointLights);
	Lights->SetNumberField(TEXT("spot"), SpotLights);
	Lights->SetNumberField(TEXT("movable"), MovableLights);
	Lights->SetNumberField(TEXT("movable_shadow_casting"), ShadowCastingMovableLights);
	Lights->SetArrayField(TEXT("large_radius_lights"), LargeLights);
	Result->SetObjectField(TEXT("lights"), Lights);

	TSharedPtr<FJsonObject> Volumes = MakeShared<FJsonObject>();
	Volumes->SetNumberField(TEXT("reflection_captures"), ReflectionCaptures);
	Volumes->SetNumberField(TEXT("post_process_volumes"), PostProcessVolumes);
	Volumes->SetNumberField(TEXT("lightmass_importance_volumes"), LightmassVolumes);
	Volumes->SetNumberField(TEXT("particle_and_niagara_components"), ParticleSystems);
	Result->SetObjectField(TEXT("volumes"), Volumes);

	TSharedPtr<FJsonObject> Navigation = MakeShared<FJsonObject>();
	Navigation->SetArrayField(TEXT("nav_mesh_actors"), NavMeshes);
	Navigation->SetArrayField(TEXT("nav_bounds_volumes"), NavVolumes);
	Result->SetObjectField(TEXT("navigation"), Navigation);

	Result->SetArrayField(TEXT("streaming_levels"), Streaming);
	Result->SetNumberField(TEXT("error_count"), Lvl_CountSeverity(Issues, TEXT("error")));
	Result->SetNumberField(TEXT("warning_count"), Lvl_CountSeverity(Issues, TEXT("warning")));
	Result->SetArrayField(TEXT("issues"), Issues);
	return CreateSuccessResponse(Result);
}
