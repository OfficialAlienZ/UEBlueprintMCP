// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/NiagaraActions.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
#include "NiagaraParameterStore.h"
#include "Misc/Paths.h"

namespace
{
	enum class ENiagaraKind { None, Float, Int, Bool, FloatVec };

	ENiagaraKind NP_Kind(const FNiagaraTypeDefinition& Type, int32& OutComponents)
	{
		OutComponents = 1;
		if (Type == FNiagaraTypeDefinition::GetFloatDef()) return ENiagaraKind::Float;
		if (Type == FNiagaraTypeDefinition::GetIntDef()) return ENiagaraKind::Int;
		if (Type == FNiagaraTypeDefinition::GetBoolDef()) return ENiagaraKind::Bool;
		if (Type == FNiagaraTypeDefinition::GetVec2Def()) { OutComponents = 2; return ENiagaraKind::FloatVec; }
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef()) { OutComponents = 3; return ENiagaraKind::FloatVec; }
		if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetColorDef() || Type == FNiagaraTypeDefinition::GetQuatDef()) { OutComponents = 4; return ENiagaraKind::FloatVec; }
		return ENiagaraKind::None;
	}

	FString NP_Format(const FNiagaraParameterStore& Store, const FNiagaraVariableBase& Var, ENiagaraKind Kind, int32 N)
	{
		const int32 Offset = Store.IndexOf(Var);
		if (Offset == INDEX_NONE) return FString();
		const uint8* Data = Store.GetParameterDataArray().GetData() + Offset;
		switch (Kind)
		{
		case ENiagaraKind::Float: return FString::SanitizeFloat(*reinterpret_cast<const float*>(Data));
		case ENiagaraKind::Int: return FString::FromInt(*reinterpret_cast<const int32*>(Data));
		case ENiagaraKind::Bool: return (*reinterpret_cast<const int32*>(Data) != 0) ? TEXT("true") : TEXT("false");
		case ENiagaraKind::FloatVec:
		{
			TArray<FString> Parts;
			for (int32 i = 0; i < N; ++i) Parts.Add(FString::SanitizeFloat(reinterpret_cast<const float*>(Data)[i]));
			return TEXT("[") + FString::Join(Parts, TEXT(",")) + TEXT("]");
		}
		default: return FString();
		}
	}

	UNiagaraSystem* NP_Load(const FString& Path)
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path);
		if (!System && !Path.Contains(TEXT(".")))
		{
			System = LoadObject<UNiagaraSystem>(nullptr, *(Path + TEXT(".") + FPaths::GetBaseFilename(Path)));
		}
		return System;
	}

	void NP_CollectScripts(UNiagaraSystem* System, TArray<UNiagaraScript*>& Out)
	{
		if (UNiagaraScript* S = System->GetSystemSpawnScript()) Out.Add(S);
		if (UNiagaraScript* S = System->GetSystemUpdateScript()) Out.Add(S);
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
			{
				TArray<UNiagaraScript*> Scripts;
				Data->GetScripts(Scripts, false, false);
				for (UNiagaraScript* S : Scripts) { if (S) Out.AddUnique(S); }
			}
		}
	}

	bool NP_ReadFloats(const TSharedPtr<FJsonValue>& Value, TArray<float>& Out)
	{
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray()) Out.Add((float)Item->AsNumber());
		}
		else if (Value->Type == EJson::Boolean) Out.Add(Value->AsBool() ? 1.f : 0.f);
		else if (Value->Type == EJson::Number) Out.Add((float)Value->AsNumber());
		else return false;
		return true;
	}

	struct FNPPending
	{
		UNiagaraScript* Script = nullptr;
		FNiagaraVariable Var;
		ENiagaraKind Kind = ENiagaraKind::None;
		int32 N = 1;
		TArray<float> Floats;
		FString Name;
	};
}

