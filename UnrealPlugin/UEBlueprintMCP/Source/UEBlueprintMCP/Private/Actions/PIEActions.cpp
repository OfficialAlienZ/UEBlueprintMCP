// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/PIEActions.h"
#include "MCPLogCapture.h"
#include "MCPCompat.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDevice.h"

namespace
{
	/** Collects text written to a console command's output archive. */
	class FPIECaptureOutput : public FOutputDevice
	{
	public:
		FString Text;

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (!Text.IsEmpty())
			{
				Text += TEXT("\n");
			}
			Text += Message;
		}
	};

	/** Seconds a start request is considered "starting" before we assume it failed. */
	constexpr double PIEStartTimeoutSeconds = 30.0;

	bool PIE_ParseVerbosity(const FString& Name, ELogVerbosity::Type& OutVerbosity)
	{
		if (Name.Equals(TEXT("Error"), ESearchCase::IgnoreCase)) { OutVerbosity = ELogVerbosity::Error; return true; }
		if (Name.Equals(TEXT("Warning"), ESearchCase::IgnoreCase)) { OutVerbosity = ELogVerbosity::Warning; return true; }
		if (Name.Equals(TEXT("Display"), ESearchCase::IgnoreCase)) { OutVerbosity = ELogVerbosity::Display; return true; }
		if (Name.Equals(TEXT("Log"), ESearchCase::IgnoreCase)) { OutVerbosity = ELogVerbosity::Log; return true; }
		if (Name.Equals(TEXT("Verbose"), ESearchCase::IgnoreCase)) { OutVerbosity = ELogVerbosity::Verbose; return true; }
		if (Name.Equals(TEXT("VeryVerbose"), ESearchCase::IgnoreCase)) { OutVerbosity = ELogVerbosity::VeryVerbose; return true; }
		return false;
	}

	const TCHAR* PIE_VerbosityName(ELogVerbosity::Type Verbosity)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Fatal: return TEXT("Fatal");
		case ELogVerbosity::Error: return TEXT("Error");
		case ELogVerbosity::Warning: return TEXT("Warning");
		case ELogVerbosity::Display: return TEXT("Display");
		case ELogVerbosity::Log: return TEXT("Log");
		case ELogVerbosity::Verbose: return TEXT("Verbose");
		case ELogVerbosity::VeryVerbose: return TEXT("VeryVerbose");
		default: return TEXT("Unknown");
		}
	}

	const TCHAR* PIE_NetModeName(ENetMode NetMode)
	{
		switch (NetMode)
		{
		case NM_Standalone: return TEXT("Standalone");
		case NM_DedicatedServer: return TEXT("DedicatedServer");
		case NM_ListenServer: return TEXT("ListenServer");
		case NM_Client: return TEXT("Client");
		default: return TEXT("Unknown");
		}
	}

	/** First whitespace-delimited token of a console command, lower-cased. */
	FString PIE_FirstToken(const FString& Command)
	{
		FString Trimmed = Command.TrimStartAndEnd();
		int32 SpaceIndex = INDEX_NONE;
		if (Trimmed.FindChar(TEXT(' '), SpaceIndex))
		{
			Trimmed.LeftInline(SpaceIndex);
		}
		return Trimmed.ToLower();
	}
}

// ============================================================================
// FMCPPIE
// ============================================================================

double FMCPPIE::StartRequestTime = 0.0;

bool FMCPPIE::IsRunning()
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

bool FMCPPIE::IsStartPending()
{
	if (IsRunning())
	{
		// Session began: the request is fulfilled.
		StartRequestTime = 0.0;
		return false;
	}
	return StartRequestTime > 0.0 && (FPlatformTime::Seconds() - StartRequestTime) < PIEStartTimeoutSeconds;
}

void FMCPPIE::MarkStartRequested()
{
	StartRequestTime = FPlatformTime::Seconds();
}

void FMCPPIE::ClearStartRequest()
{
	StartRequestTime = 0.0;
}

bool FMCPPIE::ConsumeExpiredStartRequest()
{
	if (StartRequestTime > 0.0 && !IsRunning() && !IsStartPending())
	{
		StartRequestTime = 0.0;
		return true;
	}
	return false;
}

// ============================================================================
// FStartPIEAction
// ============================================================================

