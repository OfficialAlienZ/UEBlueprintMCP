// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Misc/DateTime.h"
#include "HAL/CriticalSection.h"

/** One captured Output Log line. */
struct FMCPLogEntry
{
	int64 Id = 0;
	FDateTime TimeUtc;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FName Category;
	FString Message;
	/** True for lines emitted by this plugin ("UEBlueprintMCP: ..."). */
	bool bFromMCP = false;
};

/** Filter for FMCPLogCapture::Query. */
struct FMCPLogQuery
{
	/** Return entries with Id > SinceId (oldest first). -1 returns the most recent entries instead. */
	int64 SinceId = -1;
	int32 Limit = 200;
	/** Include entries at this severity or worse (Error < Warning < Display < Log < Verbose). */
	ELogVerbosity::Type MinSeverity = ELogVerbosity::Log;
	/** Exact category names (case-insensitive). Empty = all categories. */
	TArray<FString> Categories;
	/** Case-insensitive substring of the message. Empty = no text filter. */
	FString Contains;
	/** Include the plugin's own "UEBlueprintMCP:" lines. */
	bool bIncludeMCP = false;
};

/**
 * FMCPLogCapture
 *
 * Output device that keeps the most recent Output Log lines in a thread-safe ring buffer so
 * MCP clients can read them (get_output_log). Registered by UMCPBridge for the editor session.
 */
class UEBLUEPRINTMCP_API FMCPLogCapture : public FOutputDevice
{
public:
	static FMCPLogCapture& Get();

	void Start();
	void Stop();
	void Clear();

	/** Id of the most recently captured entry (0 if none). */
	int64 GetLatestId() const;

	/**
	 * Run a query.
	 * @param OutNextId  Cursor to pass as SinceId on the next call
	 * @param bOutTruncated  More matching entries exist than Limit allowed
	 * @param bOutGap  SinceId was older than the oldest buffered entry (lines were dropped)
	 */
	void Query(const FMCPLogQuery& Query, TArray<FMCPLogEntry>& OutEntries, int64& OutNextId, bool& bOutTruncated, bool& bOutGap) const;

	// FOutputDevice
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }

private:
	static constexpr int32 MaxEntries = 5000;

	mutable FCriticalSection Lock;
	TArray<FMCPLogEntry> Entries;
	int64 NextId = 1;
	bool bStarted = false;
};