bool FNiagaraParamsAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Path;
	if (!GetRequiredString(Params, TEXT("asset_path"), Path, OutError)) return false;
	if (!NP_Load(Path))
	{
		OutError = FString::Printf(TEXT("Niagara system '%s' not found."), *Path);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FNiagaraParamsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UNiagaraSystem* System = NP_Load(GetOptionalString(Params, TEXT("asset_path")));
	const FString Filter = GetOptionalString(Params, TEXT("filter"));
	int32 Limit = 60;
	double LimitValue = 0;
	if (Params->TryGetNumberField(TEXT("limit"), LimitValue) && LimitValue > 0) Limit = (int32)LimitValue;

	TArray<UNiagaraScript*> Scripts;
	NP_CollectScripts(System, Scripts);

	const TSharedPtr<FJsonObject>* SetObj = nullptr;
	const bool bSet = Params->TryGetObjectField(TEXT("set"), SetObj) && SetObj && (*SetObj)->Values.Num() > 0;

	TArray<TSharedPtr<FJsonValue>> SetLines;
	if (bSet)
	{
		// Resolve and validate every key first so a bad name or value changes nothing.
		TArray<FNPPending> Pending;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SetObj)->Values)
		{
			TArray<FNPPending> Exact, Partial;
			for (UNiagaraScript* Script : Scripts)
			{
				for (const FNiagaraVariableWithOffset& V : Script->RapidIterationParameters.ReadParameterVariables())
				{
					int32 N = 1;
					const ENiagaraKind Kind = NP_Kind(V.GetType(), N);
					if (Kind == ENiagaraKind::None) continue;
					const FString Name = V.GetName().ToString();
					const bool bExact = Name.Equals(Pair.Key, ESearchCase::IgnoreCase);
					if (!bExact && !Name.Contains(Pair.Key, ESearchCase::IgnoreCase)) continue;
					FNPPending P;
					P.Script = Script;
					P.Var = FNiagaraVariable(V.GetType(), V.GetName());
					P.Kind = Kind;
					P.N = N;
					P.Name = Name;
					(bExact ? Exact : Partial).Add(P);
				}
			}
			TArray<FNPPending>& Chosen = Exact.Num() > 0 ? Exact : Partial;
			TSet<FString> Names;
			for (const FNPPending& P : Chosen) Names.Add(P.Name);
			if (Names.Num() == 0)
			{
				return CreateErrorResponse(FString::Printf(TEXT("No editable parameter matches '%s'. List with filter first."), *Pair.Key), TEXT("not_found"));
			}
			if (Names.Num() > 1)
			{
				return CreateErrorResponse(FString::Printf(TEXT("'%s' is ambiguous: %s"), *Pair.Key, *FString::Join(Names.Array(), TEXT(" | "))), TEXT("validation_failed"));
			}
			TArray<float> Floats;
			if (!NP_ReadFloats(Pair.Value, Floats) || Floats.Num() != Chosen[0].N)
			{
				return CreateErrorResponse(FString::Printf(TEXT("'%s' needs %d numeric value(s)."), *Chosen[0].Name, Chosen[0].N), TEXT("validation_failed"));
			}
			for (FNPPending& P : Chosen) P.Floats = Floats;
			Pending.Append(Chosen);
		}

		for (const FNPPending& P : Pending)
		{
			uint8 Buffer[16] = {};
			if (P.Kind == ENiagaraKind::Int) { *reinterpret_cast<int32*>(Buffer) = (int32)P.Floats[0]; }
			else if (P.Kind == ENiagaraKind::Bool) { *reinterpret_cast<int32*>(Buffer) = (P.Floats[0] != 0.f) ? INT32_MIN : 0; }
			else { for (int32 i = 0; i < P.N; ++i) reinterpret_cast<float*>(Buffer)[i] = P.Floats[i]; }

			P.Script->Modify();
			P.Script->RapidIterationParameters.SetParameterData(Buffer, P.Var, false);
			SetLines.Add(MakeShared<FJsonValueString>(P.Name + TEXT("=") + NP_Format(P.Script->RapidIterationParameters, P.Var, P.Kind, P.N)));
		}
		System->Modify();
		System->MarkPackageDirty();
	}

	if (bSet && Filter.IsEmpty())
	{
		// A set without a filter only echoes the changed lines (keeps the answer short).
		TSharedPtr<FJsonObject> SetResult = MakeShared<FJsonObject>();
		SetResult->SetArrayField(TEXT("set"), SetLines);
		return CreateSuccessResponse(SetResult);
	}

	TArray<TSharedPtr<FJsonValue>> Lines;
	int32 Total = 0;
	TSet<FString> Seen;
	for (UNiagaraScript* Script : Scripts)
	{
		for (const FNiagaraVariableWithOffset& V : Script->RapidIterationParameters.ReadParameterVariables())
		{
			int32 N = 1;
			const ENiagaraKind Kind = NP_Kind(V.GetType(), N);
			if (Kind == ENiagaraKind::None) continue;
			const FString Name = V.GetName().ToString();
			if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase)) continue;
			if (Seen.Contains(Name)) continue;
			Seen.Add(Name);
			++Total;
			if (Lines.Num() < Limit)
			{
				Lines.Add(MakeShared<FJsonValueString>(Name + TEXT("=") + NP_Format(Script->RapidIterationParameters, V, Kind, N)));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (bSet) Result->SetArrayField(TEXT("set"), SetLines);
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetArrayField(TEXT("params"), Lines);
	return CreateSuccessResponse(Result);
}
