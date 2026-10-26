// Copyright (c) 2025 zolnoor. All rights reserved.

#include "MCPServer.h"
#include "MCPBridge.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Misc/ScopeLock.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

FMCPServer::FMCPServer(UMCPBridge* InBridge, int32 InPort)
	: Bridge(InBridge)
	, ListenerSocket(nullptr)
	, Port(InPort)
	, Thread(nullptr)
	, bShouldStop(false)
	, bIsRunning(false)
{
}

FMCPServer::~FMCPServer()
{
	Stop();
}

bool FMCPServer::Start()
{
	if (bIsRunning)
	{
		return true;
	}

	// Create the listener socket
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("UEBlueprintMCP: Failed to get socket subsystem"));
		return false;
	}

	ListenerSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UEBlueprintMCP Listener"), false);
	if (!ListenerSocket)
	{
		UE_LOG(LogTemp, Error, TEXT("UEBlueprintMCP: Failed to create listener socket"));
		return false;
	}

	// Bind to localhost only (127.0.0.1) - NOT exposed to network
	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bIsValid = false;
	Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
	Addr->SetPort(Port);

	if (!ListenerSocket->Bind(*Addr))
	{
		UE_LOG(LogTemp, Error, TEXT("UEBlueprintMCP: Failed to bind to port %d"), Port);
		SocketSubsystem->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
		return false;
	}

	// Start listening
	if (!ListenerSocket->Listen(8))
	{
		UE_LOG(LogTemp, Error, TEXT("UEBlueprintMCP: Failed to listen on socket"));
		SocketSubsystem->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
		return false;
	}

	// Start the worker thread
	bShouldStop = false;
	Thread = FRunnableThread::Create(this, TEXT("UEBlueprintMCP Server Thread"));
	if (!Thread)
	{
		UE_LOG(LogTemp, Error, TEXT("UEBlueprintMCP: Failed to create server thread"));
		SocketSubsystem->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
		return false;
	}

	return true;
}

void FMCPServer::Stop()
{
	// One-shot, re-entrancy-safe guard. Without this, Stop() can be entered
	// twice (Deinitialize + ~FMCPServer, or concurrently during engine
	// shutdown), causing a double-delete of Thread and a recursive
	// Stop()->WaitForCompletion() loop that crashes with STACK_OVERFLOW.
	if (bStopCalled.Exchange(true))
	{
		return;
	}

	bShouldStop = true;

	if (ListenerSocket)
	{
		ListenerSocket->Close();
	}

	if (Thread)
	{
		// Never wait on our own thread - would deadlock if Stop()
		// is ever called from the server worker thread itself.
		if (FPlatformTLS::GetCurrentThreadId() != Thread->GetThreadID())
		{
			Thread->WaitForCompletion();
		}
		delete Thread;
		Thread = nullptr;
	}

	if (ListenerSocket)
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (SocketSubsystem)
		{
			SocketSubsystem->DestroySocket(ListenerSocket);
		}
		ListenerSocket = nullptr;
	}

	bIsRunning = false;
}

bool FMCPServer::Init()
{
	return true;
}

uint32 FMCPServer::Run()
{
	bIsRunning = true;

	while (!bShouldStop)
	{
		// Wait for connection (with timeout so we can check bShouldStop)
		bool bPendingConnection = false;
		if (ListenerSocket->WaitForPendingConnection(bPendingConnection, FTimespan::FromSeconds(0.5)))
		{
			if (bPendingConnection)
			{
				// Accept the connection
				FSocket* ClientSocket = ListenerSocket->Accept(TEXT("UEBlueprintMCP Client"));
				if (ClientSocket)
				{
					UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Client connected"));
					HandleClient(ClientSocket);
					UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Client disconnected"));

					ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
					if (SocketSubsystem)
					{
						SocketSubsystem->DestroySocket(ClientSocket);
					}
				}
			}
		}
	}

	bIsRunning = false;
	return 0;
}

void FMCPServer::Exit()
{
	bIsRunning = false;
}

