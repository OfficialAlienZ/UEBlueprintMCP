// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/OptimizationActions.h"
#include "Actions/PIEActions.h"
#include "Editor.h"
#include "EditorBuildUtils.h"
#include "EditorLevelUtils.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

// set_project_config, manage_level_streaming and build_level (split from OptimizationActions.cpp to keep files small).

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> Cfg_FJsonArray;

	TSharedPtr<FJsonValue> Cfg_AsValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	bool Cfg_HasBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key)
	{
		bool Unused;
		return Params->TryGetBoolField(Key, Unused);
	}

	// ---- Ini editing ----

	bool Cfg_ResolveConfigFile(const FString& File, FString& OutFileName)
	{
		static const TCHAR* Names[] = { TEXT("Engine"), TEXT("Game"), TEXT("Input"), TEXT("Editor"), TEXT("Scalability"), TEXT("GameUserSettings"), TEXT("EditorPerProjectUserSettings") };
		for (const TCHAR* Name : Names)
		{
			if (File.Equals(Name, ESearchCase::IgnoreCase))
			{
				OutFileName = FString::Printf(TEXT("Default%s.ini"), Name);
				return true;
			}
		}
		return false;
	}

	FString Cfg_StripKeyOperator(const FString& Key)
	{
		int32 Start = 0;
		while (Start < Key.Len() && (Key[Start] == '+' || Key[Start] == '-' || Key[Start] == '.' || Key[Start] == '!'))
		{
			++Start;
		}
		return Key.Mid(Start);
	}

	bool Cfg_IsSectionHeader(const FString& Line, FString& OutName)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]")))
		{
			OutName = Trimmed.Mid(1, Trimmed.Len() - 2);
			return true;
		}
		return false;
	}

	/** Splits a "Key=Value" line. Returns false for blank / comment / header lines. */
	bool Cfg_SplitLine(const FString& Line, FString& OutKey, FString& OutValue)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT(";")) || Trimmed.StartsWith(TEXT("[")))
		{
			return false;
		}
		return Trimmed.Split(TEXT("="), &OutKey, &OutValue);
	}

	bool Cfg_HasInvalidChars(const FString& Text)
	{
		return Text.Contains(TEXT("\n")) || Text.Contains(TEXT("\r"));
	}

	/** Applies one edit. Returns the description of what happened ("replaced", "added", "removed", "unchanged", "not_found"). */
	FString Cfg_ApplyIniEdit(TArray<FString>& Lines, const FString& Section, const FString& Key, const FString& Value, const FString& Operation)
	{
		int32 SectionStart = INDEX_NONE;
		int32 SectionEnd = Lines.Num();
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			FString Name;
			if (!Cfg_IsSectionHeader(Lines[Index], Name))
			{
				continue;
			}
			if (SectionStart != INDEX_NONE)
			{
				SectionEnd = Index;
				break;
			}
			if (Name.Equals(Section, ESearchCase::IgnoreCase))
			{
				SectionStart = Index;
			}
		}

		if (SectionStart == INDEX_NONE)
		{
			if (Operation == TEXT("remove"))
			{
				return TEXT("not_found");
			}
			if (Lines.Num() > 0 && !Lines.Last().TrimStartAndEnd().IsEmpty())
			{
				Lines.Add(FString());
			}
			Lines.Add(FString::Printf(TEXT("[%s]"), *Section));
			Lines.Add(FString::Printf(TEXT("%s%s=%s"), Operation == TEXT("add") ? TEXT("+") : TEXT(""), *Key, *Value));
			return TEXT("added");
		}

		int32 LastContent = SectionStart;
		for (int32 Index = SectionStart + 1; Index < SectionEnd; ++Index)
		{
			if (!Lines[Index].TrimStartAndEnd().IsEmpty())
			{
				LastContent = Index;
			}
		}

		if (Operation == TEXT("remove"))
		{
			bool bRemoved = false;
			for (int32 Index = SectionEnd - 1; Index > SectionStart; --Index)
			{
				FString LineKey, LineValue;
				if (Cfg_SplitLine(Lines[Index], LineKey, LineValue)
					&& Cfg_StripKeyOperator(LineKey.TrimEnd()).Equals(Key, ESearchCase::IgnoreCase)
					&& (Value.IsEmpty() || LineValue.TrimStartAndEnd() == Value))
				{
					Lines.RemoveAt(Index);
					bRemoved = true;
				}
			}
			return bRemoved ? TEXT("removed") : TEXT("not_found");
		}

		if (Operation == TEXT("add"))
		{
			const FString NewLine = FString::Printf(TEXT("+%s=%s"), *Key, *Value);
			for (int32 Index = SectionStart + 1; Index < SectionEnd; ++Index)
			{
				if (Lines[Index].TrimStartAndEnd() == NewLine)
				{
					return TEXT("unchanged");
				}
			}
			Lines.Insert(NewLine, LastContent + 1);
			return TEXT("added");
		}

		// set: replace the plain "Key=" line, otherwise add it
		const FString NewLine = FString::Printf(TEXT("%s=%s"), *Key, *Value);
		for (int32 Index = SectionStart + 1; Index < SectionEnd; ++Index)
		{
			FString LineKey, LineValue;
			if (Cfg_SplitLine(Lines[Index], LineKey, LineValue) && LineKey.TrimEnd().Equals(Key, ESearchCase::IgnoreCase))
			{
				if (Lines[Index].TrimStartAndEnd() == NewLine)
				{
					return TEXT("unchanged");
				}
				Lines[Index] = NewLine;
				return TEXT("replaced");
			}
		}
		Lines.Insert(NewLine, LastContent + 1);
		return TEXT("added");
	}

	/** key/value pairs from "key"+"value" or the "values" object. */
	bool Cfg_CollectConfigValues(const TSharedPtr<FJsonObject>& Params, TArray<TPair<FString, FString>>& OutPairs, const FString& Operation, FString& OutError)
	{
		FString Key;
		if (Params->TryGetStringField(TEXT("key"), Key) && !Key.IsEmpty())
		{
			FString Value;
			Params->TryGetStringField(TEXT("value"), Value);
			OutPairs.Emplace(Key, Value);
		}
		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (Params->TryGetObjectField(TEXT("values"), Values))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
			{
				FString Text;
				switch (Pair.Value->Type)
				{
				case EJson::Boolean: Text = Pair.Value->AsBool() ? TEXT("True") : TEXT("False"); break;
				case EJson::Number: Text = FString::SanitizeFloat(Pair.Value->AsNumber()); break;
				default: Text = Pair.Value->AsString(); break;
				}
				OutPairs.Emplace(Pair.Key, Text);
			}
		}
		if (OutPairs.Num() == 0)
		{
			OutError = TEXT("Provide key + value, or a values object.");
			return false;
		}
		for (const TPair<FString, FString>& Pair : OutPairs)
		{
			if (Pair.Key.Contains(TEXT("=")) || Cfg_HasInvalidChars(Pair.Key) || Cfg_HasInvalidChars(Pair.Value))
			{
				OutError = FString::Printf(TEXT("Key '%s' or its value contains '=' (in the key) or a line break, which would corrupt the ini file."), *Pair.Key);
				return false;
			}
			if (Operation != TEXT("remove") && Pair.Value.IsEmpty() && Operation == TEXT("add"))
			{
				OutError = FString::Printf(TEXT("Key '%s' needs a value."), *Pair.Key);
				return false;
			}
		}
		return true;
	}

	// ---- Level streaming ----

	TSharedPtr<FJsonObject> Cfg_StreamingToJson(const ULevelStreaming* Level)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("level"), Level->GetWorldAssetPackageName());
		Entry->SetStringField(TEXT("class"), Level->GetClass()->GetName());
		Entry->SetBoolField(TEXT("loaded"), Level->IsLevelLoaded());
		Entry->SetBoolField(TEXT("visible"), Level->IsLevelVisible());
		Entry->SetBoolField(TEXT("should_be_loaded"), Level->ShouldBeLoaded());
		Entry->SetBoolField(TEXT("should_be_visible"), Level->ShouldBeVisible());
		return Entry;
	}

	ULevelStreaming* Cfg_FindStreaming(UWorld* World, const FString& Name)
	{
		for (ULevelStreaming* Level : World->GetStreamingLevels())
		{
			if (!Level)
			{
				continue;
			}
			const FString Package = Level->GetWorldAssetPackageName();
			if (Package.Equals(Name, ESearchCase::IgnoreCase) || FPackageName::GetShortName(Package).Equals(Name, ESearchCase::IgnoreCase))
			{
				return Level;
			}
		}
		return nullptr;
	}
}

