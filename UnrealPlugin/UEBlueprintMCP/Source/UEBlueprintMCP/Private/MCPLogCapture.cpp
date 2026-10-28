// Copyright (c) 2025 zolnoor. All rights reserved.

#include "MCPLogCapture.h"
#include "Algo/Reverse.h"
#include "Misc/ScopeLock.h"

FMCPLogCapture& FMCPLogCapture::Get()
{
	// Intentionally leaked: GLog may already be gone during static destruction.
	static FMCPLogCapture* Instance = new FMCPLogCapture();
	return *Instance;
}

void FMCPLogCapture::Start()
{
	if (!bStarted && GLog)
	{
		GLog->AddOutputDevice(this);
		bStarted = true;
	}
}

void FMCPLogCapture::Stop()
{
	if (bStarted && GLog)
	{
		GLog->RemoveOutputDevice(this);
	}
	bStarted = false;
}

void FMCPLogCapture::Clear()
{
	FScopeLock ScopeLock(&Lock);
	Entries.Reset();
}

int64 FMCPLogCapture::GetLatestId() const
{
	FScopeLock ScopeLock(&Lock);
	return NextId - 1;
}

void FMCPLogCapture::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// NOTE: never log from inside this function (would recurse).
	FMCPLogEntry Entry;
	Entry.TimeUtc = FDateTime::UtcNow();
	Entry.Verbosity = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	Entry.Category = Category;
	Entry.Message = Message;
	Entry.bFromMCP = Entry.Message.StartsWith(TEXT("UEBlueprintMCP:"));

	FScopeLock ScopeLock(&Lock);
	Entry.Id = NextId++;
	Entries.Add(MoveTemp(Entry));

	// Trim in chunks so the removal cost is amortised.
	if (Entries.Num() > MaxEntries + 500)
	{
		Entries.RemoveAt(0, Entries.Num() - MaxEntries);
	}
}

void FMCPLogCapture::Query(const FMCPLogQuery& Query, TArray<FMCPLogEntry>& OutEntries, int64& OutNextId, bool& bOutTruncated, bool& bOutGap) const
{
	OutEntries.Reset();
	bOutTruncated = false;

	FScopeLock ScopeLock(&Lock);

	const int64 LatestId = NextId - 1;
	bOutGap = Query.SinceId >= 0 && Entries.Num() > 0 && Entries[0].Id > Query.SinceId + 1;

	const int32 Limit = FMath::Clamp(Query.Limit, 1, 1000);

	auto Matches = [&Query](const FMCPLogEntry& Entry) -> bool
	{
		if (Entry.bFromMCP && !Query.bIncludeMCP)
		{
			return false;
		}
		if (Entry.Verbosity > Query.MinSeverity)
		{
			return false;
		}
		if (Query.Categories.Num() > 0)
		{
			const FString CategoryName = Entry.Category.ToString();
			bool bCategoryMatch = false;
			for (const FString& Wanted : Query.Categories)
			{
				if (CategoryName.Equals(Wanted, ESearchCase::IgnoreCase))
				{
					bCategoryMatch = true;
					break;
				}
			}
			if (!bCategoryMatch)
			{
				return false;
			}
		}
		if (!Query.Contains.IsEmpty() && !Entry.Message.Contains(Query.Contains, ESearchCase::IgnoreCase))
		{
			return false;
		}
		return true;
	};

	if (Query.SinceId >= 0)
	{
		OutNextId = Query.SinceId;
		for (const FMCPLogEntry& Entry : Entries)
		{
			if (Entry.Id <= Query.SinceId || !Matches(Entry))
			{
				continue;
			}
			if (OutEntries.Num() >= Limit)
			{
				bOutTruncated = true;
				break;
			}
			OutEntries.Add(Entry);
			OutNextId = Entry.Id;
		}
		if (!bOutTruncated)
		{
			OutNextId = FMath::Max(OutNextId, LatestId);
		}
	}
	else
	{
		for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
		{
			if (!Matches(Entries[Index]))
			{
				continue;
			}
			if (OutEntries.Num() >= Limit)
			{
				bOutTruncated = true;
				break;
			}
			OutEntries.Add(Entries[Index]);
		}
		Algo::Reverse(OutEntries);
		OutNextId = LatestId;
	}
}
