// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/OptimizationActions.h"
#include "Actions/PIEActions.h"
#include "MCPCompat.h"
#include "MCPInspectionUtils.h"
#include "Editor.h"
#include "EditorBuildUtils.h"
#include "EditorLevelUtils.h"
#include "EditorStaticMeshLibrary.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"

// LOD and collision generation go through UEditorStaticMeshLibrary (UE 4.27). UE5 moved it to a subsystem.
#if MCP_UE5
#define MCP_STATICMESH_LIBRARY 0
#else
#define MCP_STATICMESH_LIBRARY 1
#endif

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> Opt_FJsonArray;

	constexpr int32 Opt_MaxBulkAssets = 25;

	TSharedPtr<FJsonValue> Opt_AsValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	double Opt_MB(int64 Bytes)
	{
		return static_cast<double>(Bytes) / (1024.0 * 1024.0);
	}

	/** Names from "name" (string) and "names" (array) parameters. */
	TArray<FString> Opt_CollectNames(const TSharedPtr<FJsonObject>& Params, const TCHAR* SingleKey, const TCHAR* ListKey)
	{
		TArray<FString> Names;
		FString Single;
		if (Params->TryGetStringField(SingleKey, Single) && !Single.IsEmpty())
		{
			Names.Add(Single);
		}
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (Params->TryGetArrayField(ListKey, List))
		{
			for (const TSharedPtr<FJsonValue>& Value : *List)
			{
				const FString Name = Value->AsString();
				if (!Name.IsEmpty())
				{
					Names.AddUnique(Name);
				}
			}
		}
		return Names;
	}

	const FStaticMeshRenderData* Opt_GetRenderData(const UStaticMesh* Mesh)
	{
		return Mesh->GetRenderData();
	}

	bool Opt_HasNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key)
	{
		double Unused;
		return Params->TryGetNumberField(Key, Unused);
	}

	bool Opt_HasBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key)
	{
		bool Unused;
		return Params->TryGetBoolField(Key, Unused);
	}

	// ---- Static mesh ----

	FString Opt_ComplexityName(ECollisionTraceFlag Flag)
	{
		switch (Flag)
		{
		case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex: return TEXT("use_simple_as_complex");
		case CTF_UseComplexAsSimple: return TEXT("use_complex_as_simple");
		default: return TEXT("project_default");
		}
	}

	TSharedPtr<FJsonObject> Opt_DescribeMesh(UStaticMesh* Mesh)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetNumberField(TEXT("lod_count"), Mesh->GetNumLODs());
		Opt_FJsonArray Triangles;
		if (const FStaticMeshRenderData* RenderData = Opt_GetRenderData(Mesh))
		{
			for (const FStaticMeshLODResources& Lod : RenderData->LODResources)
			{
				Triangles.Add(MakeShared<FJsonValueNumber>(Lod.GetNumTriangles()));
			}
		}
		Info->SetArrayField(TEXT("triangles_per_lod"), Triangles);
		if (const UBodySetup* Body = Mesh->GetBodySetup())
		{
			Info->SetNumberField(TEXT("simple_collision_shapes"), Body->AggGeom.GetElementCount());
			Info->SetStringField(TEXT("collision_complexity"), Opt_ComplexityName(Body->CollisionTraceFlag.GetValue()));
		}
		Info->SetNumberField(TEXT("lightmap_resolution"), Mesh->GetLightMapResolution());
		Info->SetStringField(TEXT("lod_group"), Mesh->LODGroup.ToString());
		return Info;
	}

	bool Opt_ParseCollisionComplexity(const FString& Name, ECollisionTraceFlag& OutFlag)
	{
		const FString Lower = Name.ToLower();
		if (Lower == TEXT("project_default")) { OutFlag = CTF_UseDefault; return true; }
		if (Lower == TEXT("simple_and_complex")) { OutFlag = CTF_UseSimpleAndComplex; return true; }
		if (Lower == TEXT("use_simple_as_complex")) { OutFlag = CTF_UseSimpleAsComplex; return true; }
		if (Lower == TEXT("use_complex_as_simple")) { OutFlag = CTF_UseComplexAsSimple; return true; }
		return false;
	}

