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

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> Perf_FJsonArray;

	TSharedPtr<FJsonValue> Perf_AsValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	void Perf_AddIssue(Perf_FJsonArray& Issues, const TCHAR* Severity, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(Perf_AsValue(Issue));
	}

	double Perf_Ms(uint32 Cycles)
	{
		return FPlatformTime::ToMilliseconds(Cycles);
	}

	double Perf_MB(int64 Bytes)
	{
		return static_cast<double>(Bytes) / (1024.0 * 1024.0);
	}

	FString Perf_CVarString(const TCHAR* Name)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			return Variable->GetString();
		}
		return FString();
	}

	/** Reflected property value as export text ("" if the class has no such property). */
	FString Perf_ReadPropertyText(UObject* Object, const TCHAR* PropertyName)
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
	FString Perf_EnumName(int64 Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			return Enum->GetNameStringByValue(Value);
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	const FStaticMeshRenderData* Perf_GetRenderData(const UStaticMesh* Mesh)
	{
		return Mesh->GetRenderData();
	}

	// StaticEnum<ECollisionTraceFlag>() is not exported from Engine on 4.27, so map by hand.
	FString Perf_CollisionComplexityName(ECollisionTraceFlag Flag)
	{
		switch (Flag)
		{
		case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex: return TEXT("use_simple_as_complex");
		case CTF_UseComplexAsSimple: return TEXT("use_complex_as_simple");
		default: return TEXT("project_default");
		}
	}

	TSharedPtr<FJsonObject> Perf_SortedCounts(TMap<FString, int32>& Counts, int32 Limit)
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

	struct FAuditRow
	{
		TSharedPtr<FJsonObject> Json;
		double Cost = 0.0;
	};

	int32 Perf_CountSeverity(const Perf_FJsonArray& Issues, const TCHAR* Severity)
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

	// ---- Texture ----

	void Perf_AuditTexture(UTexture2D* Texture, FAuditRow& Row, Perf_FJsonArray& Issues, int64& InOutBytes)
	{
		const int32 Width = Texture->GetSizeX();
		const int32 Height = Texture->GetSizeY();
		const int64 Bytes = static_cast<int64>(Texture->CalcTextureMemorySizeEnum(TMC_AllMips));
		const EPixelFormat Format = Texture->GetPixelFormat();
		const FString FormatName = GPixelFormats[Format].Name;
		const FString Group = Perf_EnumName<TextureGroup>(Texture->LODGroup);
		const bool bPowerOfTwo = FMath::IsPowerOfTwo(Width) && FMath::IsPowerOfTwo(Height);
		const bool bUI = Group.Contains(TEXT("UI")) || Group.Contains(TEXT("Pixels2D"));

		Row.Json->SetNumberField(TEXT("width"), Width);
		Row.Json->SetNumberField(TEXT("height"), Height);
		Row.Json->SetStringField(TEXT("format"), FormatName);
		Row.Json->SetNumberField(TEXT("mips"), Texture->GetNumMips());
		Row.Json->SetNumberField(TEXT("size_mb"), Perf_MB(Bytes));
		Row.Json->SetStringField(TEXT("lod_group"), Group);
		Row.Json->SetStringField(TEXT("compression"), Perf_EnumName<TextureCompressionSettings>(Texture->CompressionSettings));
		Row.Json->SetStringField(TEXT("mip_gen"), Perf_EnumName<TextureMipGenSettings>(Texture->MipGenSettings));
		Row.Json->SetNumberField(TEXT("max_texture_size"), Texture->MaxTextureSize);
		Row.Json->SetBoolField(TEXT("never_stream"), Texture->NeverStream);
		Row.Json->SetBoolField(TEXT("srgb"), Texture->SRGB);
		Row.Cost = static_cast<double>(Bytes);
		InOutBytes += Bytes;

		const int32 MaxSide = FMath::Max(Width, Height);
		if (MaxSide >= 4096 && Texture->MaxTextureSize == 0 && !bUI)
		{
			Perf_AddIssue(Issues, TEXT("error"), FString::Printf(TEXT("%dx%d texture with no max_texture_size cap (%.1f MB). Cap it at 2048 with optimize_texture unless it is a hero asset."), Width, Height, Perf_MB(Bytes)));
		}
		else if (MaxSide >= 2048 && Bytes > 8 * 1024 * 1024 && !bUI)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%dx%d texture uses %.1f MB. Check whether it needs this resolution."), Width, Height, Perf_MB(Bytes)));
		}
		if (!bPowerOfTwo && Texture->MipGenSettings != TMGS_NoMipmaps && !bUI)
		{
			Perf_AddIssue(Issues, TEXT("warning"), TEXT("Non power-of-two size: cannot stream or generate full mips. Resize to a power of two."));
		}
		if ((Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8 || Format == PF_FloatRGBA || Format == PF_A32B32G32R32F) && MaxSide >= 512 && !bUI)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("Uncompressed format %s at %dx%d. Use a compression setting (DXT / BC) unless it is a data texture that needs precision."), *FormatName, Width, Height));
		}
		if (Texture->NeverStream && MaxSide >= 1024 && !bUI)
		{
			Perf_AddIssue(Issues, TEXT("warning"), TEXT("NeverStream is on for a large texture: it is always fully resident."));
		}
		if (Texture->MipGenSettings == TMGS_NoMipmaps && !bUI && MaxSide >= 256)
		{
			Perf_AddIssue(Issues, TEXT("info"), TEXT("No mipmaps: causes shimmering and wasted bandwidth for 3D surfaces. Fine for UI."));
		}
	}

	// ---- Static mesh ----

	void Perf_AuditStaticMesh(UStaticMesh* Mesh, FAuditRow& Row, Perf_FJsonArray& Issues, int64& InOutBytes)
	{
		const FStaticMeshRenderData* RenderData = Perf_GetRenderData(Mesh);
		const int32 LodCount = Mesh->GetNumLODs();
		int32 Triangles = 0;
		int32 Vertices = 0;
		int32 Sections = 0;
		if (RenderData && RenderData->LODResources.Num() > 0)
		{
			Triangles = RenderData->LODResources[0].GetNumTriangles();
			Vertices = RenderData->LODResources[0].GetNumVertices();
			Sections = RenderData->LODResources[0].Sections.Num();
		}
		const UBodySetup* BodySetup = Mesh->GetBodySetup();
		const int32 SimpleShapes = BodySetup ? BodySetup->AggGeom.GetElementCount() : 0;
		const ECollisionTraceFlag Complexity = BodySetup ? BodySetup->CollisionTraceFlag.GetValue() : CTF_UseDefault;
		const int32 LightmapRes = Mesh->GetLightMapResolution();
		const int64 Bytes = static_cast<int64>(Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal));

		Row.Json->SetNumberField(TEXT("lod_count"), LodCount);
		Row.Json->SetNumberField(TEXT("triangles_lod0"), Triangles);
		Row.Json->SetNumberField(TEXT("vertices_lod0"), Vertices);
		Row.Json->SetNumberField(TEXT("sections"), Sections);
		Row.Json->SetNumberField(TEXT("simple_collision_shapes"), SimpleShapes);
		Row.Json->SetStringField(TEXT("collision_complexity"), Perf_CollisionComplexityName(Complexity));
		Row.Json->SetNumberField(TEXT("lightmap_resolution"), LightmapRes);
		Row.Json->SetStringField(TEXT("lod_group"), Mesh->LODGroup.ToString());
		Row.Json->SetNumberField(TEXT("size_mb"), Perf_MB(Bytes));
		TArray<TSharedPtr<FJsonValue>> LodTriangles;
		if (RenderData)
		{
			for (const FStaticMeshLODResources& Lod : RenderData->LODResources)
			{
				LodTriangles.Add(MakeShared<FJsonValueNumber>(Lod.GetNumTriangles()));
			}
		}
		Row.Json->SetArrayField(TEXT("triangles_per_lod"), LodTriangles);
		Row.Cost = static_cast<double>(Triangles) * (LodCount <= 1 ? 1.5 : 1.0);
		InOutBytes += Bytes;

		if (Triangles >= 100000)
		{
			Perf_AddIssue(Issues, TEXT("error"), FString::Printf(TEXT("%d triangles at LOD0. Very high poly for a game mesh."), Triangles));
		}
		if (Triangles >= 5000 && LodCount <= 1)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d triangles and no LODs. Add LODs with optimize_static_mesh."), Triangles));
		}
		if (Triangles >= 2000 && Complexity == CTF_UseComplexAsSimple)
		{
			Perf_AddIssue(Issues, TEXT("warning"), TEXT("Uses per-triangle collision as simple collision: expensive physics. Add simple collision with optimize_static_mesh."));
		}
		if (SimpleShapes == 0 && Complexity != CTF_UseComplexAsSimple && Triangles >= 500)
		{
			Perf_AddIssue(Issues, TEXT("info"), TEXT("No simple collision shapes: traces and physics use the complex mesh. Add simple collision if it needs to collide."));
		}
		if (LightmapRes > 512)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("Lightmap resolution %d is high; every instance bakes a lightmap of this size."), LightmapRes));
		}
		if (Sections > 4)
		{
			Perf_AddIssue(Issues, TEXT("info"), FString::Printf(TEXT("%d material sections: each section is a separate draw call."), Sections));
		}
	}

	// ---- Skeletal mesh ----

	void Perf_AuditSkeletalMesh(USkeletalMesh* Mesh, FAuditRow& Row, Perf_FJsonArray& Issues, int64& InOutBytes)
	{
		const int32 LodCount = Mesh->GetLODNum();
		const int32 Bones = Mesh->GetRefSkeleton().GetNum();
		int32 Triangles = 0;
		int32 Vertices = 0;
		if (const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering())
		{
			if (RenderData->LODRenderData.Num() > 0)
			{
				Triangles = RenderData->LODRenderData[0].GetTotalFaces();
				Vertices = RenderData->LODRenderData[0].GetNumVertices();
			}
		}
		const int64 Bytes = static_cast<int64>(Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal));

		Row.Json->SetNumberField(TEXT("lod_count"), LodCount);
		Row.Json->SetNumberField(TEXT("bones"), Bones);
		Row.Json->SetNumberField(TEXT("triangles_lod0"), Triangles);
		Row.Json->SetNumberField(TEXT("vertices_lod0"), Vertices);
		Row.Json->SetNumberField(TEXT("materials"), Mesh->GetMaterials().Num());
		Row.Json->SetNumberField(TEXT("size_mb"), Perf_MB(Bytes));
		Row.Cost = static_cast<double>(Triangles) + Bones * 100.0;
		InOutBytes += Bytes;

		if (Triangles >= 5000 && LodCount <= 1)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d triangles and no LODs. Skinned meshes are costly on the GPU and the CPU; add LODs in the Skeletal Mesh editor."), Triangles));
		}
		if (Triangles >= 60000)
		{
			Perf_AddIssue(Issues, TEXT("error"), FString::Printf(TEXT("%d triangles at LOD0 is very heavy for a character."), Triangles));
		}
		if (Bones > 150)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d bones. Animation and skinning cost scale with bone count."), Bones));
		}
		if (Mesh->GetMaterials().Num() > 4)
		{
			Perf_AddIssue(Issues, TEXT("info"), TEXT("More than 4 material slots: each is a draw call per character."));
		}
	}

	// ---- Material ----

	void Perf_AuditMaterial(UMaterialInterface* Material, FAuditRow& Row, Perf_FJsonArray& Issues)
	{
		const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Material);
		const EBlendMode Blend = Material->GetBlendMode();
		const bool bLit = Material->GetShadingModels().IsLit();

		Row.Json->SetBoolField(TEXT("is_instance"), Material->IsA<UMaterialInstance>());
		Row.Json->SetNumberField(TEXT("pixel_instructions"), Stats.NumPixelShaderInstructions);
		Row.Json->SetNumberField(TEXT("vertex_instructions"), Stats.NumVertexShaderInstructions);
		Row.Json->SetNumberField(TEXT("samplers"), Stats.NumSamplers);
		Row.Json->SetNumberField(TEXT("pixel_texture_samples"), Stats.NumPixelTextureSamples);
		Row.Json->SetNumberField(TEXT("vertex_texture_samples"), Stats.NumVertexTextureSamples);
		Row.Json->SetStringField(TEXT("blend_mode"), Perf_EnumName<EBlendMode>(Blend));
		Row.Json->SetBoolField(TEXT("lit"), bLit);
		Row.Json->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());
		Row.Json->SetBoolField(TEXT("stats_available"), Stats.NumPixelShaderInstructions > 0 || Stats.NumVertexShaderInstructions > 0);
		Row.Cost = Stats.NumPixelShaderInstructions + 10.0 * Stats.NumPixelTextureSamples;

		if (Stats.NumPixelShaderInstructions >= 500)
		{
			Perf_AddIssue(Issues, TEXT("error"), FString::Printf(TEXT("%d pixel shader instructions: very expensive. Simplify the graph or bake parts into textures."), Stats.NumPixelShaderInstructions));
		}
		else if (Stats.NumPixelShaderInstructions >= 250)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d pixel shader instructions."), Stats.NumPixelShaderInstructions));
		}
		if (Stats.NumSamplers > 12)
		{
			Perf_AddIssue(Issues, TEXT("warning"), FString::Printf(TEXT("%d texture samplers (hardware limit is 16). Pack channels into fewer textures."), Stats.NumSamplers));
		}
		if (Blend == BLEND_Translucent && bLit)
		{
			Perf_AddIssue(Issues, TEXT("warning"), TEXT("Lit translucent material: expensive overdraw. Prefer masked or unlit translucent."));
		}
		if (Blend == BLEND_Translucent && Stats.NumPixelShaderInstructions >= 150)
		{
			Perf_AddIssue(Issues, TEXT("warning"), TEXT("Translucent material with a long pixel shader: cost multiplies with overdraw."));
		}
	}

	// ---- Blueprint ----

	void Perf_AuditBlueprint(UBlueprint* Blueprint, FAuditRow& Row, Perf_FJsonArray& Issues)
	{
		int32 NodeCount = 0;
		int32 ActorQueryCalls = 0;
		bool bHasTickEvent = false;

		TArray<UEdGraph*> Graphs;
		Graphs.Append(Blueprint->UbergraphPages);
		Graphs.Append(Blueprint->FunctionGraphs);
		Graphs.Append(Blueprint->MacroGraphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			NodeCount += Graph->Nodes.Num();
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
				{
					const FName EventName = Event->EventReference.GetMemberName();
					if (EventName == TEXT("ReceiveTick") || EventName == TEXT("Tick"))
					{
						bHasTickEvent = true;
					}
				}
				else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
				{
					const FString FunctionName = Call->FunctionReference.GetMemberName().ToString();
					if (FunctionName.StartsWith(TEXT("GetAllActorsOf")) || FunctionName.StartsWith(TEXT("GetAllActorsWith")))
					{
						++ActorQueryCalls;
					}
				}
			}
		}

		int32 TickingComponents = 0;
		int32 ComponentCount = 0;
		if (Blueprint->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				++ComponentCount;
				if (Node && Node->ComponentTemplate && Node->ComponentTemplate->PrimaryComponentTick.bCanEverTick
					&& Node->ComponentTemplate->PrimaryComponentTick.bStartWithTickEnabled)
				{
					++TickingComponents;
				}
			}
		}

		bool bActorCanTick = false;
		float TickInterval = 0.0f;
		if (const AActor* Default = Blueprint->GeneratedClass ? Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject(false)) : nullptr)
		{
			bActorCanTick = Default->PrimaryActorTick.bCanEverTick && Default->PrimaryActorTick.bStartWithTickEnabled;
			TickInterval = Default->PrimaryActorTick.TickInterval;
		}

		Row.Json->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : FString());
		Row.Json->SetNumberField(TEXT("node_count"), NodeCount);
		Row.Json->SetBoolField(TEXT("has_event_tick"), bHasTickEvent);
		Row.Json->SetBoolField(TEXT("actor_ticks"), bActorCanTick);
		Row.Json->SetNumberField(TEXT("tick_interval"), TickInterval);
		Row.Json->SetNumberField(TEXT("components"), ComponentCount);
		Row.Json->SetNumberField(TEXT("ticking_components"), TickingComponents);
		Row.Json->SetNumberField(TEXT("get_all_actors_calls"), ActorQueryCalls);
		Row.Cost = NodeCount + (bHasTickEvent ? 1000.0 : 0.0) + (bActorCanTick ? 200.0 : 0.0) + TickingComponents * 100.0 + ActorQueryCalls * 300.0;

		if (bHasTickEvent && TickInterval <= 0.0f)
		{
			Perf_AddIssue(Issues, TEXT("warning"), TEXT("Event Tick runs every frame. Move logic to events or timers, or raise the Tick Interval."));
		}
		if (bHasTickEvent && ActorQueryCalls > 0)
		{
			Perf_AddIssue(Issues, TEXT("error"), FString::Printf(TEXT("Event Tick exists and the Blueprint calls GetAllActors* %d time(s). If any call runs from Tick, cache the result instead."), ActorQueryCalls));
		}
		else if (ActorQueryCalls > 0)
		{
			Perf_AddIssue(Issues, TEXT("info"), FString::Printf(TEXT("%d GetAllActors* call(s): each one iterates every actor. Avoid calling them repeatedly."), ActorQueryCalls));
		}
		if (bActorCanTick && !bHasTickEvent && Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AActor::StaticClass()) && TickInterval <= 0.0f)
		{
			Perf_AddIssue(Issues, TEXT("info"), TEXT("Actor ticking is enabled without an Event Tick in this Blueprint (inherited or unused). Disable Start with Tick Enabled if nothing needs it."));
		}
		if (TickingComponents > 2)
		{
			Perf_AddIssue(Issues, TEXT("info"), FString::Printf(TEXT("%d components tick every frame."), TickingComponents));
		}
		if (NodeCount > 1500)
		{
			Perf_AddIssue(Issues, TEXT("info"), FString::Printf(TEXT("%d nodes: large Blueprint, hard to profile and slow to compile. Consider splitting."), NodeCount));
		}
	}
}

