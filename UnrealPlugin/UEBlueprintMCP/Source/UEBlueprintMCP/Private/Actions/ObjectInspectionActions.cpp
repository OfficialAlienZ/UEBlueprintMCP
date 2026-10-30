// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/ObjectInspectionActions.h"
#include "MCPCommonUtils.h"
#include "MCPInspectionUtils.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Components/ActorComponent.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> ObjectInspect_FJsonArray;
	typedef TArray<TPair<FString, FString>> ObjectInspect_FEntries;

	UActorComponent* ObjectInspect_FindComponent(AActor* Actor, const FString& ComponentName)
	{
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		return nullptr;
	}

	// ---- Config ----

	/** Config file name -> cache name (GEngineIni, ...) and the project's Default*.ini file name. */
	bool ObjectInspect_ResolveConfigFile(const FString& File, FString& OutIniName, FString& OutDefaultFile)
	{
		struct FConfigFileEntry
		{
			const TCHAR* Name;
			const FString* Ini;
		};
		const FConfigFileEntry Entries[] =
		{
			{ TEXT("Engine"), &GEngineIni },
			{ TEXT("Game"), &GGameIni },
			{ TEXT("Input"), &GInputIni },
			{ TEXT("Editor"), &GEditorIni },
			{ TEXT("EditorPerProjectUserSettings"), &GEditorPerProjectIni },
			{ TEXT("GameUserSettings"), &GGameUserSettingsIni },
			{ TEXT("Scalability"), &GScalabilityIni },
		};
		for (const FConfigFileEntry& Entry : Entries)
		{
			if (File.Equals(Entry.Name, ESearchCase::IgnoreCase))
			{
				OutIniName = *Entry.Ini;
				OutDefaultFile = FString::Printf(TEXT("Default%s.ini"), Entry.Name);
				return true;
			}
		}
		return false;
	}

	/** Key without the ini array operators (+ - . !). */
	FString ObjectInspect_StripOperator(const FString& Key)
	{
		int32 Start = 0;
		while (Start < Key.Len() && (Key[Start] == '+' || Key[Start] == '-' || Key[Start] == '.' || Key[Start] == '!'))
		{
			++Start;
		}
		return Key.Mid(Start);
	}

	/** Raw Default*.ini: section names and the entries of Section, in file order. */
	void ObjectInspect_ParseRawIni(const TArray<FString>& Lines, const FString& Section, TArray<FString>& OutSections, ObjectInspect_FEntries& OutEntries)
	{
		FString Current;
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")))
			{
				continue;
			}
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				Current = Line.Mid(1, Line.Len() - 2);
				OutSections.AddUnique(Current);
				continue;
			}
			FString Key, Value;
			if (!Section.IsEmpty() && Current.Equals(Section, ESearchCase::IgnoreCase) && Line.Split(TEXT("="), &Key, &Value))
			{
				OutEntries.Emplace(Key.TrimEnd(), Value.TrimStart());
			}
		}
	}

	TSharedPtr<FJsonObject> ObjectInspect_EntriesToValues(const ObjectInspect_FEntries& Entries)
	{
		TArray<FString> Order;
		TMap<FString, TArray<FString>> Grouped;
		for (const TPair<FString, FString>& Entry : Entries)
		{
			if (!Grouped.Contains(Entry.Key))
			{
				Order.Add(Entry.Key);
			}
			Grouped.FindOrAdd(Entry.Key).Add(Entry.Value);
		}

		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		for (const FString& Key : Order)
		{
			const TArray<FString>& Group = Grouped[Key];
			if (Group.Num() == 1)
			{
				Values->SetStringField(Key, Group[0]);
			}
			else
			{
				ObjectInspect_FJsonArray Array;
				for (const FString& Value : Group)
				{
					Array.Add(MakeShared<FJsonValueString>(Value));
				}
				Values->SetArrayField(Key, Array);
			}
		}
		return Values;
	}

	bool ObjectInspect_IsRawLayer(const FString& Layer)
	{
		return Layer.Equals(TEXT("project_default"), ESearchCase::IgnoreCase);
	}
}

// ============================================================================
// get_object_properties
// ============================================================================