#if MCP_STATICMESH_LIBRARY
	enum class EOptCollisionKind : uint8 { None, Shape, Convex };

	bool Opt_ParseCollision(const FString& Name, EOptCollisionKind& OutKind, EScriptingCollisionShapeType& OutShape)
	{
		const FString Lower = Name.ToLower();
		OutKind = EOptCollisionKind::Shape;
		if (Lower == TEXT("none")) { OutKind = EOptCollisionKind::None; return true; }
		if (Lower == TEXT("convex")) { OutKind = EOptCollisionKind::Convex; return true; }
		if (Lower == TEXT("box")) { OutShape = EScriptingCollisionShapeType::Box; return true; }
		if (Lower == TEXT("sphere")) { OutShape = EScriptingCollisionShapeType::Sphere; return true; }
		if (Lower == TEXT("capsule")) { OutShape = EScriptingCollisionShapeType::Capsule; return true; }
		if (Lower == TEXT("kdop10x")) { OutShape = EScriptingCollisionShapeType::NDOP10_X; return true; }
		if (Lower == TEXT("kdop10y")) { OutShape = EScriptingCollisionShapeType::NDOP10_Y; return true; }
		if (Lower == TEXT("kdop10z")) { OutShape = EScriptingCollisionShapeType::NDOP10_Z; return true; }
		if (Lower == TEXT("kdop18")) { OutShape = EScriptingCollisionShapeType::NDOP18; return true; }
		if (Lower == TEXT("kdop26")) { OutShape = EScriptingCollisionShapeType::NDOP26; return true; }
		return false;
	}
#endif

	TArray<UObject*> Opt_ResolveAssets(const TArray<FString>& Names, UClass* Class, FString& OutError)
	{
		TArray<UObject*> Assets;
		TArray<UClass*> Classes;
		Classes.Add(Class);
		for (const FString& Name : Names)
		{
			UObject* Asset = FMCPInspectionUtils::FindAsset(Name, Classes);
			if (!Asset)
			{
				OutError = FString::Printf(TEXT("%s '%s' not found. Pass the asset name or a full path such as /Game/Meshes/SM_Rock."), *Class->GetName(), *Name);
				return TArray<UObject*>();
			}
			Assets.AddUnique(Asset);
		}
		return Assets;
	}

	// ---- Texture ----

	bool Opt_ParseEnumValue(const UEnum* Enum, const FString& Name, const FString& Prefix, int64& OutValue)
	{
		int64 Value = Enum->GetValueByNameString(Name);
		if (Value == INDEX_NONE && !Prefix.IsEmpty() && !Name.StartsWith(Prefix))
		{
			Value = Enum->GetValueByNameString(Prefix + Name);
		}
		if (Value == INDEX_NONE)
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

}

// ============================================================================
// optimize_static_mesh
// ============================================================================

bool FOptimizeStaticMeshAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	const TArray<FString> Names = Opt_CollectNames(Params, TEXT("mesh"), TEXT("meshes"));
	if (Names.Num() == 0)
	{
		OutError = TEXT("Provide 'mesh' (name or path) or 'meshes' (array).");
		return false;
	}
	if (Names.Num() > Opt_MaxBulkAssets)
	{
		OutError = FString::Printf(TEXT("At most %d meshes per call (each rebuild is slow)."), Opt_MaxBulkAssets);
		return false;
	}
	if (Opt_ResolveAssets(Names, UStaticMesh::StaticClass(), OutError).Num() == 0)
	{
		return false;
	}

	FString Complexity;
	ECollisionTraceFlag Flag;
	if (Params->TryGetStringField(TEXT("collision_complexity"), Complexity) && !Opt_ParseCollisionComplexity(Complexity, Flag))
	{
		OutError = FString::Printf(TEXT("Invalid collision_complexity '%s'. Use project_default, simple_and_complex, use_simple_as_complex or use_complex_as_simple."), *Complexity);
		return false;
	}

	const bool bWantsLodsOrCollision = Params->HasField(TEXT("lods")) || Params->HasField(TEXT("collision")) || Params->HasField(TEXT("remove_lods"))
		|| Params->HasField(TEXT("generate_lightmap_uvs")) || Params->HasField(TEXT("allow_cpu_access"));