// ============================================================================
// get_performance_stats
// ============================================================================

TSharedPtr<FJsonObject> FGetPerformanceStatsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	const double GameMs = Perf_Ms(GGameThreadTime);
	const double RenderMs = Perf_Ms(GRenderThreadTime);
	const double RHIMs = Perf_Ms(GRHIThreadTime);
	const double GpuMs = Perf_Ms(GGPUFrameTime);
	const double FrameMs = GAverageMS;

	Result->SetNumberField(TEXT("fps"), GAverageFPS);
	Result->SetNumberField(TEXT("frame_ms"), FrameMs);
	Result->SetNumberField(TEXT("game_thread_ms"), GameMs);
	Result->SetNumberField(TEXT("render_thread_ms"), RenderMs);
	Result->SetNumberField(TEXT("rhi_thread_ms"), RHIMs);
	Result->SetNumberField(TEXT("gpu_ms"), GpuMs);

	FString Bottleneck = TEXT("unknown");
	const double Worst = FMath::Max(FMath::Max(GameMs, RenderMs), FMath::Max(GpuMs, RHIMs));
	if (Worst > 0.0 && FrameMs > 0.0)
	{
		if (Worst < FrameMs * 0.6)
		{
			Bottleneck = TEXT("frame_cap");
		}
		else if (Worst == GpuMs)
		{
			Bottleneck = TEXT("gpu");
		}
		else if (Worst == GameMs)
		{
			Bottleneck = TEXT("game_thread");
		}
		else if (Worst == RenderMs)
		{
			Bottleneck = TEXT("render_thread");
		}
		else
		{
			Bottleneck = TEXT("rhi_thread");
		}
	}
	Result->SetStringField(TEXT("bottleneck"), Bottleneck);
	Result->SetStringField(TEXT("bottleneck_note"),
		TEXT("Thread times are 0 until the engine has measured them (run 'stat unit' once, or use capture_performance). "
			"'frame_cap' means no thread fills the frame: vsync or t.MaxFPS is limiting, not the hardware. "
			"These are editor numbers: PIE is slower than a packaged build."));

	const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
	TSharedPtr<FJsonObject> Memory = MakeShared<FJsonObject>();
	Memory->SetNumberField(TEXT("used_physical_mb"), Perf_MB(MemoryStats.UsedPhysical));
	Memory->SetNumberField(TEXT("peak_used_physical_mb"), Perf_MB(MemoryStats.PeakUsedPhysical));
	Memory->SetNumberField(TEXT("available_physical_mb"), Perf_MB(MemoryStats.AvailablePhysical));
	Memory->SetNumberField(TEXT("total_physical_mb"), Perf_MB(FPlatformMemory::GetConstants().TotalPhysical));
	if (GDynamicRHI)
	{
		FTextureMemoryStats TextureStats;
		RHIGetTextureMemoryStats(TextureStats);
		Memory->SetNumberField(TEXT("texture_allocated_mb"), Perf_MB(TextureStats.AllocatedMemorySize));
		Memory->SetNumberField(TEXT("texture_pool_mb"), Perf_MB(TextureStats.TexturePoolSize));
		if (TextureStats.TotalGraphicsMemory >= 0)
		{
			Memory->SetNumberField(TEXT("total_graphics_memory_mb"), Perf_MB(TextureStats.TotalGraphicsMemory));
		}
	}
	Result->SetObjectField(TEXT("memory"), Memory);

	if (GDynamicRHI)
	{
		Result->SetStringField(TEXT("rhi"), GDynamicRHI->GetName());
		FString FeatureLevel;
		GetFeatureLevelName(GMaxRHIFeatureLevel, FeatureLevel);
		Result->SetStringField(TEXT("feature_level"), FeatureLevel);
	}

	TSharedPtr<FJsonObject> CVars = MakeShared<FJsonObject>();
	static const TCHAR* CVarNames[] =
	{
		TEXT("t.MaxFPS"), TEXT("r.VSync"), TEXT("r.ScreenPercentage"), TEXT("r.Streaming.PoolSize"),
		TEXT("r.ViewDistanceScale"), TEXT("sg.ViewDistanceQuality"), TEXT("sg.AntiAliasingQuality"),
		TEXT("sg.ShadowQuality"), TEXT("sg.PostProcessQuality"), TEXT("sg.TextureQuality"),
		TEXT("sg.EffectsQuality"), TEXT("sg.FoliageQuality"), TEXT("sg.ShadingQuality"),
	};
	for (const TCHAR* Name : CVarNames)
	{
		const FString Value = Perf_CVarString(Name);
		if (!Value.IsEmpty())
		{
			CVars->SetStringField(Name, Value);
		}
	}
	Result->SetObjectField(TEXT("cvars"), CVars);

	Result->SetBoolField(TEXT("pie_running"), FMCPPIE::IsRunning());
	Result->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());
	Result->SetStringField(TEXT("build_configuration"), LexToString(FApp::GetBuildConfiguration()));

	Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Result->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Result->SetStringField(TEXT("project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Result->SetStringField(TEXT("saved_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
	Result->SetStringField(TEXT("engine_dir"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
	Result->SetStringField(TEXT("engine_dir"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
	return CreateSuccessResponse(Result);
}

// ============================================================================
// audit_assets
// ============================================================================

namespace
{
	bool Perf_ResolveAuditClasses(const FString& AssetType, TArray<UClass*>& OutClasses)
	{
		const FString Type = AssetType.ToLower();
		if (Type == TEXT("texture")) { OutClasses.Add(UTexture2D::StaticClass()); return true; }
		if (Type == TEXT("static_mesh")) { OutClasses.Add(UStaticMesh::StaticClass()); return true; }
		if (Type == TEXT("skeletal_mesh")) { OutClasses.Add(USkeletalMesh::StaticClass()); return true; }
		if (Type == TEXT("material"))
		{
			OutClasses.Add(UMaterial::StaticClass());
			OutClasses.Add(UMaterialInstanceConstant::StaticClass());
			return true;
		}
		if (Type == TEXT("blueprint")) { OutClasses.Add(UBlueprint::StaticClass()); return true; }
		return false;
	}
}

bool FAuditAssetsAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString AssetType;
	if (!GetRequiredString(Params, TEXT("asset_type"), AssetType, OutError))
	{
		return false;
	}
	TArray<UClass*> Classes;
	if (!Perf_ResolveAuditClasses(AssetType, Classes))
	{
		OutError = FString::Printf(TEXT("Unknown asset_type '%s'. Use texture, static_mesh, skeletal_mesh, material or blueprint."), *AssetType);
		return false;
	}
	const FString Path = GetOptionalString(Params, TEXT("path"), TEXT("/Game"));
	if (!Path.StartsWith(TEXT("/")))
	{
		OutError = TEXT("path must be a content path such as /Game or /Game/Environment");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FAuditAssetsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString AssetType = GetOptionalString(Params, TEXT("asset_type")).ToLower();
	const FString Path = GetOptionalString(Params, TEXT("path"), TEXT("/Game"));
	const FString NameContains = GetOptionalString(Params, TEXT("name_contains"));
	const int32 Limit = FMath::Clamp(static_cast<int32>(GetOptionalNumber(Params, TEXT("limit"), 30)), 1, 500);
	const int32 MaxScan = FMath::Clamp(static_cast<int32>(GetOptionalNumber(Params, TEXT("max_scan"), 400)), 1, 3000);

	TArray<UClass*> Classes;
	Perf_ResolveAuditClasses(AssetType, Classes);

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Path));
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	for (UClass* Class : Classes)
	{
		MCPCompat::AddFilterClass(Filter, Class);
	}
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);

	TArray<FAuditRow> Rows;
	int32 Scanned = 0;
	int32 TotalErrors = 0;
	int32 TotalWarnings = 0;
	int64 TotalBytes = 0;
	int32 Matching = 0;

	for (const FAssetData& AssetData : Assets)
	{
		if (!NameContains.IsEmpty() && !AssetData.AssetName.ToString().Contains(NameContains))
		{
			continue;
		}
		++Matching;
		if (Scanned >= MaxScan)
		{
			continue;
		}
		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			continue;
		}
		++Scanned;

		FAuditRow Row;
		Row.Json = MakeShared<FJsonObject>();
		Row.Json->SetStringField(TEXT("name"), Asset->GetName());
		Row.Json->SetStringField(TEXT("path"), Asset->GetPathName());
		Perf_FJsonArray Issues;

		if (UTexture2D* Texture = Cast<UTexture2D>(Asset))
		{
			Perf_AuditTexture(Texture, Row, Issues, TotalBytes);
		}
		else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
		{
			Perf_AuditStaticMesh(StaticMesh, Row, Issues, TotalBytes);
		}
		else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
		{
			Perf_AuditSkeletalMesh(SkeletalMesh, Row, Issues, TotalBytes);
		}
		else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset))
		{
			Perf_AuditMaterial(Material, Row, Issues);
		}
		else if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			Perf_AuditBlueprint(Blueprint, Row, Issues);
		}
		else
		{
			continue;
		}

		TotalErrors += Perf_CountSeverity(Issues, TEXT("error"));
		TotalWarnings += Perf_CountSeverity(Issues, TEXT("warning"));
		Row.Json->SetArrayField(TEXT("issues"), Issues);
		Row.Json->SetNumberField(TEXT("cost"), Row.Cost);
		Rows.Add(MoveTemp(Row));
	}

	Rows.Sort([](const FAuditRow& A, const FAuditRow& B) { return A.Cost > B.Cost; });

	Perf_FJsonArray Entries;
	for (int32 Index = 0; Index < Rows.Num() && Index < Limit; ++Index)
	{
		Entries.Add(Perf_AsValue(Rows[Index].Json));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_type"), AssetType);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("matching_assets"), Matching);
	Result->SetNumberField(TEXT("scanned"), Scanned);
	Result->SetBoolField(TEXT("truncated"), Matching > Scanned);
	Result->SetNumberField(TEXT("error_count"), TotalErrors);
	Result->SetNumberField(TEXT("warning_count"), TotalWarnings);
	if (TotalBytes > 0)
	{
		Result->SetNumberField(TEXT("total_size_mb"), Perf_MB(TotalBytes));
	}
	Result->SetStringField(TEXT("sorted_by"), TEXT("cost (descending): bytes for textures, triangles for meshes, pixel instructions for materials, tick/graph cost for blueprints"));
	Result->SetArrayField(TEXT("assets"), Entries);
	if (Matching > Scanned)
	{
		Result->SetStringField(TEXT("note"), TEXT("max_scan reached: narrow 'path' / 'name_contains' or raise max_scan to inspect the rest."));
	}
	return CreateSuccessResponse(Result);
}
