// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/GameplayActions.h"
#include "Actions/PIEActions.h"
#include "MCPCommonUtils.h"
#include "MCPCompat.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UnrealType.h"

// ============================================================================
// call_actor_function helpers
// ============================================================================

namespace
{
	/** Lower-case, without spaces / underscores: lets "Max Health", "max_health" and "MaxHealth" match. */
	FString Gameplay_NormalizeName(const FString& Name)
	{
		FString Result = Name.ToLower();
		Result.ReplaceInline(TEXT(" "), TEXT(""));
		Result.ReplaceInline(TEXT("_"), TEXT(""));
		return Result;
	}

	bool Gameplay_IsInputParam(const FProperty* Property)
	{
		return !Property->HasAnyPropertyFlags(CPF_ReturnParm)
			&& (!Property->HasAnyPropertyFlags(CPF_OutParm) || Property->HasAnyPropertyFlags(CPF_ReferenceParm));
	}

	bool Gameplay_IsOutputParam(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_ReturnParm)
			|| (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ReferenceParm));
	}

	/** Zero-initialised, properly constructed / destroyed parameter block for UObject::ProcessEvent. */
	struct FFunctionCallFrame
	{
		UFunction* Function;
		uint8* Memory;

		explicit FFunctionCallFrame(UFunction* InFunction)
			: Function(InFunction)
			, Memory(nullptr)
		{
			const int32 Size = FMath::Max<int32>(Function->ParmsSize, 1);
			Memory = static_cast<uint8*>(FMemory::Malloc(Size, FMath::Max<int32>(Function->GetMinAlignment(), 16)));
			FMemory::Memzero(Memory, Size);
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->InitializeValue_InContainer(Memory);
			}
		}

		~FFunctionCallFrame()
		{
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(Memory);
			}
			FMemory::Free(Memory);
		}

		FFunctionCallFrame(const FFunctionCallFrame&) = delete;
		FFunctionCallFrame& operator=(const FFunctionCallFrame&) = delete;
	};

	/** Write a JSON value into a function parameter. Actors can be given by name / label / @alias. */
	bool Gameplay_AssignArgument(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, UWorld* World, FString& OutError)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return true;
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (Value->Type == EJson::String)
			{
				const FString Text = Value->AsString();
				if (Text.IsEmpty() || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
					return true;
				}
				if (AActor* Actor = FMCPCommonUtils::FindActor(World, Text))
				{
					if (!Actor->IsA(ObjectProperty->PropertyClass))
					{
						OutError = FString::Printf(TEXT("Actor '%s' is a %s, but parameter '%s' needs a %s"),
							*Text, *Actor->GetClass()->GetName(), *Property->GetName(), *ObjectProperty->PropertyClass->GetName());
						return false;
					}
					ObjectProperty->SetObjectPropertyValue(ValuePtr, Actor);
					return true;
				}
			}
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				UScriptStruct* Struct = StructProperty->Struct;
				if (Struct == TBaseStructure<FVector>::Get() && Items.Num() == 3)
				{
					*static_cast<FVector*>(ValuePtr) = FVector(Items[0]->AsNumber(), Items[1]->AsNumber(), Items[2]->AsNumber());
					return true;
				}
				if (Struct == TBaseStructure<FRotator>::Get() && Items.Num() == 3)
				{
					*static_cast<FRotator*>(ValuePtr) = FRotator(Items[0]->AsNumber(), Items[1]->AsNumber(), Items[2]->AsNumber());
					return true;
				}
				if (Struct == TBaseStructure<FVector2D>::Get() && Items.Num() == 2)
				{
					*static_cast<FVector2D*>(ValuePtr) = FVector2D(Items[0]->AsNumber(), Items[1]->AsNumber());
					return true;
				}
			}
		}

		if (!FJsonObjectConverter::JsonValueToUProperty(Value, Property, ValuePtr, 0, 0))
		{
			OutError = FString::Printf(TEXT("Could not convert the value for parameter '%s' (expected %s)"),
				*Property->GetName(), *Property->GetCPPType());
			return false;
		}
		return true;
	}

	/** Read a function output parameter as JSON. Objects are summarised (name / class), not dumped. */
	TSharedPtr<FJsonValue> Gameplay_OutputToJson(FProperty* Property, const void* ValuePtr)
	{
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			if (!Object)
			{
				return MakeShared<FJsonValueNull>();
			}
			TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
			Summary->SetStringField(TEXT("name"), Object->GetName());
			Summary->SetStringField(TEXT("class"), Object->GetClass()->GetName());
			if (const AActor* Actor = Cast<AActor>(Object))
			{
				Summary->SetStringField(TEXT("label"), Actor->GetActorLabel());
			}
			return MakeShared<FJsonValueObject>(Summary);
		}

		TSharedPtr<FJsonValue> Json = FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr, 0, 0);
		if (!Json.IsValid())
		{
			Json = MakeShared<FJsonValueString>(FString::Printf(TEXT("<unsupported type %s>"), *Property->GetCPPType()));
		}
		return Json;
	}
}

