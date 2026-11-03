// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/AutomationActions.h"
#include "Actions/PIEActions.h"
#include "MCPCompat.h"
#include "MCPLogCapture.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

namespace
{
	/** One automation event (error / warning / info) recorded by a test. */
	struct FMCPTestEvent
	{
		EAutomationEventType Type = EAutomationEventType::Info;
		FString Message;
		FString Context;
		FString File;
		int32 Line = -1;
	};

	struct FMCPTestResult
	{
		FString Name;
		FString Path;
		bool bSuccess = false;
		bool bTimedOut = false;
		double Duration = 0.0;
		int32 Errors = 0;
		int32 Warnings = 0;
		TArray<FMCPTestEvent> Events;
	};

	/**
	 * Runs a batch of automation tests one after another from a core ticker, mirroring what the
	 * AutomationWorker does for the Test Automation window: start the test, then execute its latent
	 * commands every frame until the queue is empty, then stop it and collect the results.
	 */
	class FMCPAutomationRunner
	{
	public:
		static FMCPAutomationRunner& Get()
		{
			static FMCPAutomationRunner Instance;
			return Instance;
		}

		bool IsRunning() const { return bRunning; }

		void Start(const TArray<FAutomationTestInfo>& InTests, double InTimeoutSeconds)
		{
			Queue = InTests;
			Results.Empty();
			QueueIndex = 0;
			bCurrentStarted = false;
			TimeoutSeconds = InTimeoutSeconds;
			BatchStartTime = FPlatformTime::Seconds();
			ElapsedAtEnd = 0.0;
			LogCursor = FMCPLogCapture::Get().GetLatestId();
			bRunning = true;
			TickerHandle = MCPCompat::AddCoreTicker([this](float) { return Tick(); }, 0.0f);
		}

		/** Stop the batch. The current test (if any) is recorded as failed / cancelled. */
		void Cancel()
		{
			if (!bRunning)
			{
				return;
			}
			if (bCurrentStarted)
			{
				FAutomationTestFramework::Get().DequeueAllCommands();
				CompleteCurrent(false, TEXT("Cancelled by cancel request"));
			}
			EndBatch();
			MCPCompat::RemoveCoreTicker(TickerHandle);
			TickerHandle.Reset();
		}

		int32 GetTotal() const { return Queue.Num(); }
		int32 GetQueueIndex() const { return QueueIndex; }
		const TArray<FMCPTestResult>& GetResults() const { return Results; }
		int64 GetLogCursor() const { return LogCursor; }

		FString GetCurrentName() const
		{
			return (bRunning && Queue.IsValidIndex(QueueIndex)) ? Queue[QueueIndex].GetTestName() : FString();
		}

		double GetElapsed() const
		{
			return bRunning ? FPlatformTime::Seconds() - BatchStartTime : ElapsedAtEnd;
		}

	private:
		bool Tick()
		{
			if (!bRunning)
			{
				return false;
			}
			FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

			if (!bCurrentStarted)
			{
				if (QueueIndex >= Queue.Num())
				{
					EndBatch();
					return false;
				}
				// A previous test's PIE session may still be shutting down; never start on top of it.
				if (GEditor && (GEditor->PlayWorld || GIsPlayInEditorWorld))
				{
					return true;
				}
				CurrentStartTime = FPlatformTime::Seconds();
				Framework.StartTestByName(Queue[QueueIndex].GetTestName(), 0);
				if (!GIsAutomationTesting)
				{
					CompleteCurrent(false, TEXT("The test could not be started (see get_output_log, LogAutomationTest)"));
					return true;
				}
				bCurrentStarted = true;
				return true;
			}

			if (FPlatformTime::Seconds() - CurrentStartTime > TimeoutSeconds)
			{
				Framework.DequeueAllCommands();
				CompleteCurrent(true, FString::Printf(TEXT("Timed out after %.0f seconds"), TimeoutSeconds));
				if (GEditor && GEditor->PlayWorld)
				{
					GEditor->RequestEndPlayMap();
				}
				return true;
			}

			// Latent commands run on the frame after the previous one so the rest of the engine gets a turn.
			if (Framework.ExecuteLatentCommands())
			{
				if (Framework.ExecuteNetworkCommands())
				{
					CompleteCurrent(false, FString());
				}
			}
			return true;
		}