// ============================================================================
// set_project_config
// ============================================================================

bool FSetProjectConfigAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString File;
	FString Section;
	if (!GetRequiredString(Params, TEXT("file"), File, OutError) || !GetRequiredString(Params, TEXT("section"), Section, OutError))
	{
		return false;
	}
	FString FileName;
	if (!Cfg_ResolveConfigFile(File, FileName))
	{
		OutError = FString::Printf(TEXT("Unknown config file '%s'. Valid: Engine, Game, Input, Editor, Scalability, GameUserSettings, EditorPerProjectUserSettings."), *File);
		return false;
	}
	Section = Section.TrimStartAndEnd();
	if (Section.StartsWith(TEXT("[")) && Section.EndsWith(TEXT("]")))
	{
		Section = Section.Mid(1, Section.Len() - 2);
	}
	if (Section.IsEmpty() || Section.Contains(TEXT("[")) || Section.Contains(TEXT("]")) || Cfg_HasInvalidChars(Section))
	{
		OutError = TEXT("Invalid section name.");
		return false;
	}
	const FString Operation = GetOptionalString(Params, TEXT("operation"), TEXT("set")).ToLower();
	if (Operation != TEXT("set") && Operation != TEXT("add") && Operation != TEXT("remove"))
	{
		OutError = TEXT("operation must be set, add or remove.");
		return false;
	}
	TArray<TPair<FString, FString>> Pairs;
	return Cfg_CollectConfigValues(Params, Pairs, Operation, OutError);
}