// ============================================================================
// FCallActorFunctionAction
// ============================================================================

bool FCallActorFunctionAction::Resolve(const TSharedPtr<FJsonObject>& Params, UObject*& OutTarget, UFunction*& OutFunction, FString& OutError) const
{
	if (!FMCPPIE::IsRunning())
	{
		OutError = FMCPPIE::IsStartPending()
			? TEXT("PIE is still starting. Wait until get_pie_status reports 'running'.")
			: TEXT("No PIE session is running. Call start_pie first; functions can only be called on live runtime actors.");
		return false;
	}

	FString ActorName;
	FString FunctionName;
	if (!GetRequiredString(Params, TEXT("actor_name"), ActorName, OutError)
		|| !GetRequiredString(Params, TEXT("function_name"), FunctionName, OutError))
	{
		return false;
	}

	UWorld* World = GEditor->PlayWorld;
	AActor* Actor = FMCPCommonUtils::FindActor(World, ActorName);
	if (!Actor)
	{
		OutError = FString::Printf(TEXT("Actor '%s' not found in the PIE world. Use get_actors_in_level with world='pie' to list actors, or an alias such as @player_pawn."), *ActorName);
		return false;
	}

	UObject* Target = Actor;
	const FString ComponentName = GetOptionalString(Params, TEXT("component_name"));
	if (!ComponentName.IsEmpty())
	{
		Target = nullptr;
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				Target = Component;
				break;
			}
		}
		if (!Target)
		{
			OutError = FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *ComponentName, *Actor->GetName());
			return false;
		}
	}

	UFunction* Function = nullptr;
	for (TFieldIterator<UFunction> It(Target->GetClass()); It; ++It)
	{
		if (It->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			Function = *It;
			break;
		}
	}
	if (!Function)
	{
		// Help the caller: list the Blueprint-defined functions / events of the class.
		TArray<FString> Candidates;
		for (TFieldIterator<UFunction> It(Target->GetClass()); It && Candidates.Num() < 40; ++It)
		{
			const UClass* Owner = It->GetOwnerClass();
			if (Owner && !Owner->IsNative() && !It->GetName().StartsWith(TEXT("ExecuteUbergraph")))
			{
				Candidates.Add(It->GetName());
			}
		}
		OutError = FString::Printf(TEXT("Function '%s' not found on %s (%s)."), *FunctionName, *Target->GetName(), *Target->GetClass()->GetName());
		if (Candidates.Num() > 0)
		{
			OutError += TEXT(" Blueprint functions / events: ") + FString::Join(Candidates, TEXT(", "));
		}
		return false;
	}
	if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent))
	{
		OutError = FString::Printf(TEXT("Function '%s' is not Blueprint-callable"), *Function->GetName());
		return false;
	}

	OutTarget = Target;
	OutFunction = Function;
	return true;
}