		void CompleteCurrent(bool bTimedOut, const FString& ExtraError)
		{
			FMCPTestResult Result;
			Result.Name = Queue[QueueIndex].GetTestName();
			Result.Path = Queue[QueueIndex].GetFullTestPath();
			Result.bTimedOut = bTimedOut;

			FAutomationTestExecutionInfo Info;
			bool bSuccess = false;
			if (GIsAutomationTesting)
			{
				bSuccess = FAutomationTestFramework::Get().StopTest(Info);
			}
			Result.Duration = FPlatformTime::Seconds() - CurrentStartTime;
			Result.Errors = Info.GetErrorTotal();
			Result.Warnings = Info.GetWarningTotal();

			for (const FAutomationExecutionEntry& Entry : Info.GetEntries())
			{
				FMCPTestEvent Event;
				Event.Type = Entry.Event.Type;
				Event.Message = Entry.Event.Message;
				Event.Context = Entry.Event.Context;
				Event.File = Entry.Filename;
				Event.Line = Entry.LineNumber;
				Result.Events.Add(MoveTemp(Event));
			}
			if (!ExtraError.IsEmpty())
			{
				FMCPTestEvent Event;
				Event.Type = EAutomationEventType::Error;
				Event.Message = ExtraError;
				Result.Events.Add(MoveTemp(Event));
				++Result.Errors;
				bSuccess = false;
			}
			Result.bSuccess = bSuccess && Result.Errors == 0;
			Results.Add(MoveTemp(Result));

			bCurrentStarted = false;
			++QueueIndex;
		}

		void EndBatch()
		{
			ElapsedAtEnd = FPlatformTime::Seconds() - BatchStartTime;
			bRunning = false;
		}

		TArray<FAutomationTestInfo> Queue;
		TArray<FMCPTestResult> Results;
		int32 QueueIndex = 0;
		bool bCurrentStarted = false;
		bool bRunning = false;
		double TimeoutSeconds = 300.0;
		double BatchStartTime = 0.0;
		double CurrentStartTime = 0.0;
		double ElapsedAtEnd = 0.0;
		int64 LogCursor = 0;
		MCPCompat::FTickerHandle TickerHandle;
	};

	/** Load test modules and fetch every test that can run in this editor. */
	void Automation_GetAllTests(TArray<FAutomationTestInfo>& OutTests)
	{
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
		Framework.LoadTestModules();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		Framework.SetRequestedTestFilter(EAutomationTestFlags_FilterMask); // the enum member became a constant in 5.5
#else
		Framework.SetRequestedTestFilter(EAutomationTestFlags::FilterMask);
#endif
		Framework.GetValidTestNames(OutTests);
	}

	const TCHAR* Automation_EventTypeName(EAutomationEventType Type)
	{
		switch (Type)
		{
		case EAutomationEventType::Error: return TEXT("error");
		case EAutomationEventType::Warning: return TEXT("warning");
		default: return TEXT("info");
		}
	}
}

// ============================================================================
// FListAutomationTestsAction
// ============================================================================