bool FStartPIEAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		OutError = TEXT("No editor world is loaded. Open a level before starting PIE.");
		return false;
	}
	if (FMCPPIE::IsRunning())
	{
		OutError = TEXT("A PIE session is already running. Call stop_pie first.");
		return false;
	}
	if (FMCPPIE::IsStartPending())
	{
		OutError = TEXT("A PIE start is already pending. Poll get_pie_status until state is 'running'.");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FStartPIEAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const bool bSimulate = GetOptionalBool(Params, TEXT("simulate"), false);
	if (GetOptionalBool(Params, TEXT("clear_log"), false))
	{
		FMCPLogCapture::Get().Clear();
	}

	// Capture the cursor first so callers can read everything logged by the session.
	const int64 LogCursor = FMCPLogCapture::Get().GetLatestId();

	// Persist any deferred auto-save now; saving is blocked once PIE is running
	Context.FlushPendingSave();

	FMCPPIE::MarkStartRequested();
	MCPCompat::RequestPlaySession(bSimulate);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("state"), TEXT("starting"));
	Result->SetBoolField(TEXT("simulate"), bSimulate);
	Result->SetNumberField(TEXT("log_cursor"), static_cast<double>(LogCursor));
	Result->SetStringField(TEXT("message"), TEXT("PIE start requested. Poll get_pie_status until state is 'running'."));
	return CreateSuccessResponse(Result);
}

// ============================================================================
// FStopPIEAction
// ============================================================================

bool FStopPIEAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (FMCPPIE::IsRunning())
	{
		return true;
	}
	OutError = FMCPPIE::IsStartPending()
		? TEXT("PIE is still starting. Wait until get_pie_status reports 'running', then call stop_pie.")
		: TEXT("No PIE session is running.");
	return false;
}

TSharedPtr<FJsonObject> FStopPIEAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FMCPPIE::ClearStartRequest();
	GEditor->RequestEndPlayMap();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("state"), TEXT("stopping"));
	Result->SetStringField(TEXT("message"), TEXT("PIE stop requested. Poll get_pie_status until state is 'stopped'."));
	return CreateSuccessResponse(Result);
}

// ============================================================================
// FGetPIEStatusAction
// ============================================================================

TSharedPtr<FJsonObject> FGetPIEStatusAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (FMCPPIE::IsRunning())
	{
		UWorld* PlayWorld = GEditor->PlayWorld;
		Result->SetStringField(TEXT("state"), TEXT("running"));
		Result->SetBoolField(TEXT("simulate"), GEditor->bIsSimulatingInEditor);
		Result->SetStringField(TEXT("world"), PlayWorld->GetName());
		Result->SetStringField(TEXT("net_mode"), PIE_NetModeName(PlayWorld->GetNetMode()));
		Result->SetNumberField(TEXT("time_seconds"), PlayWorld->GetTimeSeconds());
		Result->SetBoolField(TEXT("paused"), PlayWorld->IsPaused());
		Result->SetNumberField(TEXT("actor_count"), PlayWorld->GetActorCount());

		if (APlayerController* PC = PlayWorld->GetFirstPlayerController())
		{
			TSharedPtr<FJsonObject> Player = MakeShared<FJsonObject>();
			Player->SetStringField(TEXT("controller"), PC->GetName());
			Player->SetStringField(TEXT("controller_class"), PC->GetClass()->GetName());
			if (APawn* Pawn = PC->GetPawn())
			{
				Player->SetStringField(TEXT("pawn"), Pawn->GetName());
				Player->SetStringField(TEXT("pawn_class"), Pawn->GetClass()->GetName());
				const FVector Location = Pawn->GetActorLocation();
				Player->SetArrayField(TEXT("pawn_location"), TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(Location.X), MakeShared<FJsonValueNumber>(Location.Y), MakeShared<FJsonValueNumber>(Location.Z)});
			}
			Player->SetBoolField(TEXT("input_ignored"), PC->IsMoveInputIgnored() && PC->IsLookInputIgnored());
			Result->SetObjectField(TEXT("player"), Player);
		}
	}
	else if (FMCPPIE::IsStartPending())
	{
		Result->SetStringField(TEXT("state"), TEXT("starting"));
	}
	else
	{
		Result->SetStringField(TEXT("state"), TEXT("stopped"));
		if (FMCPPIE::ConsumeExpiredStartRequest())
		{
			Result->SetStringField(TEXT("warning"),
				TEXT("A PIE start was requested but the session never began. Check get_output_log (min_verbosity=Warning) and look for a modal dialog in the editor (e.g. Blueprint compile errors)."));
		}
	}

	Result->SetNumberField(TEXT("log_cursor"), static_cast<double>(FMCPLogCapture::Get().GetLatestId()));
	return CreateSuccessResponse(Result);
}

// ============================================================================
// FGetOutputLogAction
// ============================================================================