#if MCP_STATICMESH_LIBRARY
	FString Collision;
	if (Params->TryGetStringField(TEXT("collision"), Collision))
	{
		EOptCollisionKind Kind;
		EScriptingCollisionShapeType Shape = EScriptingCollisionShapeType::Box;
		if (!Opt_ParseCollision(Collision, Kind, Shape))
		{
			OutError = FString::Printf(TEXT("Invalid collision '%s'. Use none, box, sphere, capsule, kdop10x, kdop10y, kdop10z, kdop18, kdop26 or convex."), *Collision);
			return false;
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Lods = nullptr;
	if (Params->TryGetArrayField(TEXT("lods"), Lods))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Lods)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			double Percent = 0.0;
			if (!Value->TryGetObject(Entry) || !(*Entry)->TryGetNumberField(TEXT("percent_triangles"), Percent) || Percent <= 0.0 || Percent > 1.0)
			{
				OutError = TEXT("Each lods entry must be an object with percent_triangles in (0, 1], e.g. {\"percent_triangles\": 0.5}.");
				return false;
			}
		}
	}
#else
	if (bWantsLodsOrCollision)
	{
		OutError = TEXT("LOD / collision generation is only wired for UE 4.27 (UEditorStaticMeshLibrary). Use the Static Mesh editor on UE5.");
		return false;
	}
#endif
	return true;
}

TSharedPtr<FJsonObject> FOptimizeStaticMeshAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString Error;
	const TArray<UObject*> Assets = Opt_ResolveAssets(Opt_CollectNames(Params, TEXT("mesh"), TEXT("meshes")), UStaticMesh::StaticClass(), Error);

	Opt_FJsonArray Results;
	for (UObject* Asset : Assets)
	{
		UStaticMesh* Mesh = Cast<UStaticMesh>(Asset);
		if (!Mesh)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("mesh"), Mesh->GetPathName());
		Entry->SetObjectField(TEXT("before"), Opt_DescribeMesh(Mesh));
		Opt_FJsonArray Applied;
		Opt_FJsonArray Warnings;

		Mesh->Modify();

		FString LodGroup;
		if (Params->TryGetStringField(TEXT("lod_group"), LodGroup))
		{
			Mesh->SetLODGroup(LodGroup.Equals(TEXT("None"), ESearchCase::IgnoreCase) ? NAME_None : FName(*LodGroup), true);
			Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("lod_group=%s"), *LodGroup)));
		}

