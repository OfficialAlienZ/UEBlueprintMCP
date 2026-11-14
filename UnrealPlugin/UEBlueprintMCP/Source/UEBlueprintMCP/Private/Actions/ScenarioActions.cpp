// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/ScenarioActions.h"
#include "Actions/PIEActions.h"
#include "MCPCompat.h"
#include "MCPLogCapture.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMathLibrary.h"
#include "UObject/UnrealType.h"

namespace
{
	const TCHAR* DefaultArenaMap = TEXT("/Game/ZZ_ClaudeTest/ClaudeTestLevel");

	/** Actors of the running scenario by name (spawn list, mobs, "spawner"); lets "@name" in args refer to them. */
	TMap<FString, TWeakObjectPtr<AActor>>* GNamed = nullptr;

	/** Authored name of a Blueprint struct member ("maxHP_3_<32 hex GUID>" -> "maxHP"). */
	FString Scn_FriendlyName(const FProperty* Property)
	{
		FString Name = Property->GetName();
		int32 Last = INDEX_NONE;
		if (Name.FindLastChar(TEXT('_'), Last) && Name.Len() - Last - 1 == 32)
		{
			FString Rest = Name.Left(Last);
			int32 Prev = INDEX_NONE;
			if (Rest.FindLastChar(TEXT('_'), Prev) && Rest.Mid(Prev + 1).IsNumeric())
			{
				return Rest.Left(Prev);
			}
		}
		return Name;
	}

	FProperty* Scn_FindProperty(const UStruct* Struct, const FString& Name)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (Scn_FriendlyName(*It).Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	UFunction* Scn_FindFunction(UObject* Object, const FString& Name)
	{
		for (TFieldIterator<UFunction> It(Object->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	UClass* Scn_LoadClass(const FString& Path)
	{
		if (UClass* Class = LoadClass<UObject>(nullptr, *Path))
		{
			return Class;
		}
		// "/Game/Dir/BP_X" -> "/Game/Dir/BP_X.BP_X_C"
		FString Short = FPaths::GetBaseFilename(Path);
		return LoadClass<UObject>(nullptr, *FString::Printf(TEXT("%s.%s_C"), *Path, *Short));
	}

	FString Scn_JsonToText(const TSharedPtr<FJsonValue>& Value)
	{
		switch (Value->Type)
		{
		case EJson::Number: return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean: return Value->AsBool() ? TEXT("True") : TEXT("False");
		default: return Value->AsString();
		}
	}

	/** Write a JSON value into a property. JSON objects merge into structs member by member (Blueprint member names). */
	bool Scn_Assign(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, FString& Error)
	{
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (Value->Type == EJson::Object)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
				{
					FProperty* Member = Scn_FindProperty(StructProperty->Struct, Pair.Key);
					if (!Member)
					{
						Error = FString::Printf(TEXT("struct %s has no member '%s'"), *StructProperty->Struct->GetName(), *Pair.Key);
						return false;
					}
					if (!Scn_Assign(Member, Member->ContainerPtrToValuePtr<void>(ValuePtr), Pair.Value, Error))
					{
						return false;
					}
				}
				return true;
			}
			if (Value->Type == EJson::Number && StructProperty->Struct == TBaseStructure<FRandomStream>::Get())
			{
				*static_cast<FRandomStream*>(ValuePtr) = FRandomStream(static_cast<int32>(Value->AsNumber()));
				return true;
			}
			if (Value->Type == EJson::Array && Value->AsArray().Num() == 3)
			{
				const TArray<TSharedPtr<FJsonValue>>& V = Value->AsArray();
				const bool bRot = StructProperty->Struct == TBaseStructure<FRotator>::Get();
				const FString Text = bRot
					? FString::Printf(TEXT("(Pitch=%f,Yaw=%f,Roll=%f)"), V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber())
					: FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber());
				return Property->ImportText_Direct(*Text, ValuePtr, nullptr, PPF_None) != nullptr;
			}
		}
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (Value->Type == EJson::String && Value->AsString().StartsWith(TEXT("@")))
			{
				TWeakObjectPtr<AActor>* Found = GNamed ? GNamed->Find(Value->AsString().Mid(1)) : nullptr;
				if (!Found || !Found->IsValid())
				{
					Error = FString::Printf(TEXT("no scenario actor '%s'"), *Value->AsString());
					return false;
				}
				ObjectProperty->SetObjectPropertyValue(ValuePtr, Found->Get());
				return true;
			}
		}
		const FString Text = Scn_JsonToText(Value);
		if (!Property->ImportText_Direct(*Text, ValuePtr, nullptr, PPF_None))
		{
			Error = FString::Printf(TEXT("cannot set '%s' from '%s' (%s)"), *Property->GetName(), *Text, *Property->GetCPPType());
			return false;
		}
		return true;
	}