bool FGetOutputLogAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	const FString Verbosity = GetOptionalString(Params, TEXT("min_verbosity"));
	ELogVerbosity::Type Parsed;
	if (!Verbosity.IsEmpty() && !PIE_ParseVerbosity(Verbosity, Parsed))
	{
		OutError = FString::Printf(TEXT("Invalid min_verbosity '%s'. Use Error, Warning, Display, Log, Verbose or VeryVerbose."), *Verbosity);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetOutputLogAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FMCPLogQuery Query;
	Query.SinceId = static_cast<int64>(GetOptionalNumber(Params, TEXT("since_id"), -1.0));
	Query.Limit = static_cast<int32>(GetOptionalNumber(Params, TEXT("limit"), 200.0));
	Query.Contains = GetOptionalString(Params, TEXT("contains"));
	Query.bIncludeMCP = GetOptionalBool(Params, TEXT("include_mcp"), false);

	const FString Verbosity = GetOptionalString(Params, TEXT("min_verbosity"));
	if (!Verbosity.IsEmpty())
	{
		PIE_ParseVerbosity(Verbosity, Query.MinSeverity);
	}

	if (const TArray<TSharedPtr<FJsonValue>>* Categories = GetOptionalArray(Params, TEXT("categories")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Categories)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
			{
				Query.Categories.Add(Name);
			}
		}
	}

	TArray<FMCPLogEntry> Entries;
	int64 NextId = 0;
	bool bTruncated = false;
	bool bGap = false;
	FMCPLogCapture::Get().Query(Query, Entries, NextId, bTruncated, bGap);

	TArray<TSharedPtr<FJsonValue>> EntryArray;
	for (const FMCPLogEntry& Entry : Entries)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("id"), static_cast<double>(Entry.Id));
		Item->SetStringField(TEXT("time"), Entry.TimeUtc.ToIso8601());
		Item->SetStringField(TEXT("verbosity"), PIE_VerbosityName(Entry.Verbosity));
		Item->SetStringField(TEXT("category"), Entry.Category.ToString());
		Item->SetStringField(TEXT("message"), Entry.Message);
		EntryArray.Add(MakeShared<FJsonValueObject>(Item));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("entries"), EntryArray);
	Result->SetNumberField(TEXT("count"), EntryArray.Num());
	Result->SetNumberField(TEXT("next_id"), static_cast<double>(NextId));
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetBoolField(TEXT("dropped"), bGap);

	if (GetOptionalBool(Params, TEXT("clear"), false))
	{
		FMCPLogCapture::Get().Clear();
	}

	return CreateSuccessResponse(Result);
}

// ============================================================================
// FExecuteConsoleCommandAction
// ============================================================================

bool FExecuteConsoleCommandAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Command;
	if (!GetRequiredString(Params, TEXT("command"), Command, OutError))
	{
		return false;
	}

	const FString Token = PIE_FirstToken(Command);
	if (Token == TEXT("quit") || Token == TEXT("exit") || Token == TEXT("crash") || Token == TEXT("py"))
	{
		OutError = FString::Printf(TEXT("Console command '%s' is not allowed through MCP."), *Token);
		return false;
	}

	if (!GEditor)
	{
		OutError = TEXT("Editor is not available.");
		return false;
	}

	const FString WorldName = GetOptionalString(Params, TEXT("world"), TEXT("auto")).ToLower();
	if (WorldName != TEXT("auto") && WorldName != TEXT("pie") && WorldName != TEXT("editor"))
	{
		OutError = FString::Printf(TEXT("Invalid world '%s'. Use auto, pie or editor."), *WorldName);
		return false;
	}
	if (WorldName == TEXT("pie") && !FMCPPIE::IsRunning())
	{
		OutError = TEXT("world 'pie' requested but no PIE session is running. Call start_pie first.");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FExecuteConsoleCommandAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString Command = GetOptionalString(Params, TEXT("command")).TrimStartAndEnd();
	const FString WorldName = GetOptionalString(Params, TEXT("world"), TEXT("auto")).ToLower();

	const bool bUsePIE = WorldName == TEXT("pie") || (WorldName == TEXT("auto") && FMCPPIE::IsRunning());
	// static_cast: PlayWorld is a TObjectPtr in UE5 (the ternary is ambiguous in 5.8) and a raw pointer in 4.27
	UWorld* World = bUsePIE ? static_cast<UWorld*>(GEditor->PlayWorld) : GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return CreateErrorResponse(TEXT("Target world is not available."), TEXT("not_found"));
	}

	FPIECaptureOutput Output;
	const bool bHandled = GEngine->Exec(World, *Command, Output);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("command"), Command);
	Result->SetStringField(TEXT("world"), bUsePIE ? TEXT("pie") : TEXT("editor"));
	Result->SetBoolField(TEXT("handled"), bHandled);
	Result->SetStringField(TEXT("output"), Output.Text);
	return CreateSuccessResponse(Result);
}