#if MCP_STATICMESH_LIBRARY
		if (GetOptionalBool(Params, TEXT("remove_lods"), false))
		{
			UEditorStaticMeshLibrary::RemoveLods(Mesh);
			Applied.Add(MakeShared<FJsonValueString>(TEXT("removed LODs")));
		}

		const TArray<TSharedPtr<FJsonValue>>* Lods = nullptr;
		if (Params->TryGetArrayField(TEXT("lods"), Lods) && Lods->Num() > 0)
		{
			FEditorScriptingMeshReductionOptions Options;
			bool bAnyScreenSize = false;
			for (const TSharedPtr<FJsonValue>& Value : *Lods)
			{
				const TSharedPtr<FJsonObject>& LodObject = Value->AsObject();
				FEditorScriptingMeshReductionSettings Settings;
				Settings.PercentTriangles = static_cast<float>(LodObject->GetNumberField(TEXT("percent_triangles")));
				double ScreenSize = 0.0;
				if (LodObject->TryGetNumberField(TEXT("screen_size"), ScreenSize))
				{
					Settings.ScreenSize = static_cast<float>(ScreenSize);
					bAnyScreenSize = true;
				}
				Options.ReductionSettings.Add(Settings);
			}
			Options.bAutoComputeLODScreenSize = GetOptionalBool(Params, TEXT("auto_compute_screen_size"), !bAnyScreenSize);
			const int32 NewCount = UEditorStaticMeshLibrary::SetLods(Mesh, Options);
			if (NewCount < 0)
			{
				Warnings.Add(MakeShared<FJsonValueString>(TEXT("SetLods failed (mesh may have no valid source geometry).")));
			}
			else
			{
				Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("generated %d LODs"), NewCount)));
			}
		}

		FString Collision;
		if (Params->TryGetStringField(TEXT("collision"), Collision))
		{
			EOptCollisionKind Kind = EOptCollisionKind::None;
			EScriptingCollisionShapeType Shape = EScriptingCollisionShapeType::Box;
			Opt_ParseCollision(Collision, Kind, Shape);
			UEditorStaticMeshLibrary::RemoveCollisions(Mesh);
			if (Kind == EOptCollisionKind::Shape)
			{
				if (UEditorStaticMeshLibrary::AddSimpleCollisions(Mesh, Shape) < 0)
				{
					Warnings.Add(MakeShared<FJsonValueString>(TEXT("AddSimpleCollisions failed.")));
				}
			}
			else if (Kind == EOptCollisionKind::Convex)
			{
				const int32 HullCount = static_cast<int32>(GetOptionalNumber(Params, TEXT("hull_count"), 4));
				const int32 MaxHullVerts = static_cast<int32>(GetOptionalNumber(Params, TEXT("max_hull_verts"), 16));
				const int32 HullPrecision = static_cast<int32>(GetOptionalNumber(Params, TEXT("hull_precision"), 100000));
				if (!UEditorStaticMeshLibrary::SetConvexDecompositionCollisions(Mesh, HullCount, MaxHullVerts, HullPrecision))
				{
					Warnings.Add(MakeShared<FJsonValueString>(TEXT("Convex decomposition failed.")));
				}
			}
			Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("collision=%s"), *Collision)));
		}

		if (Opt_HasBool(Params, TEXT("generate_lightmap_uvs")))
		{
			UEditorStaticMeshLibrary::SetGenerateLightmapUVs(Mesh, GetOptionalBool(Params, TEXT("generate_lightmap_uvs")));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("generate_lightmap_uvs")));
		}
		if (Opt_HasBool(Params, TEXT("allow_cpu_access")))
		{
			UEditorStaticMeshLibrary::SetAllowCPUAccess(Mesh, GetOptionalBool(Params, TEXT("allow_cpu_access")));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("allow_cpu_access")));
		}