	/** Call a function by name with JSON args (parameter names as authored in the Blueprint). */
	bool Scn_CallFunction(UObject* Target, const FString& FunctionName, const TSharedPtr<FJsonObject>& Args, FString& Error)
	{
		UFunction* Function = Scn_FindFunction(Target, FunctionName);
		if (!Function)
		{
			Error = FString::Printf(TEXT("%s has no function '%s'"), *Target->GetName(), *FunctionName);
			return false;
		}
		uint8* Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max<int32>(Function->ParmsSize, 1), 16));
		FMemory::Memzero(Memory, FMath::Max<int32>(Function->ParmsSize, 1));
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Memory);
		}
		bool bOk = true;
		if (Args.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Args->Values)
			{
				FProperty* Parameter = Scn_FindProperty(Function, Pair.Key);
				if (!Parameter || !Parameter->HasAnyPropertyFlags(CPF_Parm))
				{
					Error = FString::Printf(TEXT("%s has no parameter '%s'"), *Function->GetName(), *Pair.Key);
					bOk = false;
					break;
				}
				if (!Scn_Assign(Parameter, Parameter->ContainerPtrToValuePtr<void>(Memory), Pair.Value, Error))
				{
					bOk = false;
					break;
				}
			}
		}
		if (bOk)
		{
			Target->ProcessEvent(Function, Memory);
		}
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Memory);
		}
		FMemory::Free(Memory);
		return bOk;
	}

	FString Scn_ReadProperty(UObject* Object, const FString& Name)
	{
		FProperty* Property = Scn_FindProperty(Object->GetClass(), Name);
		if (!Property)
		{
			return TEXT("?");
		}
		FString Text;
		Property->ExportText_Direct(Text, Property->ContainerPtrToValuePtr<void>(Object), nullptr, nullptr, PPF_None);
		return Text;
	}

	FString Scn_Num(double Value)
	{
		return FString::Printf(TEXT("%d"), FMath::RoundToInt(Value));
	}

	FString Scn_ClassForType(const FString& Type)
	{
		const FString Lower = Type.ToLower();
		if (Lower == TEXT("zombie")) return TEXT("/Game/TopDown/Enemies/Mobs/Zombie/ZombieBP.ZombieBP_C");
		if (Lower == TEXT("bat")) return TEXT("/Game/TopDown/Enemies/Mobs/Bat/BatBP.BatBP_C");
		if (Lower == TEXT("pigly")) return TEXT("/Game/TopDown/Enemies/Mobs/Pigly/PiglyBP.PiglyBP_C");
		if (Lower == TEXT("skullghost") || Lower == TEXT("boss")) return TEXT("/Game/TopDown/Enemies/Bosses/SkullGhost/SkullGhostBP.SkullGhostBP_C");
		return Type;
	}

	struct FWatched
	{
		FString Label;
		FString ClassName;
		TWeakObjectPtr<AActor> Actor;
		FVector Start = FVector::ZeroVector;
		bool bMob = false;
		TArray<FString> Props;
		TArray<double> Dist, Angle, Yaw;
		TMap<FString, TArray<FString>> PropRows;
		int32 AliveSamples = 0;
	};

	struct FTimedAction
	{
		double T = 0.0;
		FString Target;
		FString Call;
		TSharedPtr<FJsonObject> Args;
		TSharedPtr<FJsonObject> Set;
		TArray<FString> Read;
		bool bDone = false;
	};

	enum class EScenarioState { Idle, Stopping, Starting, Setup, Running, Done };

	/** Single scripted scenario, driven by the core ticker. */
	struct FScenario
	{
		EScenarioState State = EScenarioState::Idle;
		FTSTicker::FDelegateHandle Ticker;
		FString Report;
		FString Error;

		// Parameters
		FString Map;
		double Duration = 6.0;
		double Interval = 1.0;
		bool bKeepOpen = false;
		FString LogContains;
		TSharedPtr<FJsonObject> Params;
		TArray<FTimedAction> Actions;

		// Runtime
		double WallStart = 0.0;
		double T0 = 0.0;
		double NextSample = 0.0;
		bool bStartGameCalled = false;
		double SetupSince = 0.0;
		int64 LogCursor = 0;
		TArray<double> SampleTimes;
		TArray<FWatched> Watched;
		TArray<FString> ActionLines;
		FVector PlayerStart = FVector::ZeroVector;
		TArray<FString> PlayerProps;
		TMap<FString, TArray<FString>> PlayerRows;
		FString PlayerClass;
		TMap<FString, TWeakObjectPtr<AActor>> Named;
	};

	FScenario GScenario;

	UWorld* PlayWorld() { return GEditor ? static_cast<UWorld*>(GEditor->PlayWorld) : nullptr; }

	AActor* Scn_Resolve(const FString& Target, UWorld* World)
	{
		if (Target.Equals(TEXT("player"), ESearchCase::IgnoreCase))
		{
			APlayerController* PC = World->GetFirstPlayerController();
			return PC ? PC->GetPawn() : nullptr;
		}
		if (Target.Equals(TEXT("controller"), ESearchCase::IgnoreCase))
		{
			return World->GetFirstPlayerController();
		}
		if (TWeakObjectPtr<AActor>* Found = GScenario.Named.Find(Target))
		{
			return Found->Get();
		}
		for (const FWatched& W : GScenario.Watched)
		{
			if (W.Label.Equals(Target, ESearchCase::IgnoreCase))
			{
				return W.Actor.Get();
			}
		}
		return nullptr;
	}

	TArray<FString> Scn_StringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Items))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				Out.Add(Item->AsString());
			}
		}
		return Out;
	}

	bool Scn_ReadXY(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Items) && Items->Num() >= 2)
		{
			Out.X = (*Items)[0]->AsNumber();
			Out.Y = (*Items)[1]->AsNumber();
			if (Items->Num() >= 3)
			{
				Out.Z = (*Items)[2]->AsNumber();
			}
			return true;
		}
		return false;
	}

	void Scn_Finish(const FString& Error);

	/** Phase "Setup": start the game as the menu does, then spawn everything. Returns false while still waiting. */
	bool Scn_Setup(UWorld* World)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		if (!PC || !World->HasBegunPlay())
		{
			return false;
		}
		FScenario& S = GScenario;
		if (!S.bStartGameCalled)
		{
			FString Error;
			if (!Scn_CallFunction(PC, TEXT("StartGame"), nullptr, Error))
			{
				Scn_Finish(TEXT("StartGame on the controller failed: ") + Error);
				return false;
			}
			S.bStartGameCalled = true;
			S.SetupSince = FPlatformTime::Seconds();
			return false;
		}
		APawn* Pawn = PC->GetPawn();
		if (!Pawn)
		{
			if (FPlatformTime::Seconds() - S.SetupSince > 20.0)
			{
				Scn_Finish(TEXT("no pawn 20 s after StartGame (controller ") + PC->GetClass()->GetName() + TEXT(")"));
			}
			return false;
		}
		if (FPlatformTime::Seconds() - S.SetupSince < 1.0)
		{
			return false; // let the pawn settle on the floor
		}

		FString Error;
		const TSharedPtr<FJsonObject>* PlayerObj = nullptr;
		S.Params->TryGetObjectField(TEXT("player"), PlayerObj);
		FVector Loc = Pawn->GetActorLocation();
		if (PlayerObj && Scn_ReadXY(*PlayerObj, TEXT("location"), Loc))
		{
			Loc.Z = Pawn->GetActorLocation().Z;
			Pawn->SetActorLocation(Loc);
		}
		S.PlayerStart = Pawn->GetActorLocation();
		S.PlayerClass = Pawn->GetClass()->GetName();
		S.PlayerProps = PlayerObj ? Scn_StringArray(*PlayerObj, TEXT("props")) : TArray<FString>();

		// Extra actors
		const TArray<TSharedPtr<FJsonValue>>* SpawnList = nullptr;
		if (S.Params->TryGetArrayField(TEXT("spawn"), SpawnList))
		{
			for (const TSharedPtr<FJsonValue>& Item : *SpawnList)
			{
				const TSharedPtr<FJsonObject> Obj = Item->AsObject();
				UClass* Class = Obj.IsValid() ? Scn_LoadClass(Obj->GetStringField(TEXT("class"))) : nullptr;
				if (!Class)
				{
					Scn_Finish(TEXT("spawn: class not found: ") + (Obj.IsValid() ? Obj->GetStringField(TEXT("class")) : FString()));
					return false;
				}
				FVector P = FVector::ZeroVector;
				Scn_ReadXY(Obj, TEXT("location"), P);
				FVector R = FVector::ZeroVector;
				Scn_ReadXY(Obj, TEXT("rotation"), R);
				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AActor* Actor = World->SpawnActor<AActor>(Class, P, FRotator(R.X, R.Y, R.Z), SpawnParams);
				const FString Name = Obj->HasField(TEXT("name")) ? Obj->GetStringField(TEXT("name")) : FString::Printf(TEXT("spawn%d"), S.Named.Num() + 1);
				if (Actor)
				{
					S.Named.Add(Name, Actor);
				}
				else
				{
					S.ActionLines.Add(FString::Printf(TEXT("spawn %s FAILED"), *Name));
				}
			}
		}

		// Mobs: the game's own path (Spawner_BP.SpawnNextWave): SpawnActor facing the player, then Initialize(stats, spawner, seed)
		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		for (const TPair<FString, double>& D : TArray<TPair<FString, double>>{
			{TEXT("maxHP"), 150}, {TEXT("attackSpeed"), 50}, {TEXT("armor"), 0}, {TEXT("hpVamp"), 0},
			{TEXT("movementSpeed"), 300}, {TEXT("damage"), 15}, {TEXT("cdReduc"), 0}, {TEXT("critChance"), 0}})
		{
			Stats->SetNumberField(D.Key, D.Value);
		}
		const TSharedPtr<FJsonObject>* StatOverride = nullptr;
		if (S.Params->TryGetObjectField(TEXT("mob_stats"), StatOverride))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*StatOverride)->Values)
			{
				Stats->SetField(Pair.Key, Pair.Value);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* MobList = nullptr;
		if (S.Params->TryGetArrayField(TEXT("mobs"), MobList))
		{
			// Mobs read Spawner.Wave (their friends) when they shoot / check lines: give them a real Spawner_BP, as the game does.
			AActor* Spawner = nullptr;
			if (UClass* SpawnerClass = Scn_LoadClass(TEXT("/Game/TopDown/Spawner/Spawner_BP.Spawner_BP_C")))
			{
				Spawner = World->SpawnActor<AActor>(SpawnerClass, FTransform(S.PlayerStart));
			}
			FArrayProperty* WaveProperty = Spawner ? CastField<FArrayProperty>(Scn_FindProperty(Spawner->GetClass(), TEXT("Wave"))) : nullptr;
			TMap<FString, int32> Counters;
			for (const TSharedPtr<FJsonValue>& Item : *MobList)
			{
				const TSharedPtr<FJsonObject> Obj = Item->AsObject();
				const FString Type = Obj->GetStringField(TEXT("type"));
				UClass* Class = Scn_LoadClass(Scn_ClassForType(Type));
				if (!Class)
				{
					Scn_Finish(TEXT("mobs: class not found: ") + Type);
					return false;
				}
				const int32 Count = Obj->HasField(TEXT("count")) ? static_cast<int32>(Obj->GetNumberField(TEXT("count"))) : 1;
				const double Spacing = Obj->HasField(TEXT("spacing")) ? Obj->GetNumberField(TEXT("spacing")) : 150.0;
				const int32 Seed = Obj->HasField(TEXT("seed")) ? static_cast<int32>(Obj->GetNumberField(TEXT("seed"))) : 1;
				const TArray<FString> Props = Obj->HasField(TEXT("props")) ? Scn_StringArray(Obj, TEXT("props")) : TArray<FString>{TEXT("currentHp"), TEXT("Moving")};
				FVector Base = FVector::ZeroVector;
				const bool bAbsolute = Scn_ReadXY(Obj, TEXT("location"), Base);
				const double Dist = Obj->HasField(TEXT("distance")) ? Obj->GetNumberField(TEXT("distance")) : 600.0;
				const double Angle = Obj->HasField(TEXT("angle")) ? Obj->GetNumberField(TEXT("angle")) : 0.0;
				for (int32 i = 0; i < Count; ++i)
				{
					FVector P;
					if (bAbsolute)
					{
						P = Base + FVector(0.0, Spacing * i, 0.0);
					}
					else
					{
						const double Rad = FMath::DegreesToRadians(Angle);
						const FVector Dir(FMath::Cos(Rad), FMath::Sin(Rad), 0.0);
						P = S.PlayerStart + Dir * Dist + FVector(-Dir.Y, Dir.X, 0.0) * Spacing * i;
					}
					P.Z = Obj->HasField(TEXT("z")) ? Obj->GetNumberField(TEXT("z")) : 169.0; // Spawner_BP drops mobs from Z=169 (the arena floor top is at Z=74, like a real room)
					const FRotator Look = UKismetMathLibrary::FindLookAtRotation(P, S.PlayerStart);
					FActorSpawnParameters SpawnParams;
					SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					AActor* Mob = World->SpawnActor<AActor>(Class, FTransform(Look, P), SpawnParams);
					if (!Mob)
					{
						S.ActionLines.Add(FString::Printf(TEXT("spawn %s #%d FAILED"), *Type, i + 1));
						continue;
					}
					TSharedPtr<FJsonObject> InitArgs = MakeShared<FJsonObject>();
					InitArgs->SetObjectField(TEXT("stats"), Stats);
					InitArgs->SetNumberField(TEXT("seed"), Seed);
					if (Spawner)
					{
						S.Named.FindOrAdd(TEXT("spawner")) = Spawner;
						InitArgs->SetStringField(TEXT("spawner"), TEXT("@spawner"));
					}
					if (!Scn_CallFunction(Mob, TEXT("Initialize"), InitArgs, Error))
					{
						S.ActionLines.Add(TEXT("Initialize failed: ") + Error);
					}
					if (WaveProperty)
					{
						FScriptArrayHelper Wave(WaveProperty, WaveProperty->ContainerPtrToValuePtr<void>(Spawner));
						const int32 Index = Wave.AddValue();
						if (FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(WaveProperty->Inner))
						{
							Inner->SetObjectPropertyValue(Wave.GetRawPtr(Index), Mob);
						}
					}
					FWatched W;
					W.bMob = true;
					W.Label = FString::Printf(TEXT("%s_%d"), *Type, ++Counters.FindOrAdd(Type));
					W.ClassName = Class->GetName();
					W.Actor = Mob;
					W.Start = P;
					W.Props = Props;
					S.Watched.Add(W);
				}
			}
		}

		S.T0 = World->GetTimeSeconds();
		S.NextSample = 0.0;
		S.State = EScenarioState::Running;
		return true;
	}

	void Scn_Sample(UWorld* World, double T)
	{
		FScenario& S = GScenario;
		APlayerController* PC = World->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		S.SampleTimes.Add(T);
		for (const FString& Name : S.PlayerProps)
		{
			S.PlayerRows.FindOrAdd(Name).Add(Pawn ? Scn_ReadProperty(Pawn, Name) : TEXT("x"));
		}
		for (FWatched& W : S.Watched)
		{
			AActor* Actor = W.Actor.Get();
			if (!Actor || !IsValid(Actor) || !Pawn)
			{
				W.Dist.Add(-1);
				W.Angle.Add(0);
				W.Yaw.Add(0);
				for (const FString& Name : W.Props)
				{
					W.PropRows.FindOrAdd(Name).Add(TEXT("x"));
				}
				continue;
			}
			++W.AliveSamples;
			const FVector Delta = Pawn->GetActorLocation() - Actor->GetActorLocation();
			const double Yaw = Actor->GetActorRotation().Yaw;
			const double ToPlayer = FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
			W.Dist.Add(FVector(Delta.X, Delta.Y, 0.0).Size());
			W.Angle.Add(FMath::FindDeltaAngleDegrees(Yaw, ToPlayer));
			W.Yaw.Add(Yaw);
			for (const FString& Name : W.Props)
			{
				W.PropRows.FindOrAdd(Name).Add(Scn_ReadProperty(Actor, Name));
			}
		}
	}

	void Scn_RunAction(UWorld* World, FTimedAction& A, double T)
	{
		A.bDone = true;
		AActor* Target = Scn_Resolve(A.Target, World);
		FString Line = FString::Printf(TEXT("t=%s %s"), *FString::SanitizeFloat(T, 1), *A.Target);
		if (!Target)
		{
			GScenario.ActionLines.Add(Line + TEXT(": target not found / destroyed"));
			return;
		}
		FString Error;
		if (A.Set.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : A.Set->Values)
			{
				FProperty* Property = Scn_FindProperty(Target->GetClass(), Pair.Key);
				if (!Property)
				{
					Error = FString::Printf(TEXT("no property '%s'"), *Pair.Key);
				}
				else
				{
					Scn_Assign(Property, Property->ContainerPtrToValuePtr<void>(Target), Pair.Value, Error);
				}
			}
			Line += TEXT(" set");
		}
		if (!A.Call.IsEmpty())
		{
			Scn_CallFunction(Target, A.Call, A.Args, Error);
			Line += TEXT(" ") + A.Call + TEXT("()");
		}
		for (const FString& Name : A.Read)
		{
			Line += FString::Printf(TEXT(" %s=%s"), *Name, *Scn_ReadProperty(Target, Name));
		}
		if (!Error.IsEmpty())
		{
			Line += TEXT(" ERROR: ") + Error;
		}
		GScenario.ActionLines.Add(Line);
	}

	FString Scn_Row(const TArray<double>& Values, bool bDashForNegative = true)
	{
		FString Row;
		for (double V : Values)
		{
			Row += (V < 0 && bDashForNegative) ? TEXT("x") : Scn_Num(V);
			Row += TEXT(" ");
		}
		return Row.TrimEnd();
	}

	FString Scn_BuildReport(const FString& Error)
	{
		FScenario& S = GScenario;
		FString Out;
		UWorld* World = PlayWorld();
		Out += FString::Printf(TEXT("SCENARIO %s map=%s duration=%ss samples=%d player=%s@(%s,%s)\n"),
			Error.IsEmpty() ? TEXT("ok") : TEXT("FAILED"), *FPaths::GetBaseFilename(S.Map), *FString::SanitizeFloat(S.Duration, 1),
			S.SampleTimes.Num(), *S.PlayerClass, *Scn_Num(S.PlayerStart.X), *Scn_Num(S.PlayerStart.Y));
		if (!Error.IsEmpty())
		{
			Out += TEXT("error: ") + Error + TEXT("\n");
		}
		FString Times;
		for (double T : S.SampleTimes)
		{
			Times += FString::SanitizeFloat(T, 0).Left(5) + TEXT(" ");
		}
		Out += TEXT("t(s) ") + Times.TrimEnd() + TEXT("\n");
		for (const TPair<FString, TArray<FString>>& Row : S.PlayerRows)
		{
			Out += FString::Printf(TEXT("player.%s %s\n"), *Row.Key, *FString::Join(Row.Value, TEXT(" ")));
		}
		for (const FWatched& W : S.Watched)
		{
			Out += FString::Printf(TEXT("%s [%s] start(%s,%s)\n"), *W.Label, *W.ClassName, *Scn_Num(W.Start.X), *Scn_Num(W.Start.Y));
			Out += TEXT("  dist ") + Scn_Row(W.Dist) + TEXT("\n");
			Out += TEXT("  angle ") + Scn_Row(W.Angle, false) + TEXT("\n");
			for (const TPair<FString, TArray<FString>>& Row : W.PropRows)
			{
				Out += FString::Printf(TEXT("  %s %s\n"), *Row.Key, *FString::Join(Row.Value, TEXT(" ")));
			}
			// Verdict from the samples while alive
			int32 First = INDEX_NONE, Last = INDEX_NONE;
			for (int32 i = 0; i < W.Dist.Num(); ++i)
			{
				if (W.Dist[i] >= 0)
				{
					if (First == INDEX_NONE) { First = i; }
					Last = i;
				}
			}
			if (First != INDEX_NONE && Last > First)
			{
				const double Closed = W.Dist[First] - W.Dist[Last];
				double YawTravel = 0.0;
				for (int32 i = First + 1; i <= Last; ++i)
				{
					YawTravel += FMath::Abs(FMath::FindDeltaAngleDegrees(W.Yaw[i - 1], W.Yaw[i]));
				}
				Out += FString::Printf(TEXT("  => chasing:%s (closed %scm) rotating:%s (yaw travel %sdeg, final angle %sdeg)%s\n"),
					Closed > 50.0 ? TEXT("YES") : TEXT("NO"), *Scn_Num(Closed), YawTravel > 5.0 ? TEXT("YES") : TEXT("NO"),
					*Scn_Num(YawTravel), *Scn_Num(W.Angle[Last]), Last < W.Dist.Num() - 1 ? TEXT(" gone-before-end") : TEXT(""));
			}
			else
			{
				Out += TEXT("  => never alive in a sample\n");
			}
		}
		for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : S.Named)
		{
			Out += FString::Printf(TEXT("%s: %s\n"), *Pair.Key, Pair.Value.IsValid() ? TEXT("still exists") : TEXT("destroyed"));
		}
		for (const FString& Line : S.ActionLines)
		{
			Out += Line + TEXT("\n");
		}

		// Log: warnings/errors plus optional substring, de-duplicated
		FMCPLogQuery Query;
		Query.SinceId = S.LogCursor;
		Query.Limit = 400;
		Query.MinSeverity = ELogVerbosity::Warning;
		TArray<FMCPLogEntry> Entries;
		int64 Next = 0;
		bool bTruncated = false, bGap = false;
		FMCPLogCapture::Get().Query(Query, Entries, Next, bTruncated, bGap);
		if (!S.LogContains.IsEmpty())
		{
			FMCPLogQuery Extra = Query;
			Extra.MinSeverity = ELogVerbosity::Log;
			Extra.Contains = S.LogContains;
			TArray<FMCPLogEntry> More;
			FMCPLogCapture::Get().Query(Extra, More, Next, bTruncated, bGap);
			Entries.Append(More);
		}
		TMap<FString, int32> Counts;
		TArray<FString> Order;
		for (const FMCPLogEntry& E : Entries)
		{
			const FString Line = FString::Printf(TEXT("[%s] %s"), *E.Category.ToString(), *E.Message.Left(220));
			if (!Counts.Contains(Line))
			{
				Order.Add(Line);
			}
			++Counts.FindOrAdd(Line);
		}
		Out += FString::Printf(TEXT("log (%d warn/error lines, %d distinct)\n"), Entries.Num(), Order.Num());
		for (int32 i = 0; i < FMath::Min(Order.Num(), 15); ++i)
		{
			Out += FString::Printf(TEXT("  %dx %s\n"), Counts[Order[i]], *Order[i]);
		}
		return Out;
	}

	void Scn_Finish(const FString& Error)
	{
		FScenario& S = GScenario;
		S.Error = Error;
		S.Report = Scn_BuildReport(Error);
		S.State = EScenarioState::Done;
		FMCPPIE::ClearStartRequest();
		if (!S.bKeepOpen && GEditor && FMCPPIE::IsRunning())
		{
			GEditor->RequestEndPlayMap();
		}
	}

	bool Scn_Tick(float)
	{
		FScenario& S = GScenario;
		const double Wall = FPlatformTime::Seconds() - S.WallStart;
		switch (S.State)
		{
		case EScenarioState::Stopping:
			if (!FMCPPIE::IsRunning())
			{
				S.State = EScenarioState::Starting;
				S.WallStart = FPlatformTime::Seconds();
				FMCPPIE::MarkStartRequested();
				MCPCompat::RequestPlaySession(false, S.Map);
			}
			break;
		case EScenarioState::Starting:
			if (FMCPPIE::IsRunning() && PlayWorld() && PlayWorld()->HasBegunPlay())
			{
				S.State = EScenarioState::Setup;
				S.WallStart = FPlatformTime::Seconds();
			}
			else if (Wall > 60.0)
			{
				Scn_Finish(TEXT("PIE did not start in 60 s"));
			}
			break;
		case EScenarioState::Setup:
			if (UWorld* World = PlayWorld())
			{
				Scn_Setup(World);
			}
			if (S.State == EScenarioState::Setup && Wall > 90.0)
			{
				Scn_Finish(TEXT("setup timed out"));
			}
			break;
		case EScenarioState::Running:
		{
			UWorld* World = PlayWorld();
			if (!World || !FMCPPIE::IsRunning())
			{
				Scn_Finish(TEXT("PIE stopped before the scenario ended"));
				break;
			}
			const double T = World->GetTimeSeconds() - S.T0;
			for (FTimedAction& A : S.Actions)
			{
				if (!A.bDone && T >= A.T)
				{
					Scn_RunAction(World, A, T);
				}
			}
			if (T >= S.NextSample)
			{
				Scn_Sample(World, T);
				S.NextSample += S.Interval;
			}
			if (T >= S.Duration)
			{
				if (S.SampleTimes.Num() == 0 || S.SampleTimes.Last() < S.Duration - 0.01)
				{
					Scn_Sample(World, T);
				}
				Scn_Finish(FString());
			}
			break;
		}
		default:
			break;
		}
		if (S.State == EScenarioState::Done || S.State == EScenarioState::Idle)
		{
			S.Ticker.Reset();
			return false;
		}
		return true;
	}
}