UObject* FGetObjectPropertiesAction::ResolveTarget(const TSharedPtr<FJsonObject>& Params, FString& OutError) const
{
	const FString ObjectPath = GetOptionalString(Params, TEXT("object_path"));
	const FString ActorName = GetOptionalString(Params, TEXT("actor_name"));
	const FString BlueprintName = GetOptionalString(Params, TEXT("blueprint_name"));
	const FString ComponentName = GetOptionalString(Params, TEXT("component_name"));

	const int32 TargetCount = (!ObjectPath.IsEmpty() ? 1 : 0) + (!ActorName.IsEmpty() ? 1 : 0) + (!BlueprintName.IsEmpty() ? 1 : 0);
	if (TargetCount != 1)
	{
		OutError = TEXT("Specify exactly one of object_path, actor_name or blueprint_name");
		return nullptr;
	}
	if (!ComponentName.IsEmpty() && ActorName.IsEmpty() && BlueprintName.IsEmpty())
	{
		OutError = TEXT("component_name needs actor_name or blueprint_name");
		return nullptr;
	}

	// Only the actor / world_settings targets need a world; a bad "world" value is reported for those.
	FString WorldError;
	UWorld* World = ResolveWorld(Params, WorldError);

	// ---- Blueprint class defaults / component template ----
	if (!BlueprintName.IsEmpty())
	{
		UBlueprint* Blueprint = FindBlueprint(BlueprintName, OutError);
		if (!Blueprint)
		{
			return nullptr;
		}
		UObject* DefaultObject = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
		if (!DefaultObject)
		{
			OutError = FString::Printf(TEXT("Blueprint '%s' has no generated class; compile it first"), *BlueprintName);
			return nullptr;
		}
		if (ComponentName.IsEmpty())
		{
			return DefaultObject;
		}
		if (const USCS_Node* Node = FMCPCommonUtils::FindComponentNode(Blueprint, ComponentName))
		{
			if (Node->ComponentTemplate)
			{
				return Node->ComponentTemplate;
			}
		}
		if (AActor* ActorDefault = Cast<AActor>(DefaultObject))
		{
			if (UActorComponent* Native = ObjectInspect_FindComponent(ActorDefault, ComponentName))
			{
				return Native;
			}
		}
		OutError = FString::Printf(TEXT("Component '%s' not found in Blueprint '%s'"), *ComponentName, *BlueprintName);
		return nullptr;
	}

	// ---- Actor in the level / component ----
	if (!ActorName.IsEmpty())
	{
		if (!World)
		{
			OutError = WorldError;
			return nullptr;
		}
		AActor* Found = FMCPCommonUtils::FindActor(World, ActorName);
		if (!Found)
		{
			OutError = FString::Printf(TEXT("Actor not found: %s"), *ActorName);
			return nullptr;
		}
		if (ComponentName.IsEmpty())
		{
			return Found;
		}
		if (UActorComponent* Component = ObjectInspect_FindComponent(Found, ComponentName))
		{
			return Component;
		}
		OutError = FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *ComponentName, *ActorName);
		return nullptr;
	}

	// ---- Object path ----
	if (ObjectPath.Equals(TEXT("world_settings"), ESearchCase::IgnoreCase))
	{
		AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		if (!Settings)
		{
			OutError = WorldError.IsEmpty() ? FString(TEXT("World settings are not available")) : WorldError;
		}
		return Settings;
	}
	if (!ObjectPath.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(TEXT("object_path '%s' must be a full object path (e.g. /Script/Engine.Default__WorldSettings or /Game/Data/DA_Config) or 'world_settings'"), *ObjectPath);
		return nullptr;
	}

	FString Path = ObjectPath;
	if (!Path.Contains(TEXT(".")))
	{
		// Asset path without object name: /Game/Folder/Name -> /Game/Folder/Name.Name
		FString ShortName;
		Path.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		Path = Path + TEXT(".") + ShortName;
	}
	UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *Path);
	if (!Object)
	{
		Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path, nullptr, LOAD_NoWarn);
	}
	if (!Object)
	{
		OutError = FString::Printf(TEXT("Object '%s' not found"), *ObjectPath);
	}
	return Object;
}

bool FGetObjectPropertiesAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	return ResolveTarget(Params, OutError) != nullptr;
}

TSharedPtr<FJsonObject> FGetObjectPropertiesAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString Error;
	UObject* Object = ResolveTarget(Params, Error);
	if (!Object)
	{
		return CreateErrorResponse(Error, TEXT("not_found"));
	}

	TArray<FString> PropertyNames;
	if (const TArray<TSharedPtr<FJsonValue>>* Names = GetOptionalArray(Params, TEXT("property_names")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Names)
		{
			PropertyNames.Add(Value->AsString());
		}
	}

	TArray<FString> Unknown;
	TSharedPtr<FJsonObject> Properties = FMCPInspectionUtils::ExportAllProperties(
		Object, PropertyNames, GetOptionalBool(Params, TEXT("editable_only"), false), &Unknown);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Object->GetName());
	Result->SetStringField(TEXT("class"), Object->GetClass()->GetName());
	Result->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
	Result->SetStringField(TEXT("path"), Object->GetPathName());
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object->GetClass()->ClassGeneratedBy))
	{
		Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	}
	Result->SetNumberField(TEXT("property_count"), Properties->Values.Num());
	Result->SetObjectField(TEXT("properties"), Properties);
	if (Unknown.Num() > 0)
	{
		ObjectInspect_FJsonArray UnknownValues;
		for (const FString& Name : Unknown)
		{
			UnknownValues.Add(MakeShared<FJsonValueString>(Name));
		}
		Result->SetArrayField(TEXT("unknown_properties"), UnknownValues);
	}
	return CreateSuccessResponse(Result);
}

// ============================================================================
// get_project_config
// ============================================================================