#endif

		FString Complexity;
		ECollisionTraceFlag Flag = CTF_UseDefault;
		if (Params->TryGetStringField(TEXT("collision_complexity"), Complexity) && Opt_ParseCollisionComplexity(Complexity, Flag))
		{
			if (!Mesh->GetBodySetup())
			{
				Mesh->CreateBodySetup();
			}
			if (UBodySetup* Body = Mesh->GetBodySetup())
			{
				Body->Modify();
				Body->CollisionTraceFlag = Flag;
				Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("collision_complexity=%s"), *Complexity)));
			}
		}
		if (Opt_HasNumber(Params, TEXT("lightmap_resolution")))
		{
			Mesh->SetLightMapResolution(FMath::Clamp(static_cast<int32>(GetOptionalNumber(Params, TEXT("lightmap_resolution"))), 4, 4096));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("lightmap_resolution")));
		}
		if (Opt_HasBool(Params, TEXT("distance_field")))
		{
			Mesh->bGenerateMeshDistanceField = GetOptionalBool(Params, TEXT("distance_field"));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("distance_field")));
		}
		if (Opt_HasNumber(Params, TEXT("min_lod")))
		{
			Mesh->SetMinLOD(FPerPlatformInt(FMath::Max(0, static_cast<int32>(GetOptionalNumber(Params, TEXT("min_lod"))))));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("min_lod")));
		}

		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();

		Entry->SetObjectField(TEXT("after"), Opt_DescribeMesh(Mesh));
		Entry->SetArrayField(TEXT("applied"), Applied);
		if (Warnings.Num() > 0)
		{
			Entry->SetArrayField(TEXT("warnings"), Warnings);
		}
		Results.Add(Opt_AsValue(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Results.Num());
	Result->SetArrayField(TEXT("meshes"), Results);
	return CreateSuccessResponse(Result);
}

// ============================================================================
// optimize_texture
// ============================================================================