TSharedPtr<FJsonObject> FListAutomationTestsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	TArray<FAutomationTestInfo> All;
	Automation_GetAllTests(All);

	const FString Filter = GetOptionalString(Params, TEXT("filter"));
	const int32 Limit = FMath::Clamp(static_cast<int32>(GetOptionalNumber(Params, TEXT("limit"), 100.0)), 1, 1000);
	const int32 Offset = FMath::Max(0, static_cast<int32>(GetOptionalNumber(Params, TEXT("offset"), 0.0)));

	TArray<const FAutomationTestInfo*> Matches;
	TMap<FString, int32> Categories;
	for (const FAutomationTestInfo& Test : All)
	{
		if (!Filter.IsEmpty() && !Test.GetFullTestPath().Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Matches.Add(&Test);

		TArray<FString> Segments;
		Test.GetFullTestPath().ParseIntoArray(Segments, TEXT("."));
		const FString Category = Segments.Num() >= 2 ? Segments[0] + TEXT(".") + Segments[1] : (Segments.Num() == 1 ? Segments[0] : FString());
		Categories.FindOrAdd(Category)++;
	}
	Matches.Sort([](const FAutomationTestInfo& A, const FAutomationTestInfo& B) { return A.GetFullTestPath() < B.GetFullTestPath(); });

	TArray<TSharedPtr<FJsonValue>> TestsJson;
	for (int32 Index = Offset; Index < Matches.Num() && TestsJson.Num() < Limit; ++Index)
	{
		const FAutomationTestInfo& Test = *Matches[Index];
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Test.GetTestName());
		Entry->SetStringField(TEXT("path"), Test.GetFullTestPath());
		Entry->SetStringField(TEXT("display_name"), Test.GetDisplayName());
		if (!Test.GetAssetPath().IsEmpty())
		{
			Entry->SetStringField(TEXT("asset_path"), Test.GetAssetPath());
		}
		TestsJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Largest categories first, capped, to make discovery cheap on projects with thousands of tests.
	TArray<TPair<FString, int32>> CategoryList;
	for (const TPair<FString, int32>& Pair : Categories)
	{
		CategoryList.Add(Pair);
	}
	CategoryList.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
	TSharedPtr<FJsonObject> CategoriesJson = MakeShared<FJsonObject>();
	for (int32 Index = 0; Index < CategoryList.Num() && Index < 40; ++Index)
	{
		CategoriesJson->SetNumberField(CategoryList[Index].Key, CategoryList[Index].Value);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total"), Matches.Num());
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetBoolField(TEXT("truncated"), Offset + TestsJson.Num() < Matches.Num());
	Result->SetObjectField(TEXT("categories"), CategoriesJson);
	Result->SetArrayField(TEXT("tests"), TestsJson);
	return CreateSuccessResponse(Result);
}

// ============================================================================
// FRunAutomationTestsAction
// ============================================================================

bool FRunAutomationTestsAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (GetOptionalBool(Params, TEXT("cancel"), false))
	{
		return true;
	}
	if (FMCPAutomationRunner::Get().IsRunning())
	{
		OutError = TEXT("A test batch is already running. Poll get_automation_results, or pass cancel=true to stop it.");
		return false;
	}
	if (FMCPPIE::IsActiveOrPending())
	{
		OutError = TEXT("PIE is running or starting. Call stop_pie first; automation tests manage their own PIE sessions.");
		return false;
	}
	if (GIsAutomationTesting || GIsSlowTask)
	{
		OutError = TEXT("The editor is busy (another automation run or a slow task is active). Try again shortly.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Tests = GetOptionalArray(Params, TEXT("tests"));
	if ((!Tests || Tests->Num() == 0) && GetOptionalString(Params, TEXT("filter")).IsEmpty())
	{
		OutError = TEXT("Specify tests (array of names/paths) or filter. Use list_automation_tests to discover them.");
		return false;
	}
	if (GetOptionalNumber(Params, TEXT("timeout_seconds"), 300.0) <= 0.0)
	{
		OutError = TEXT("timeout_seconds must be positive");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FRunAutomationTestsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FMCPAutomationRunner& Runner = FMCPAutomationRunner::Get();

	if (GetOptionalBool(Params, TEXT("cancel"), false))
	{
		const bool bWasRunning = Runner.IsRunning();
		Runner.Cancel();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("state"), TEXT("idle"));
		Result->SetBoolField(TEXT("was_running"), bWasRunning);
		return CreateSuccessResponse(Result);
	}

	TArray<FAutomationTestInfo> All;
	Automation_GetAllTests(All);

	TArray<FAutomationTestInfo> Selected;
	TSet<FString> SelectedNames;
	auto AddTest = [&Selected, &SelectedNames](const FAutomationTestInfo& Test)
	{
		if (!SelectedNames.Contains(Test.GetTestName()))
		{
			SelectedNames.Add(Test.GetTestName());
			Selected.Add(Test);
		}
	};

	TArray<FString> Unknown;
	if (const TArray<TSharedPtr<FJsonValue>>* Tests = GetOptionalArray(Params, TEXT("tests")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tests)
		{
			const FString Wanted = Value->AsString();
			bool bFound = false;
			for (const FAutomationTestInfo& Test : All)
			{
				if (Test.GetTestName().Equals(Wanted, ESearchCase::IgnoreCase) || Test.GetFullTestPath().Equals(Wanted, ESearchCase::IgnoreCase))
				{
					AddTest(Test);
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				Unknown.Add(Wanted);
			}
		}
	}
	if (Unknown.Num() > 0)
	{
		return CreateErrorResponse(FString::Printf(TEXT("Unknown test(s): %s. Use the exact name or path from list_automation_tests."), *FString::Join(Unknown, TEXT(", "))), TEXT("not_found"));
	}

	const FString Filter = GetOptionalString(Params, TEXT("filter"));
	if (!Filter.IsEmpty())
	{
		for (const FAutomationTestInfo& Test : All)
		{
			if (Test.GetFullTestPath().Contains(Filter, ESearchCase::IgnoreCase))
			{
				AddTest(Test);
			}
		}
	}

	const int32 MaxTests = FMath::Max(1, static_cast<int32>(GetOptionalNumber(Params, TEXT("max_tests"), 100.0)));
	if (Selected.Num() == 0)
	{
		return CreateErrorResponse(TEXT("No automation tests matched. Use list_automation_tests to see what is available."), TEXT("not_found"));
	}
	if (Selected.Num() > MaxTests)
	{
		return CreateErrorResponse(FString::Printf(TEXT("%d tests matched, more than max_tests (%d). Narrow the filter or raise max_tests."), Selected.Num(), MaxTests), TEXT("validation_failed"));
	}

	Selected.Sort([](const FAutomationTestInfo& A, const FAutomationTestInfo& B) { return A.GetFullTestPath() < B.GetFullTestPath(); });

	Context.FlushPendingSave();
	Runner.Start(Selected, GetOptionalNumber(Params, TEXT("timeout_seconds"), 300.0));

	TArray<TSharedPtr<FJsonValue>> Names;
	for (const FAutomationTestInfo& Test : Selected)
	{
		Names.Add(MakeShared<FJsonValueString>(Test.GetFullTestPath()));
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("state"), TEXT("running"));
	Result->SetNumberField(TEXT("total"), Selected.Num());
	Result->SetArrayField(TEXT("tests"), Names);
	Result->SetNumberField(TEXT("log_cursor"), static_cast<double>(Runner.GetLogCursor()));
	Result->SetStringField(TEXT("message"), TEXT("Tests started. Poll get_automation_results until state is 'idle'."));
	return CreateSuccessResponse(Result);
}

// ============================================================================
// FGetAutomationResultsAction
// ============================================================================

TSharedPtr<FJsonObject> FGetAutomationResultsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FMCPAutomationRunner& Runner = FMCPAutomationRunner::Get();
	const bool bIncludeInfo = GetOptionalBool(Params, TEXT("include_info"), false);
	constexpr int32 MaxEventsPerTest = 50;

	int32 Passed = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	for (const FMCPTestResult& Test : Runner.GetResults())
	{
		(Test.bSuccess ? Passed : Failed)++;

		TArray<TSharedPtr<FJsonValue>> Events;
		for (const FMCPTestEvent& Event : Test.Events)
		{
			if (Event.Type == EAutomationEventType::Info && !bIncludeInfo)
			{
				continue;
			}
			if (Events.Num() >= MaxEventsPerTest)
			{
				break;
			}
			TSharedPtr<FJsonObject> EventJson = MakeShared<FJsonObject>();
			EventJson->SetStringField(TEXT("type"), Automation_EventTypeName(Event.Type));
			EventJson->SetStringField(TEXT("message"), Event.Message);
			if (!Event.Context.IsEmpty())
			{
				EventJson->SetStringField(TEXT("context"), Event.Context);
			}
			if (!Event.File.IsEmpty())
			{
				EventJson->SetStringField(TEXT("file"), Event.File);
				EventJson->SetNumberField(TEXT("line"), Event.Line);
			}
			Events.Add(MakeShared<FJsonValueObject>(EventJson));
		}

		TSharedPtr<FJsonObject> TestJson = MakeShared<FJsonObject>();
		TestJson->SetStringField(TEXT("name"), Test.Name);
		TestJson->SetStringField(TEXT("path"), Test.Path);
		TestJson->SetBoolField(TEXT("success"), Test.bSuccess);
		TestJson->SetBoolField(TEXT("timed_out"), Test.bTimedOut);
		TestJson->SetNumberField(TEXT("duration"), Test.Duration);
		TestJson->SetNumberField(TEXT("errors"), Test.Errors);
		TestJson->SetNumberField(TEXT("warnings"), Test.Warnings);
		TestJson->SetArrayField(TEXT("events"), Events);
		ResultsJson.Add(MakeShared<FJsonValueObject>(TestJson));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("state"), Runner.IsRunning() ? TEXT("running") : TEXT("idle"));
	Result->SetNumberField(TEXT("total"), Runner.GetTotal());
	Result->SetNumberField(TEXT("completed"), Runner.GetResults().Num());
	Result->SetNumberField(TEXT("passed"), Passed);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetNumberField(TEXT("elapsed_seconds"), Runner.GetElapsed());
	Result->SetNumberField(TEXT("log_cursor"), static_cast<double>(Runner.GetLogCursor()));
	if (Runner.IsRunning())
	{
		Result->SetStringField(TEXT("current"), Runner.GetCurrentName());
	}
	Result->SetArrayField(TEXT("results"), ResultsJson);
	return CreateSuccessResponse(Result);
}