TSharedPtr<FJsonObject> FSetProjectConfigAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString FileName;
	Cfg_ResolveConfigFile(GetOptionalString(Params, TEXT("file")), FileName);
	FString Section = GetOptionalString(Params, TEXT("section")).TrimStartAndEnd();
	if (Section.StartsWith(TEXT("[")) && Section.EndsWith(TEXT("]")))
	{
		Section = Section.Mid(1, Section.Len() - 2);
	}
	const FString Operation = GetOptionalString(Params, TEXT("operation"), TEXT("set")).ToLower();
	const bool bApplyLive = GetOptionalBool(Params, TEXT("apply_live"), false);

	FString Error;
	TArray<TPair<FString, FString>> Pairs;
	Cfg_CollectConfigValues(Params, Pairs, Operation, Error);

	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), FileName));
	TArray<FString> Lines;
	const bool bExisted = FPaths::FileExists(Path);
	if (bExisted && !FFileHelper::LoadFileToStringArray(Lines, *Path))
	{
		return CreateErrorResponse(FString::Printf(TEXT("Could not read %s"), *Path), TEXT("execution_failed"));
	}

	Cfg_FJsonArray Changes;
	Cfg_FJsonArray AppliedLive;
	bool bAnyChange = false;
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		const FString Outcome = Cfg_ApplyIniEdit(Lines, Section, Pair.Key, Pair.Value, Operation);
		bAnyChange |= (Outcome == TEXT("replaced") || Outcome == TEXT("added") || Outcome == TEXT("removed"));

		TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("key"), Pair.Key);
		Change->SetStringField(TEXT("value"), Pair.Value);
		Change->SetStringField(TEXT("result"), Outcome);
		Changes.Add(Cfg_AsValue(Change));

		if (bApplyLive && Operation != TEXT("remove"))
		{
			if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
			{
				Variable->Set(*Pair.Value, ECVF_SetByConsole);
				AppliedLive.Add(MakeShared<FJsonValueString>(Pair.Key));
			}
		}
	}

	if (bAnyChange)
	{
		if (bExisted)
		{
			IFileManager::Get().Copy(*(Path + TEXT(".mcpbak")), *Path);
			if (IFileManager::Get().IsReadOnly(*Path))
			{
				FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Path, false);
			}
		}
		if (!FFileHelper::SaveStringArrayToFile(Lines, *Path))
		{
			return CreateErrorResponse(FString::Printf(TEXT("Could not write %s (read-only or locked by source control?)"), *Path), TEXT("execution_failed"));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("file"), Path);
	Result->SetStringField(TEXT("section"), Section);
	Result->SetStringField(TEXT("operation"), Operation);
	Result->SetBoolField(TEXT("written"), bAnyChange);
	Result->SetBoolField(TEXT("created_file"), bAnyChange && !bExisted);
	if (bAnyChange && bExisted)
	{
		Result->SetStringField(TEXT("backup"), Path + TEXT(".mcpbak"));
	}
	Result->SetArrayField(TEXT("changes"), Changes);
	Result->SetArrayField(TEXT("applied_live"), AppliedLive);
	Result->SetBoolField(TEXT("restart_needed"), bAnyChange && AppliedLive.Num() < Pairs.Num());
	return CreateSuccessResponse(Result);
}