void FMCPServer::HandleClient(FSocket* ClientSocket)
{
	// Set socket options for low-latency communication
	ClientSocket->SetNonBlocking(false);
	ClientSocket->SetNoDelay(true);  // Disable Nagle's algorithm (critical for all platforms)

#if PLATFORM_MAC
	// Mac-specific: Larger socket buffers help with kqueue polling behavior
	// and reduce latency caused by macOS's default delayed_ack=3 setting.
	// These values are tuned for MCP's small JSON message pattern.
	int32 RecvBufSize = 65536;
	int32 SendBufSize = 65536;
	ClientSocket->SetReceiveBufferSize(RecvBufSize, RecvBufSize);
	ClientSocket->SetSendBufferSize(SendBufSize, SendBufSize);
#endif

	float LastActivityTime = FPlatformTime::Seconds();

	// Keep connection alive until client disconnects or timeout
	while (!bShouldStop)
	{
		// Check for timeout
		float CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastActivityTime > ConnectionTimeout)
		{
			UE_LOG(LogTemp, Warning, TEXT("UEBlueprintMCP: Client connection timed out"));
			break;
		}

		// Check if data is available FIRST (non-blocking check)
		uint32 PendingDataSize = 0;
		if (!ClientSocket->HasPendingData(PendingDataSize) || PendingDataSize == 0)
		{
			// No data available - sleep briefly and continue
			// On Mac, we must NOT do a blocking peek recv when there's no data,
			// as it can return errors that look like disconnections.
			FPlatformProcess::Sleep(0.01f);
			continue;
		}

		// Data is available - now safe to peek to verify connection health
		uint8 PeekByte;
		int32 PeekBytes = 0;
		if (!ClientSocket->Recv(&PeekByte, 1, PeekBytes, ESocketReceiveFlags::Peek))
		{
			// Socket error - likely disconnected
			UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Client disconnected (recv error)"));
			break;
		}
		if (PeekBytes == 0)
		{
			// Connection closed by peer (EOF)
			UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Client disconnected (EOF)"));
			break;
		}

		// Receive message
		FString Message;
		if (!ReceiveMessage(ClientSocket, Message))
		{
			UE_LOG(LogTemp, Warning, TEXT("UEBlueprintMCP: Failed to receive message"));
			break;
		}

		LastActivityTime = FPlatformTime::Seconds();

		// Parse JSON
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			FString ErrorResponse = TEXT("{\"status\":\"error\",\"error\":\"Invalid JSON\"}");
			SendResponse(ClientSocket, ErrorResponse);
			continue;
		}

		// Get command type
		FString CommandType;
		if (!JsonObj->TryGetStringField(TEXT("type"), CommandType))
		{
			FString ErrorResponse = TEXT("{\"status\":\"error\",\"error\":\"Missing 'type' field\"}");
			SendResponse(ClientSocket, ErrorResponse);
			continue;
		}

		// Handle special commands that don't need game thread
		if (CommandType == TEXT("ping"))
		{
			SendResponse(ClientSocket, HandlePing());
			continue;
		}

		if (CommandType == TEXT("close"))
		{
			HandleClose(ClientSocket);
			break;
		}

		if (CommandType == TEXT("get_context"))
		{
			SendResponse(ClientSocket, HandleGetContext());
			continue;
		}

		// Get params (optional)
		TSharedPtr<FJsonObject> Params;
		const TSharedPtr<FJsonObject>* ParamsPtr;
		if (JsonObj->TryGetObjectField(TEXT("params"), ParamsPtr))
		{
			Params = *ParamsPtr;
		}
		else
		{
			Params = MakeShared<FJsonObject>();
		}

		// Execute on game thread and get response
		FString Response = ExecuteOnGameThread(CommandType, Params);
		SendResponse(ClientSocket, Response);
	}
}

bool FMCPServer::ReceiveMessage(FSocket* ClientSocket, FString& OutMessage)
{
	// Receive length prefix (4 bytes, big endian)
	uint8 LengthBytes[4];
	int32 BytesRead = 0;
	if (!ClientSocket->Recv(LengthBytes, 4, BytesRead) || BytesRead != 4)
	{
		return false;
	}

	int32 Length = (LengthBytes[0] << 24) | (LengthBytes[1] << 16) | (LengthBytes[2] << 8) | LengthBytes[3];

	// Sanity check
	if (Length <= 0 || Length > RecvBufferSize)
	{
		UE_LOG(LogTemp, Warning, TEXT("UEBlueprintMCP: Invalid message length: %d"), Length);
		return false;
	}

	// Receive message
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(Length);

	int32 TotalReceived = 0;
	while (TotalReceived < Length)
	{
		int32 Received = 0;
		if (!ClientSocket->Recv(Buffer.GetData() + TotalReceived, Length - TotalReceived, Received) || Received <= 0)
		{
			return false;
		}
		TotalReceived += Received;
	}

	// Convert to string
	OutMessage = FString(Length, UTF8_TO_TCHAR(reinterpret_cast<const char*>(Buffer.GetData())));
	return true;
}

bool FMCPServer::SendResponse(FSocket* ClientSocket, const FString& Response)
{
	// Convert to UTF-8
	FTCHARToUTF8 Converter(*Response);
	int32 Length = Converter.Length();

	// Send length prefix (4 bytes, big endian)
	uint8 LengthBytes[4] = {
		static_cast<uint8>((Length >> 24) & 0xFF),
		static_cast<uint8>((Length >> 16) & 0xFF),
		static_cast<uint8>((Length >> 8) & 0xFF),
		static_cast<uint8>(Length & 0xFF)
	};

	int32 BytesSent = 0;
	if (!ClientSocket->Send(LengthBytes, 4, BytesSent) || BytesSent != 4)
	{
		return false;
	}

	// Send message
	int32 TotalSent = 0;
	while (TotalSent < Length)
	{
		int32 Sent = 0;
		if (!ClientSocket->Send(reinterpret_cast<const uint8*>(Converter.Get()) + TotalSent, Length - TotalSent, Sent) || Sent <= 0)
		{
			return false;
		}
		TotalSent += Sent;
	}

	return true;
}