bool FCallActorFunctionAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	const TSharedPtr<FJsonValue>* ArgsField = Params->Values.Find(TEXT("args"));
	if (ArgsField && ArgsField->IsValid() && !(*ArgsField)->IsNull() && (*ArgsField)->Type != EJson::Object)
	{
		OutError = TEXT("args must be an object mapping parameter name to value");
		return false;
	}
	UObject* Target = nullptr;
	UFunction* Function = nullptr;
	return Resolve(Params, Target, Function, OutError);
}

TSharedPtr<FJsonObject> FCallActorFunctionAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UObject* Target = nullptr;
	UFunction* Function = nullptr;
	FString Error;
	if (!Resolve(Params, Target, Function, Error))
	{
		return CreateErrorResponse(Error, TEXT("not_found"));
	}

	UWorld* World = GEditor->PlayWorld;
	FFunctionCallFrame Frame(Function);

	const TSharedPtr<FJsonObject>* Args = nullptr;
	if (Params->TryGetObjectField(TEXT("args"), Args) && Args && Args->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Args)->Values)
		{
			FProperty* Parameter = nullptr;
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				if (Gameplay_IsInputParam(*It) && Gameplay_NormalizeName(It->GetName()) == Gameplay_NormalizeName(Pair.Key))
				{
					Parameter = *It;
					break;
				}
			}
			if (!Parameter)
			{
				TArray<FString> InputNames;
				for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					if (Gameplay_IsInputParam(*It))
					{
						InputNames.Add(It->GetName());
					}
				}
				return CreateErrorResponse(
					FString::Printf(TEXT("Function '%s' has no input parameter '%s'. Inputs: %s"),
						*Function->GetName(), *Pair.Key, InputNames.Num() > 0 ? *FString::Join(InputNames, TEXT(", ")) : TEXT("(none)")),
					TEXT("validation_failed"));
			}
			if (!Gameplay_AssignArgument(Parameter, Parameter->ContainerPtrToValuePtr<void>(Frame.Memory), Pair.Value, World, Error))
			{
				return CreateErrorResponse(Error, TEXT("validation_failed"));
			}
		}
	}

	Target->ProcessEvent(Function, Frame.Memory);

	TSharedPtr<FJsonObject> Outputs = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (Gameplay_IsOutputParam(*It))
		{
			const FString OutputName = It->HasAnyPropertyFlags(CPF_ReturnParm) ? FString(TEXT("return_value")) : It->GetName();
			Outputs->SetField(OutputName, Gameplay_OutputToJson(*It, It->ContainerPtrToValuePtr<void>(Frame.Memory)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("function"), Function->GetName());
	Result->SetStringField(TEXT("target"), Target->GetName());
	Result->SetStringField(TEXT("target_class"), Target->GetClass()->GetName());
	Result->SetObjectField(TEXT("outputs"), Outputs);
	Result->SetNumberField(TEXT("game_time"), World->GetTimeSeconds());
	return CreateSuccessResponse(Result);
}

// ============================================================================
// inject_input helpers
// ============================================================================

namespace
{
	struct FHeldInput
	{
		MCPCompat::FTickerHandle Handle;
		uint32 Serial = 0;
	};

	/** Inputs kept alive by a ticker (timed holds, axis values), keyed by "<player>:<key>". */
	TMap<FName, FHeldInput> GHeldInputs;
	uint32 GNextHeldSerial = 1;

	FName Gameplay_HeldId(int32 PlayerIndex, const FKey& Key)
	{
		return FName(*FString::Printf(TEXT("%d:%s"), PlayerIndex, *Key.ToString()));
	}

	void Gameplay_StopHeld(const FName& Id)
	{
		if (const FHeldInput* Held = GHeldInputs.Find(Id))
		{
			MCPCompat::RemoveCoreTicker(Held->Handle);
			GHeldInputs.Remove(Id);
		}
	}

	void Gameplay_StopAllHeld()
	{
		for (const TPair<FName, FHeldInput>& Pair : GHeldInputs)
		{
			MCPCompat::RemoveCoreTicker(Pair.Value.Handle);
		}
		GHeldInputs.Empty();
	}

	/** Called by a ticker that ends on its own; does not remove a newer hold that replaced it. */
	void Gameplay_ForgetHeld(const FName& Id, uint32 Serial)
	{
		const FHeldInput* Held = GHeldInputs.Find(Id);
		if (Held && Held->Serial == Serial)
		{
			GHeldInputs.Remove(Id);
		}
	}

	void Gameplay_ReleaseKey(APlayerController* PlayerController, const FKey& Key)
	{
		const bool bGamepad = Key.IsGamepadKey();
		if (Key.IsAxis1D())
		{
			MCPCompat::InjectKeyAxis(PlayerController, Key, 0.0f, PlayerController->GetWorld()->GetDeltaSeconds(), bGamepad);
		}
		else
		{
			MCPCompat::InjectKeyEvent(PlayerController, Key, IE_Released, 0.0f, bGamepad);
		}
	}

	struct FInputRequest
	{
		FString Action;
		FKey Key;
		float Value = 1.0f;
		float Duration = 0.0f;
		int32 PlayerIndex = 0;
	};

	bool Gameplay_ParseInputRequest(const TSharedPtr<FJsonObject>& Params, FInputRequest& OutRequest, FString& OutError)
	{
		if (!Params->TryGetStringField(TEXT("action"), OutRequest.Action) || OutRequest.Action.IsEmpty())
		{
			OutRequest.Action = TEXT("tap");
		}
		OutRequest.Action = OutRequest.Action.ToLower();
		if (OutRequest.Action != TEXT("tap") && OutRequest.Action != TEXT("press")
			&& OutRequest.Action != TEXT("release") && OutRequest.Action != TEXT("release_all"))
		{
			OutError = FString::Printf(TEXT("Invalid action '%s'. Use tap, press, release or release_all."), *OutRequest.Action);
			return false;
		}

		double Number = 0.0;
		if (Params->TryGetNumberField(TEXT("value"), Number))
		{
			OutRequest.Value = static_cast<float>(Number);
		}
		if (Params->TryGetNumberField(TEXT("duration"), Number))
		{
			if (Number < 0.0)
			{
				OutError = TEXT("duration must not be negative");
				return false;
			}
			OutRequest.Duration = static_cast<float>(Number);
		}
		if (Params->TryGetNumberField(TEXT("player_index"), Number))
		{
			OutRequest.PlayerIndex = static_cast<int32>(Number);
		}

		if (OutRequest.Action == TEXT("release_all"))
		{
			return true;
		}

		FString KeyName;
		if (!Params->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
		{
			OutError = TEXT("key is required (e.g. \"W\", \"SpaceBar\", \"LeftMouseButton\", \"Gamepad_LeftX\", \"MouseX\")");
			return false;
		}
		OutRequest.Key = FKey(*KeyName);
		if (!OutRequest.Key.IsValid())
		{
			OutError = FString::Printf(TEXT("Unknown key '%s'. Use Unreal key names such as W, SpaceBar, LeftShift, LeftMouseButton, MouseX, Gamepad_LeftX, Gamepad_FaceButton_Bottom."), *KeyName);
			return false;
		}
		if (OutRequest.Key.IsAnalog() && !OutRequest.Key.IsAxis1D())
		{
			OutError = FString::Printf(TEXT("'%s' is a 2D/3D axis. Drive its single axes instead (e.g. MouseX and MouseY, Gamepad_LeftX and Gamepad_LeftY)."), *KeyName);
			return false;
		}
		return true;
	}
}

// ============================================================================
// FInjectInputAction
// ============================================================================

bool FInjectInputAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!FMCPPIE::IsRunning())
	{
		OutError = FMCPPIE::IsStartPending()
			? TEXT("PIE is still starting. Wait until get_pie_status reports 'running'.")
			: TEXT("No PIE session is running. Call start_pie first; input can only be injected into a running game.");
		return false;
	}

	FInputRequest Request;
	if (!Gameplay_ParseInputRequest(Params, Request, OutError))
	{
		return false;
	}
	if (!UGameplayStatics::GetPlayerController(GEditor->PlayWorld, Request.PlayerIndex))
	{
		OutError = FString::Printf(TEXT("No player controller at player_index %d. The game may still be loading; check get_pie_status."), Request.PlayerIndex);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FInjectInputAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FInputRequest Request;
	FString Error;
	if (!Gameplay_ParseInputRequest(Params, Request, Error))
	{
		return CreateErrorResponse(Error, TEXT("validation_failed"));
	}

	UWorld* World = GEditor->PlayWorld;
	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(World, Request.PlayerIndex);
	if (!PlayerController)
	{
		return CreateErrorResponse(TEXT("The player controller is no longer available"), TEXT("not_found"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("action"), Request.Action);
	Result->SetStringField(TEXT("controller"), PlayerController->GetName());

	if (Request.Action == TEXT("release_all"))
	{
		Gameplay_StopAllHeld();
		PlayerController->FlushPressedKeys();
		Result->SetNumberField(TEXT("game_time"), World->GetTimeSeconds());
		return CreateSuccessResponse(Result);
	}

	const FKey Key = Request.Key;
	const bool bAxis = Key.IsAxis1D();
	const bool bGamepad = Key.IsGamepadKey();
	const FName HeldId = Gameplay_HeldId(Request.PlayerIndex, Key);
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetBoolField(TEXT("axis"), bAxis);

	Gameplay_StopHeld(HeldId);

	if (Request.Action == TEXT("release"))
	{
		Gameplay_ReleaseKey(PlayerController, Key);
		Result->SetNumberField(TEXT("game_time"), World->GetTimeSeconds());
		return CreateSuccessResponse(Result);
	}

	// tap / press
	const float HoldTime = Request.Action == TEXT("tap") ? (Request.Duration > 0.0f ? Request.Duration : 0.1f) : Request.Duration;
	if (!bAxis)
	{
		Result->SetBoolField(TEXT("handled"), MCPCompat::InjectKeyEvent(PlayerController, Key, IE_Pressed, Request.Value, bGamepad));
	}

	// Timed holds and axis values need a per-frame ticker (axis values only last one frame in the input stack).
	if (HoldTime > 0.0f || bAxis)
	{
		const uint32 Serial = GNextHeldSerial++;
		const double EndTime = HoldTime > 0.0f ? World->GetTimeSeconds() + HoldTime : -1.0;
		const TWeakObjectPtr<APlayerController> WeakController(PlayerController);
		const float Value = Request.Value;

		FHeldInput Held;
		Held.Serial = Serial;
		Held.Handle = MCPCompat::AddCoreTicker([WeakController, Key, HeldId, Serial, EndTime, Value, bAxis, bGamepad](float DeltaTime) -> bool
		{
			APlayerController* Controller = WeakController.Get();
			UWorld* PlayWorld = Controller ? Controller->GetWorld() : nullptr;
			if (!PlayWorld || !GEditor || GEditor->PlayWorld != PlayWorld)
			{
				Gameplay_ForgetHeld(HeldId, Serial);
				return false;
			}
			if (EndTime >= 0.0 && PlayWorld->GetTimeSeconds() >= EndTime)
			{
				Gameplay_ReleaseKey(Controller, Key);
				Gameplay_ForgetHeld(HeldId, Serial);
				return false;
			}
			if (bAxis)
			{
				MCPCompat::InjectKeyAxis(Controller, Key, Value, DeltaTime, bGamepad);
			}
			return true;
		}, 0.0f);
		GHeldInputs.Add(HeldId, Held);

		if (HoldTime > 0.0f)
		{
			Result->SetNumberField(TEXT("duration"), HoldTime);
		}
	}

	Result->SetNumberField(TEXT("game_time"), World->GetTimeSeconds());
	return CreateSuccessResponse(Result);
}