// ============================================================================
// manage_level_streaming
// ============================================================================

bool FManageLevelStreamingAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Action;
	if (!GetRequiredString(Params, TEXT("action"), Action, OutError))
	{
		return false;
	}
	Action = Action.ToLower();
	if (Action != TEXT("list") && Action != TEXT("add") && Action != TEXT("remove") && Action != TEXT("set"))
	{
		OutError = TEXT("action must be list, add, remove or set.");
		return false;
	}
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		OutError = TEXT("No editor world is loaded.");
		return false;
	}
	if (Action == TEXT("list"))
	{
		return true;
	}
	if (FMCPPIE::IsActiveOrPending())
	{
		OutError = FString::Printf(TEXT("manage_level_streaming '%s' edits the level and is blocked while PIE runs. Call stop_pie first."), *Action);
		return false;
	}
	FString Level;
	if (!GetRequiredString(Params, TEXT("level"), Level, OutError))
	{
		return false;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (Action == TEXT("add"))
	{
		if (Cfg_FindStreaming(World, Level))
		{
			OutError = FString::Printf(TEXT("Level '%s' is already a streaming level."), *Level);
			return false;
		}
		if (!Level.StartsWith(TEXT("/")) || !FPackageName::DoesPackageExist(Level))
		{
			OutError = FString::Printf(TEXT("Level package '%s' not found. Use a package path such as /Game/Maps/Sub_Level."), *Level);
			return false;
		}
	}
	else if (!Cfg_FindStreaming(World, Level))
	{
		OutError = FString::Printf(TEXT("Streaming level '%s' not found. Use action=list to see the names."), *Level);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FManageLevelStreamingAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	const FString Action = GetOptionalString(Params, TEXT("action")).ToLower();
	const FString LevelName = GetOptionalString(Params, TEXT("level"));

	if (Action == TEXT("add"))
	{
		const bool bDynamic = GetOptionalString(Params, TEXT("streaming_class"), TEXT("always_loaded")).Equals(TEXT("blueprint"), ESearchCase::IgnoreCase);
		TSubclassOf<ULevelStreaming> StreamingClass = bDynamic ? TSubclassOf<ULevelStreaming>(ULevelStreamingDynamic::StaticClass()) : TSubclassOf<ULevelStreaming>(ULevelStreamingAlwaysLoaded::StaticClass());
		ULevelStreaming* Added = UEditorLevelUtils::AddLevelToWorld(World, *LevelName, StreamingClass);
		if (!Added)
		{
			return CreateErrorResponse(FString::Printf(TEXT("Could not add '%s' as a streaming level."), *LevelName), TEXT("execution_failed"));
		}
		World->MarkPackageDirty();
		Context.RequestSave();
	}
	else if (Action == TEXT("remove"))
	{
		ULevelStreaming* Streaming = Cfg_FindStreaming(World, LevelName);
		ULevel* Loaded = Streaming ? Streaming->GetLoadedLevel() : nullptr;
		if (!Loaded)
		{
			return CreateErrorResponse(TEXT("The streaming level is not loaded in the editor; load it first (action=set should_be_loaded=true)."), TEXT("validation_failed"));
		}
		if (!UEditorLevelUtils::RemoveLevelFromWorld(Loaded))
		{
			return CreateErrorResponse(FString::Printf(TEXT("Could not remove '%s'."), *LevelName), TEXT("execution_failed"));
		}
		World->MarkPackageDirty();
		Context.RequestSave();
	}
	else if (Action == TEXT("set"))
	{
		ULevelStreaming* Streaming = Cfg_FindStreaming(World, LevelName);
		if (Cfg_HasBool(Params, TEXT("should_be_loaded")))
		{
			Streaming->SetShouldBeLoaded(GetOptionalBool(Params, TEXT("should_be_loaded")));
		}
		if (Cfg_HasBool(Params, TEXT("should_be_visible")))
		{
			Streaming->SetShouldBeVisibleInEditor(GetOptionalBool(Params, TEXT("should_be_visible")));
		}
		World->MarkPackageDirty();
		Context.RequestSave();
	}

	Cfg_FJsonArray Levels;
	for (const ULevelStreaming* Level : World->GetStreamingLevels())
	{
		if (Level)
		{
			Levels.Add(Cfg_AsValue(Cfg_StreamingToJson(Level)));
		}
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("action"), Action);
	Result->SetNumberField(TEXT("count"), Levels.Num());
	Result->SetArrayField(TEXT("streaming_levels"), Levels);
	return CreateSuccessResponse(Result);
}

// ============================================================================
// build_level
// ============================================================================

bool FBuildLevelAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString What;
	if (!GetRequiredString(Params, TEXT("what"), What, OutError))
	{
		return false;
	}
	What = What.ToLower();
	if (What != TEXT("lighting") && What != TEXT("navmesh") && What != TEXT("hlod") && What != TEXT("geometry") && What != TEXT("status"))
	{
		OutError = TEXT("what must be lighting, navmesh, hlod, geometry or status.");
		return false;
	}
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		OutError = TEXT("No editor world is loaded.");
		return false;
	}
	const FString Quality = GetOptionalString(Params, TEXT("quality"), TEXT("preview")).ToLower();
	if (Quality != TEXT("preview") && Quality != TEXT("medium") && Quality != TEXT("high") && Quality != TEXT("production"))
	{
		OutError = TEXT("quality must be preview, medium, high or production.");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FBuildLevelAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString What = GetOptionalString(Params, TEXT("what")).ToLower();
	UWorld* World = GEditor->GetEditorWorldContext().World();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("what"), What);
	Result->SetStringField(TEXT("level"), World->GetMapName());
	Result->SetBoolField(TEXT("lighting_running"), GEditor->IsLightingBuildCurrentlyRunning());
	Result->SetBoolField(TEXT("lighting_exporting"), GEditor->IsLightingBuildCurrentlyExporting());
	if (What == TEXT("status"))
	{
		return CreateSuccessResponse(Result);
	}

	if (GEditor->IsLightingBuildCurrentlyRunning())
	{
		return CreateErrorResponse(TEXT("A lighting build is already running. Poll build_level what=status until lighting_running is false."), TEXT("validation_failed"));
	}

	// Persist pending edits first so the build never shows a save prompt.
	Context.SaveDirtyPackages();

	FName BuildId = FBuildOptions::BuildGeometry;
	if (What == TEXT("lighting"))
	{
		BuildId = FBuildOptions::BuildLighting;
		const FString Quality = GetOptionalString(Params, TEXT("quality"), TEXT("preview")).ToLower();
		const int32 QualityLevel = Quality == TEXT("preview") ? 0 : Quality == TEXT("medium") ? 1 : Quality == TEXT("high") ? 2 : 3;
		GConfig->SetInt(TEXT("LightingBuildOptions"), TEXT("QualityLevel"), QualityLevel, GEditorPerProjectIni);
		Result->SetStringField(TEXT("quality"), Quality);
	}
	else if (What == TEXT("navmesh"))
	{
		BuildId = FBuildOptions::BuildAIPaths;
	}
	else if (What == TEXT("hlod"))
	{
		BuildId = FBuildOptions::BuildHierarchicalLOD;
	}

	const bool bStarted = FEditorBuildUtils::EditorBuild(World, BuildId, false);
	Result->SetBoolField(TEXT("started"), bStarted);
	Result->SetBoolField(TEXT("lighting_running"), GEditor->IsLightingBuildCurrentlyRunning());
	if (What == TEXT("lighting"))
	{
		Result->SetStringField(TEXT("note"), TEXT("The lighting build runs in the background. Poll build_level what=status until lighting_running is false, then save_all."));
	}
	if (!bStarted)
	{
		return CreateErrorResponse(FString::Printf(TEXT("The editor refused to start the '%s' build (it may have been cancelled or the level has nothing to build). Check get_output_log."), *What), TEXT("execution_failed"));
	}
	return CreateSuccessResponse(Result);
}
