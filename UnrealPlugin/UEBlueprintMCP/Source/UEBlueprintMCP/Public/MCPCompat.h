// Copyright (c) 2025 zolnoor. All rights reserved.
//
// Engine-version compatibility helpers (UE 4.27 <-> UE 5.x).
// Keep every version-specific API difference in this header so the action
// code stays free of #if blocks.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "Logging/TokenizedMessage.h"
#include "FileHelpers.h"
#include "EdGraphSchema_K2.h"
#include "Materials/Material.h"
#include "AssetRegistry/AssetRegistryModule.h"

#if ENGINE_MAJOR_VERSION >= 5
#include "UObject/SavePackage.h"
#define MCP_UE5 1
#else
#define MCP_UE5 0
#endif

// Defined by UEBlueprintMCP.Build.cs. 1 when the EnhancedInput module is linked.
#ifndef MCP_WITH_ENHANCED_INPUT
#define MCP_WITH_ENHANCED_INPUT 0
#endif

namespace MCPCompat
{
	/** MarkAsGarbage (UE5) / MarkPendingKill (UE4). */
	inline void MarkGarbage(UObject* Object)
	{
		if (!Object) return;
#if MCP_UE5
		Object->MarkAsGarbage();
#else
		Object->MarkPendingKill();
#endif
	}

	/** Save a package to disk. Returns true on success. */
	inline bool SavePackageToDisk(UPackage* Package, UObject* Asset, const FString& Filename, EObjectFlags TopLevelFlags)
	{
#if MCP_UE5
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = TopLevelFlags;
		return UPackage::SavePackage(Package, Asset, *Filename, SaveArgs);
#else
		return UPackage::SavePackage(Package, Asset, TopLevelFlags, *Filename);
#endif
	}

	/** All dirty map + content packages. */
	inline void GetDirtyPackages(TArray<UPackage*>& OutPackages)
	{
#if MCP_UE5
		FEditorFileUtils::GetDirtyPackages(OutPackages);
#else
		FEditorFileUtils::GetDirtyWorldPackages(OutPackages);
		TArray<UPackage*> ContentPackages;
		FEditorFileUtils::GetDirtyContentPackages(ContentPackages);
		OutPackages.Append(ContentPackages);
#endif
	}

	/** Append an expression to a material's expression list. */
	inline void AddMaterialExpression(UMaterial* Material, UMaterialExpression* Expression)
	{
#if MCP_UE5
		Material->GetExpressionCollection().AddExpression(Expression);
#else
		Material->Expressions.Add(Expression);
		Expression->Material = Material;
		Expression->UpdateMaterialExpressionGuid(true, true);
#endif
	}

	/**
	 * Object that owns the material output pins (BaseColor, Roughness, ...).
	 * UE5: UMaterialEditorOnlyData. UE4: the UMaterial itself.
	 */
#if MCP_UE5
	inline UMaterialEditorOnlyData* GetMaterialInputs(UMaterial* Material) { return Material->GetEditorOnlyData(); }
#else
	inline UMaterial* GetMaterialInputs(UMaterial* Material) { return Material; }
#endif

	/** Pin type for a floating point value (PC_Real/PC_Double in UE5, PC_Float in UE4). */
	inline void SetRealPinType(FEdGraphPinType& PinType)
	{
#if MCP_UE5
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
#else
		PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
		PinType.PinSubCategory = NAME_None;
#endif
	}

	/** Asset registry: all assets of a given class. */
	inline void GetAssetsByClass(IAssetRegistry& Registry, UClass* Class, TArray<FAssetData>& OutAssets)
	{
#if MCP_UE5
		Registry.GetAssetsByClass(Class->GetClassPathName(), OutAssets);
#else
		Registry.GetAssetsByClass(Class->GetFName(), OutAssets);
#endif
	}

	/** Find a loaded native/user enum by short name (e.g. ECollisionChannel). */
	inline UEnum* FindEnum(const FString& Name)
	{
#if MCP_UE5
		return FindFirstObject<UEnum>(*Name, EFindFirstObjectOptions::NativeFirst);
#else
		return FindObject<UEnum>(ANY_PACKAGE, *Name, true);
#endif
	}

	/** Add a class to an asset registry filter. */
	inline void AddFilterClass(FARFilter& Filter, UClass* Class)
	{
#if MCP_UE5
		Filter.ClassPaths.Add(Class->GetClassPathName());
#else
		Filter.ClassNames.Add(Class->GetFName());
#endif
	}
}
