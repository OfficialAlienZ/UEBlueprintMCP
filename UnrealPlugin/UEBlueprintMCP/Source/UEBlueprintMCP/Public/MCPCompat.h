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
#include "Editor.h"
#include "EdGraphSchema_K2.h"
#include "Materials/Material.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"

#if ENGINE_MAJOR_VERSION >= 5
#include "UObject/SavePackage.h"
#define MCP_UE5 1
#else
#define MCP_UE5 0
#endif

// APlayerController::InputKey takes FInputKeyParams from UE 5.1 on (the loose-argument overloads are deprecated).
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#define MCP_INPUT_KEY_PARAMS 1
#else
#define MCP_INPUT_KEY_PARAMS 0
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

	/** Queue a Play-In-Editor / Simulate session (starts on the next editor tick). Same API in UE 4.25+ and 5.x. */
	inline void RequestPlaySession(bool bSimulate)
	{
		FRequestPlaySessionParams Params;
		Params.WorldType = bSimulate ? EPlaySessionWorldType::SimulateInEditor : EPlaySessionWorldType::PlayInEditor;
		GEditor->RequestPlaySession(Params);
	}

	/**
	 * Feed a key event (press/release/analog value) to a player controller as if it came from a device.
	 * UE 5.1+ takes FInputKeyParams; UE 4.27 / 5.0 take the loose arguments.
	 */
	inline bool InjectKeyEvent(APlayerController* PlayerController, const FKey& Key, EInputEvent Event, float AmountDepressed, bool bGamepad)
	{
#if MCP_INPUT_KEY_PARAMS
		return PlayerController->InputKey(FInputKeyParams(Key, Event, static_cast<double>(AmountDepressed), bGamepad));
#else
		return PlayerController->InputKey(Key, Event, AmountDepressed, bGamepad);
#endif
	}

	/** Feed an axis delta (mouse movement, analog stick) to a player controller. */
	inline bool InjectKeyAxis(APlayerController* PlayerController, const FKey& Key, float Delta, float DeltaTime, bool bGamepad)
	{
#if MCP_INPUT_KEY_PARAMS
		return PlayerController->InputKey(FInputKeyParams(Key, static_cast<double>(Delta), DeltaTime, 1, bGamepad));
#else
		return PlayerController->InputAxis(Key, Delta, DeltaTime, 1, bGamepad);
#endif
	}

	/** Handle returned by AddCoreTicker: FTSTicker has its own handle type in UE5 (a plain FDelegateHandle does not convert in 5.8). */
#if MCP_UE5
	using FTickerHandle = FTSTicker::FDelegateHandle;
#else
	using FTickerHandle = FDelegateHandle;
#endif

	/** Add a game-thread core ticker. Delegate returns true to keep ticking, false to stop. FTSTicker (UE5) / FTicker (UE4). */
	inline FTickerHandle AddCoreTicker(TFunction<bool(float)> Callback, float DelaySeconds)
	{
#if MCP_UE5
		return FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(MoveTemp(Callback)), DelaySeconds);
#else
		return FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(MoveTemp(Callback)), DelaySeconds);
#endif
	}

	inline void RemoveCoreTicker(FTickerHandle Handle)
	{
#if MCP_UE5
		FTSTicker::GetCoreTicker().RemoveTicker(Handle);
#else
		FTicker::GetCoreTicker().RemoveTicker(Handle);
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