bool FOptimizeTextureAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	const TArray<FString> Names = Opt_CollectNames(Params, TEXT("texture"), TEXT("textures"));
	if (Names.Num() == 0)
	{
		OutError = TEXT("Provide 'texture' (name or path) or 'textures' (array).");
		return false;
	}
	if (Names.Num() > Opt_MaxBulkAssets)
	{
		OutError = FString::Printf(TEXT("At most %d textures per call (each one is recompressed)."), Opt_MaxBulkAssets);
		return false;
	}
	if (Opt_ResolveAssets(Names, UTexture2D::StaticClass(), OutError).Num() == 0)
	{
		return false;
	}

	int64 Unused = 0;
	FString Value;
	if (Params->TryGetStringField(TEXT("lod_group"), Value) && !Opt_ParseEnumValue(StaticEnum<TextureGroup>(), Value, TEXT("TEXTUREGROUP_"), Unused))
	{
		OutError = FString::Printf(TEXT("Unknown lod_group '%s'. Examples: World, WorldNormalMap, WorldSpecular, Character, Weapon, Vehicle, Cinematic, Effects, Skybox, UI, Lightmap, Shadowmap."), *Value);
		return false;
	}
	if (Params->TryGetStringField(TEXT("compression"), Value) && !Opt_ParseEnumValue(StaticEnum<TextureCompressionSettings>(), Value, TEXT("TC_"), Unused))
	{
		OutError = FString::Printf(TEXT("Unknown compression '%s'. Examples: TC_Default, TC_Normalmap, TC_Masks, TC_Grayscale, TC_Alpha, TC_BC7, TC_HDR."), *Value);
		return false;
	}
	if (Params->TryGetStringField(TEXT("mip_gen"), Value) && !Opt_ParseEnumValue(StaticEnum<TextureMipGenSettings>(), Value, TEXT("TMGS_"), Unused))
	{
		OutError = FString::Printf(TEXT("Unknown mip_gen '%s'. Examples: TMGS_FromTextureGroup, TMGS_NoMipmaps, TMGS_Sharpen0, TMGS_SimpleAverage."), *Value);
		return false;
	}
	if (Opt_HasNumber(Params, TEXT("max_texture_size")))
	{
		const int32 Size = static_cast<int32>(GetOptionalNumber(Params, TEXT("max_texture_size")));
		if (Size < 0 || Size > 8192 || (Size > 0 && !FMath::IsPowerOfTwo(Size)))
		{
			OutError = TEXT("max_texture_size must be 0 (no cap) or a power of two up to 8192.");
			return false;
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FOptimizeTextureAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString Error;
	const TArray<UObject*> Assets = Opt_ResolveAssets(Opt_CollectNames(Params, TEXT("texture"), TEXT("textures")), UTexture2D::StaticClass(), Error);

	Opt_FJsonArray Results;
	double TotalBefore = 0.0;
	double TotalAfterEstimate = 0.0;
	for (UObject* Asset : Assets)
	{
		UTexture2D* Texture = Cast<UTexture2D>(Asset);
		if (!Texture)
		{
			continue;
		}
		const int64 BeforeBytes = static_cast<int64>(Texture->CalcTextureMemorySizeEnum(TMC_AllMips));
		const int32 MaxSide = FMath::Max(Texture->GetSizeX(), Texture->GetSizeY());

		Texture->Modify();
		Opt_FJsonArray Applied;
		int64 EnumValue = 0;
		FString Name;

		if (Params->TryGetStringField(TEXT("lod_group"), Name) && Opt_ParseEnumValue(StaticEnum<TextureGroup>(), Name, TEXT("TEXTUREGROUP_"), EnumValue))
		{
			Texture->LODGroup = static_cast<TextureGroup>(EnumValue);
			Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("lod_group=%s"), *Name)));
		}
		if (Params->TryGetStringField(TEXT("compression"), Name) && Opt_ParseEnumValue(StaticEnum<TextureCompressionSettings>(), Name, TEXT("TC_"), EnumValue))
		{
			Texture->CompressionSettings = static_cast<TextureCompressionSettings>(EnumValue);
			Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("compression=%s"), *Name)));
		}
		if (Params->TryGetStringField(TEXT("mip_gen"), Name) && Opt_ParseEnumValue(StaticEnum<TextureMipGenSettings>(), Name, TEXT("TMGS_"), EnumValue))
		{
			Texture->MipGenSettings = static_cast<TextureMipGenSettings>(EnumValue);
			Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("mip_gen=%s"), *Name)));
		}
		if (Opt_HasNumber(Params, TEXT("max_texture_size")))
		{
			Texture->MaxTextureSize = static_cast<int32>(GetOptionalNumber(Params, TEXT("max_texture_size")));
			Applied.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("max_texture_size=%d"), Texture->MaxTextureSize)));
		}
		if (Opt_HasBool(Params, TEXT("never_stream")))
		{
			Texture->NeverStream = GetOptionalBool(Params, TEXT("never_stream"));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("never_stream")));
		}
		if (Opt_HasBool(Params, TEXT("srgb")))
		{
			Texture->SRGB = GetOptionalBool(Params, TEXT("srgb"));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("srgb")));
		}
		if (Opt_HasNumber(Params, TEXT("lod_bias")))
		{
			Texture->LODBias = static_cast<int32>(GetOptionalNumber(Params, TEXT("lod_bias")));
			Applied.Add(MakeShared<FJsonValueString>(TEXT("lod_bias")));
		}

		Texture->PostEditChange();
		Texture->MarkPackageDirty();

		// Platform data is rebuilt asynchronously, so estimate the size from the new cap.
		double EstimatedAfter = Opt_MB(BeforeBytes);
		const int32 Cap = Texture->MaxTextureSize;
		if (Cap > 0 && MaxSide > Cap)
		{
			const double Scale = static_cast<double>(Cap) / static_cast<double>(MaxSide);
			EstimatedAfter *= Scale * Scale;
		}
		TotalBefore += Opt_MB(BeforeBytes);
		TotalAfterEstimate += EstimatedAfter;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("texture"), Texture->GetPathName());
		Entry->SetNumberField(TEXT("size_mb_before"), Opt_MB(BeforeBytes));
		Entry->SetNumberField(TEXT("size_mb_after_estimate"), EstimatedAfter);
		Entry->SetArrayField(TEXT("applied"), Applied);
		Results.Add(Opt_AsValue(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Results.Num());
	Result->SetNumberField(TEXT("total_mb_before"), TotalBefore);
	Result->SetNumberField(TEXT("total_mb_after_estimate"), TotalAfterEstimate);
	Result->SetArrayField(TEXT("textures"), Results);
	return CreateSuccessResponse(Result);
}