bool FGetProjectConfigAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString File;
	if (!GetRequiredString(Params, TEXT("file"), File, OutError))
	{
		return false;
	}
	FString IniName, DefaultFile;
	if (!ObjectInspect_ResolveConfigFile(File, IniName, DefaultFile))
	{
		OutError = FString::Printf(TEXT("Unknown config file '%s'. Valid files: Engine, Game, Input, Editor, EditorPerProjectUserSettings, GameUserSettings, Scalability"), *File);
		return false;
	}

	const FString Layer = GetOptionalString(Params, TEXT("layer"), TEXT("merged"));
	if (!Layer.Equals(TEXT("merged"), ESearchCase::IgnoreCase) && !ObjectInspect_IsRawLayer(Layer))
	{
		OutError = FString::Printf(TEXT("Unknown layer '%s'. Valid layers: merged, project_default"), *Layer);
		return false;
	}

	if (GetOptionalString(Params, TEXT("section")).IsEmpty() && !GetOptionalString(Params, TEXT("key")).IsEmpty())
	{
		OutError = TEXT("'key' needs a 'section'");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetProjectConfigAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString File = GetOptionalString(Params, TEXT("file"));
	const FString Section = GetOptionalString(Params, TEXT("section"));
	const FString Key = GetOptionalString(Params, TEXT("key"));
	const FString Contains = GetOptionalString(Params, TEXT("contains"));
	const FString Layer = GetOptionalString(Params, TEXT("layer"), TEXT("merged"));
	const bool bRaw = ObjectInspect_IsRawLayer(Layer);

	FString IniName, DefaultFile;
	ObjectInspect_ResolveConfigFile(File, IniName, DefaultFile);

	TArray<FString> SectionNames;
	ObjectInspect_FEntries Entries;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("file"), File);
	Result->SetStringField(TEXT("layer"), bRaw ? TEXT("project_default") : TEXT("merged"));

	if (bRaw)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectConfigDir(), DefaultFile));
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *Path))
		{
			return CreateErrorResponse(FString::Printf(TEXT("Config file not found or unreadable: %s"), *Path), TEXT("not_found"));
		}
		Result->SetStringField(TEXT("path"), Path);
		ObjectInspect_ParseRawIni(Lines, Section, SectionNames, Entries);
	}
	else
	{
		if (!GConfig)
		{
			return CreateErrorResponse(TEXT("GConfig is not available"), TEXT("execution_failed"));
		}
		Result->SetStringField(TEXT("path"), IniName);
		GConfig->GetSectionNames(IniName, SectionNames);
		if (!Section.IsEmpty())
		{
			TArray<FString> Lines;
			GConfig->GetSection(*Section, Lines, IniName);
			for (const FString& Line : Lines)
			{
				FString EntryKey, EntryValue;
				if (Line.Split(TEXT("="), &EntryKey, &EntryValue))
				{
					Entries.Emplace(EntryKey, EntryValue);
				}
			}
		}
	}

	// ---- Section list ----
	if (Section.IsEmpty())
	{
		ObjectInspect_FJsonArray Names;
		for (const FString& Name : SectionNames)
		{
			if (Contains.IsEmpty() || Name.Contains(Contains, ESearchCase::IgnoreCase))
			{
				Names.Add(MakeShared<FJsonValueString>(Name));
			}
		}
		Result->SetNumberField(TEXT("count"), Names.Num());
		Result->SetArrayField(TEXT("sections"), Names);
		return CreateSuccessResponse(Result);
	}

	// ---- Entries of one section (optionally one key) ----
	bool bSectionFound = false;
	for (const FString& Name : SectionNames)
	{
		if (Name.Equals(Section, ESearchCase::IgnoreCase))
		{
			bSectionFound = true;
			break;
		}
	}
	if (!bSectionFound)
	{
		return CreateErrorResponse(
			FString::Printf(TEXT("Section '%s' not found in %s (%s layer). Call without 'section' (optionally with 'contains') to list the sections."), *Section, *File, bRaw ? TEXT("project_default") : TEXT("merged")),
			TEXT("not_found"));
	}

	if (!Key.IsEmpty())
	{
		const FString WantedKey = ObjectInspect_StripOperator(Key);
		ObjectInspect_FEntries Filtered;
		for (const TPair<FString, FString>& Entry : Entries)
		{
			if (ObjectInspect_StripOperator(Entry.Key).Equals(WantedKey, ESearchCase::IgnoreCase))
			{
				Filtered.Add(Entry);
			}
		}
		if (Filtered.Num() == 0)
		{
			return CreateErrorResponse(
				FString::Printf(TEXT("Key '%s' not found in section '%s' of %s (%s layer)"), *Key, *Section, *File, bRaw ? TEXT("project_default") : TEXT("merged")),
				TEXT("not_found"));
		}
		Entries = MoveTemp(Filtered);
	}

	Result->SetStringField(TEXT("section"), Section);
	Result->SetNumberField(TEXT("count"), Entries.Num());
	Result->SetObjectField(TEXT("values"), ObjectInspect_EntriesToValues(Entries));
	return CreateSuccessResponse(Result);
}