FString FMCPServer::HandlePing()
{
	return TEXT("{\"status\":\"success\",\"result\":{\"pong\":true}}");
}

void FMCPServer::HandleClose(FSocket* ClientSocket)
{
	UE_LOG(LogTemp, Log, TEXT("UEBlueprintMCP: Client requested disconnect"));
	SendResponse(ClientSocket, TEXT("{\"status\":\"success\",\"result\":{\"closed\":true}}"));
}

FString FMCPServer::HandleGetContext()
{
	// Execute on game thread to safely access context
	return RunOnGameThread(TEXT("get_context"), ContextTimeoutSeconds, [](UMCPBridge* InBridge) -> FString
	{
		TSharedPtr<FJsonObject> ContextJson = InBridge->GetContext().ToJson();

		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("status"), TEXT("success"));
		Response->SetObjectField(TEXT("result"), ContextJson);

		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
		return ResponseStr;
	});
}

namespace
{
	/** Shared state between the server thread and a queued game-thread task. */
	struct FGameThreadCall
	{
		FCriticalSection Lock;
		int32 State = 0;  // 0 = queued, 1 = running, 2 = cancelled (timed out before it started)
		TPromise<FString> Promise;
	};

	/** Commands that legitimately take long (compile, save). Keep in sync with the Python client. */
	double GetCommandTimeoutSeconds(const FString& CommandType)
	{
		static const TSet<FString> LongRunning = {
			TEXT("compile_blueprint"),
			TEXT("compile_material"),
			TEXT("save_all")
		};
		return LongRunning.Contains(CommandType) ? 120.0 : 25.0;
	}
}

FString FMCPServer::RunOnGameThread(const FString& Label, double TimeoutSeconds, TFunction<FString(UMCPBridge*)> Work)
{
	TSharedRef<FGameThreadCall, ESPMode::ThreadSafe> Call = MakeShared<FGameThreadCall, ESPMode::ThreadSafe>();
	TFuture<FString> Future = Call->Promise.GetFuture();
	TWeakObjectPtr<UMCPBridge> WeakBridge(Bridge);

	// Everything the task touches is captured by value / shared, so it stays valid
	// even if this function has already returned because of a timeout.
	AsyncTask(ENamedThreads::GameThread, [Call, WeakBridge, Work]()
	{
		{
			FScopeLock Lock(&Call->Lock);
			if (Call->State == 2)
			{
				return;  // Caller already timed out; do not run a stale command.
			}
			Call->State = 1;
		}

		FString Result;
		if (UMCPBridge* LiveBridge = WeakBridge.Get())
		{
			Result = Work(LiveBridge);
		}
		else
		{
			Result = TEXT("{\"status\":\"error\",\"error\":\"Bridge not available\"}");
		}
		Call->Promise.SetValue(Result);
	});

	if (Future.WaitFor(FTimespan::FromSeconds(TimeoutSeconds)))
	{
		return Future.Get();
	}

	bool bStarted = false;
	{
		FScopeLock Lock(&Call->Lock);
		bStarted = (Call->State == 1);
		if (!bStarted)
		{
			Call->State = 2;
		}
	}

	// It may have finished in the instant between the wait expiring and taking the lock.
	if (bStarted && Future.IsReady())
	{
		return Future.Get();
	}

	FString Message;
	if (bStarted)
	{
		UE_LOG(LogTemp, Warning, TEXT("UEBlueprintMCP: Command '%s' still running after %.0fs, reporting timeout"), *Label, TimeoutSeconds);
		Message = FString::Printf(TEXT("Command '%s' did not finish within %.0fs. It is still running in the editor and may complete later. Check the editor state before retrying."), *Label, TimeoutSeconds);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UEBlueprintMCP: Command '%s' never started within %.0fs (game thread busy), cancelled"), *Label, TimeoutSeconds);
		Message = FString::Printf(TEXT("Command '%s' was not started within %.0fs because the editor was busy (modal dialog, compile, or asset loading). It was cancelled and not executed."), *Label, TimeoutSeconds);
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), false);
	Response->SetStringField(TEXT("error"), Message);
	Response->SetStringField(TEXT("error_type"), TEXT("timeout"));
	Response->SetBoolField(TEXT("command_started"), bStarted);

	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
	return ResponseStr;
}

FString FMCPServer::ExecuteOnGameThread(const FString& CommandType, TSharedPtr<FJsonObject> Params)
{
	return RunOnGameThread(CommandType, GetCommandTimeoutSeconds(CommandType), [CommandType, Params](UMCPBridge* InBridge) -> FString
	{
		// Execute with crash protection
		TSharedPtr<FJsonObject> Response = InBridge->ExecuteCommandSafe(CommandType, Params);

		// Serialize response
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
		return ResponseStr;
	});
}
