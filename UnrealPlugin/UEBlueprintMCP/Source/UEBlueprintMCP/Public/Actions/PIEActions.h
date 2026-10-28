// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FMCPPIE
 *
 * Play-In-Editor session state shared by the PIE actions and the action pipeline.
 * PIE starts asynchronously (on the next editor tick), so "pending" covers the gap
 * between a start request and GEditor->PlayWorld becoming valid.
 */
class UEBLUEPRINTMCP_API FMCPPIE
{
public:
	/** A PIE / Simulate world is currently running. */
	static bool IsRunning();

	/** A start was requested recently and the session has not begun yet. */
	static bool IsStartPending();

	static bool IsActiveOrPending() { return IsRunning() || IsStartPending(); }

	static void MarkStartRequested();
	static void ClearStartRequest();

	/** True once if a start request timed out without PIE ever starting. */
	static bool ConsumeExpiredStartRequest();

private:
	static double StartRequestTime;
};

/**
 * FStartPIEAction
 *
 * Request a Play-In-Editor session. Returns immediately; poll get_pie_status until state is "running".
 *
 * Parameters:
 *   - simulate (optional, default false): Simulate In Editor instead of Play In Editor
 *   - clear_log (optional, default false): Clear the captured Output Log before starting
 */
class UEBLUEPRINTMCP_API FStartPIEAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("start_pie"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FStopPIEAction
 *
 * Request the running PIE session to end. Poll get_pie_status until state is "stopped".
 */
class UEBLUEPRINTMCP_API FStopPIEAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("stop_pie"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FGetPIEStatusAction
 *
 * Report PIE state: "stopped", "starting" or "running" (plus world name, net mode,
 * game time, paused flag and actor count while running) and the current log cursor.
 */
class UEBLUEPRINTMCP_API FGetPIEStatusAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override { return true; }
	virtual FString GetActionName() const override { return TEXT("get_pie_status"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FGetOutputLogAction
 *
 * Read captured Output Log lines.
 *
 * Parameters:
 *   - since_id (optional): Return entries newer than this id (oldest first). Omit for the most recent entries.
 *   - limit (optional, default 200, max 1000)
 *   - min_verbosity (optional, default "Log"): Error | Warning | Display | Log | Verbose | VeryVerbose
 *   - categories (optional): Array of log category names (e.g. ["LogBlueprintUserMessages"])
 *   - contains (optional): Case-insensitive substring of the message
 *   - include_mcp (optional, default false): Include this plugin's own "UEBlueprintMCP:" lines
 *   - clear (optional, default false): Clear the buffer after reading
 *
 * Returns entries (id, time, verbosity, category, message), next_id (pass as since_id next call),
 * truncated and dropped flags.
 */
class UEBLUEPRINTMCP_API FGetOutputLogAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("get_output_log"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FExecuteConsoleCommandAction
 *
 * Run a console command in the PIE world (when running) or the editor world.
 *
 * Parameters:
 *   - command (required): Console command (e.g. "stat fps", "ke * MyCustomEvent")
 *   - world (optional, default "auto"): "auto" (PIE if running, else editor) | "pie" | "editor"
 *
 * Returns handled and any text written to the command's output; many commands log to the
 * Output Log instead, so follow up with get_output_log. quit/exit/crash/py are rejected.
 */
class UEBLUEPRINTMCP_API FExecuteConsoleCommandAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("execute_console_command"); }
	virtual bool RequiresSave() const override { return false; }
};