// ============================================================================
// FRunScenarioAction
// ============================================================================

bool FRunScenarioAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!GEditor)
	{
		OutError = TEXT("No editor.");
		return false;
	}
	if (GScenario.State != EScenarioState::Idle && GScenario.State != EScenarioState::Done)
	{
		OutError = TEXT("A scenario is already running. Wait for scenario_report to say done.");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FRunScenarioAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FScenario& S = GScenario;
	S = FScenario();
	GNamed = &S.Named;
	S.Params = Params;
	S.Map = GetOptionalString(Params, TEXT("map"), DefaultArenaMap);
	S.Duration = GetOptionalNumber(Params, TEXT("duration"), 6.0);
	S.Interval = FMath::Max(0.1, GetOptionalNumber(Params, TEXT("interval"), 1.0));
	S.bKeepOpen = GetOptionalBool(Params, TEXT("keep_open"), false);
	S.LogContains = GetOptionalString(Params, TEXT("log_contains"));

	const TArray<TSharedPtr<FJsonValue>>* ActionList = nullptr;
	if (Params->TryGetArrayField(TEXT("actions"), ActionList))
	{
		for (const TSharedPtr<FJsonValue>& Item : *ActionList)
		{
			const TSharedPtr<FJsonObject> Obj = Item->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FTimedAction A;
			A.T = Obj->HasField(TEXT("t")) ? Obj->GetNumberField(TEXT("t")) : 0.0;
			A.Target = Obj->GetStringField(TEXT("target"));
			A.Call = Obj->HasField(TEXT("call")) ? Obj->GetStringField(TEXT("call")) : FString();
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if (Obj->TryGetObjectField(TEXT("args"), Sub)) { A.Args = *Sub; }
			if (Obj->TryGetObjectField(TEXT("set"), Sub)) { A.Set = *Sub; }
			A.Read = Scn_StringArray(Obj, TEXT("read"));
			S.Actions.Add(A);
		}
	}

	S.LogCursor = FMCPLogCapture::Get().GetLatestId();
	S.WallStart = FPlatformTime::Seconds();
	if (FMCPPIE::IsActiveOrPending())
	{
		S.State = EScenarioState::Stopping;
		if (FMCPPIE::IsRunning())
		{
			GEditor->RequestEndPlayMap();
		}
		FMCPPIE::ClearStartRequest();
	}
	else
	{
		S.State = EScenarioState::Starting;
		FMCPPIE::MarkStartRequested();
		MCPCompat::RequestPlaySession(false, S.Map);
	}
	S.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Scn_Tick), 0.0f);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("state"), TEXT("running"));
	Result->SetStringField(TEXT("message"), TEXT("Scenario started. Call scenario_report until state is done."));
	return CreateSuccessResponse(Result);
}

// ============================================================================
// FScenarioReportAction
// ============================================================================

TSharedPtr<FJsonObject> FScenarioReportAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FScenario& S = GScenario;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	switch (S.State)
	{
	case EScenarioState::Idle:
		Result->SetStringField(TEXT("state"), TEXT("idle"));
		break;
	case EScenarioState::Done:
		Result->SetStringField(TEXT("state"), TEXT("done"));
		Result->SetStringField(TEXT("report"), S.Report);
		break;
	default:
	{
		Result->SetStringField(TEXT("state"), TEXT("running"));
		const UWorld* World = PlayWorld();
		if (S.State == EScenarioState::Running && World)
		{
			Result->SetNumberField(TEXT("t"), World->GetTimeSeconds() - S.T0);
		}
		break;
	}
	}
	return CreateSuccessResponse(Result);
}
