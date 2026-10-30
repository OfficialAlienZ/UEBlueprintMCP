// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FListAutomationTestsAction
 *
 * List the automation tests available in this editor: engine/editor tests, "Project.Functional Tests.*"
 * (AFunctionalTest actors placed in maps - Blueprint-friendly), "Project.Maps.*" and anything the project adds.
 * Same list as Window > Test Automation.
 *
 * Parameters:
 *   - filter (optional): case-insensitive substring of the test path (e.g. "Functional Tests", "Project.")
 *   - limit (optional, default 100, max 1000) / offset (optional)
 *
 * Returns total, categories (first two path segments with counts) and tests [{name, path, display_name,
 * asset_path}]. Pass `name` (or `path`) to run_automation_tests.
 */
class UEBLUEPRINTMCP_API FListAutomationTestsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override { return true; }
	virtual FString GetActionName() const override { return TEXT("list_automation_tests"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FRunAutomationTestsAction
 *
 * Start automation tests. Asynchronous: the tests run one after another on the editor's tick (the same way the
 * Test Automation window runs them); poll get_automation_results. Functional tests start and stop PIE themselves,
 * so PIE must not be running when this is called.
 *
 * Parameters:
 *   - tests (optional): Array of exact test names or paths from list_automation_tests
 *   - filter (optional): case-insensitive substring of the test path; runs every match
 *   - max_tests (optional, default 100): refuse to start if filter matches more than this
 *   - timeout_seconds (optional, default 300): per-test limit (real time); a test that exceeds it fails
 *   - cancel (optional): stop the running batch (no other parameters needed)
 */
class UEBLUEPRINTMCP_API FRunAutomationTestsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("run_automation_tests"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FGetAutomationResultsAction
 *
 * Progress and results of the last run_automation_tests batch.
 *
 * Parameters:
 *   - include_info (optional, default false): include Info events too (errors and warnings are always included)
 *
 * Returns state ("idle" | "running"), total, completed, passed, failed, current (running test),
 * elapsed_seconds, log_cursor (Output Log id at batch start, for get_output_log since_id) and results
 * [{name, path, success, timed_out, duration, errors, warnings, events: [{type, message, context, file, line}]}].
 */
class UEBLUEPRINTMCP_API FGetAutomationResultsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override { return true; }
	virtual FString GetActionName() const override { return TEXT("get_automation_results"); }
	virtual bool RequiresSave() const override { return false; }
};
