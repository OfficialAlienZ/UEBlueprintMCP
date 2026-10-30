// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/ObserveActions.h"
#include "Actions/PIEActions.h"
#include "MCPCommonUtils.h"
#include "MCPLogCapture.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"

namespace
{
	// ------------------------------------------------------------------
	// Property reading / comparison
	// ------------------------------------------------------------------

	FString Observe_NormalizeName(const FString& Name)
	{
		FString Result = Name.ToLower();
		Result.ReplaceInline(TEXT(" "), TEXT(""));
		Result.ReplaceInline(TEXT("_"), TEXT(""));
		return Result;
	}

	FProperty* Observe_FindProperty(const UStruct* Struct, const FString& Name)
	{
		const FString Wanted = Observe_NormalizeName(Name);
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (Observe_NormalizeName(It->GetName()) == Wanted)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** A property value reduced to what conditions can compare. */
	struct FObservedValue
	{
		bool bHasNumber = false;
		bool bHasBool = false;
		bool bHasString = false;
		double Number = 0.0;
		bool Bool = false;
		FString String;

		TSharedPtr<FJsonValue> ToJson() const
		{
			if (bHasBool)
			{
				return MakeShared<FJsonValueBoolean>(Bool);
			}
			if (bHasString && !bHasNumber)
			{
				return MakeShared<FJsonValueString>(String);
			}
			if (bHasNumber)
			{
				return MakeShared<FJsonValueNumber>(Number);
			}
			return MakeShared<FJsonValueNull>();
		}
	};

	enum class EReadResult : uint8 { Ok, Error, Unavailable };

	bool Observe_ReadValue(FProperty* Property, const void* ValuePtr, FObservedValue& Out)
	{
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			Out.bHasBool = true;
			Out.Bool = BoolProperty->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			Out.bHasNumber = true;
			Out.Number = Numeric->IsFloatingPoint()
				? Numeric->GetFloatingPointPropertyValue(ValuePtr)
				: static_cast<double>(Numeric->GetSignedIntPropertyValue(ValuePtr));
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				Out.bHasString = true;
				Out.String = Enum->GetNameStringByValue(Numeric->GetSignedIntPropertyValue(ValuePtr));
			}
			return true;
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 Value = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			Out.bHasNumber = true;
			Out.Number = static_cast<double>(Value);
			if (const UEnum* Enum = EnumProperty->GetEnum())
			{
				Out.bHasString = true;
				Out.String = Enum->GetNameStringByValue(Value);
			}
			return true;
		}
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			Out.bHasString = true;
			Out.String = StrProperty->GetPropertyValue(ValuePtr);
			return true;
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			Out.bHasString = true;
			Out.String = NameProperty->GetPropertyValue(ValuePtr).ToString();
			return true;
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			Out.bHasString = true;
			Out.String = TextProperty->GetPropertyValue(ValuePtr).ToString();
			return true;
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			Out.bHasString = true;
			Out.String = Object ? Object->GetName() : FString(TEXT("None"));
			return true;
		}
		return false;
	}

	/** Resolve a dotted property path ("Health", "Stats.Level", "TargetRef.Health") on an object and read it. */
	EReadResult Observe_ReadPath(UObject* Root, const FString& Path, FObservedValue& Out, FString& OutMessage)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		if (Segments.Num() == 0)
		{
			OutMessage = TEXT("property_name is empty");
			return EReadResult::Error;
		}

		const UStruct* CurrentStruct = Root->GetClass();
		const void* CurrentData = Root;
		FProperty* Property = nullptr;
		const void* ValuePtr = nullptr;

		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			Property = Observe_FindProperty(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				OutMessage = FString::Printf(TEXT("Property '%s' not found on %s"), *Segments[Index], *CurrentStruct->GetName());
				return EReadResult::Error;
			}
			ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentData);
			if (Index == Segments.Num() - 1)
			{
				break;
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				CurrentStruct = StructProperty->Struct;
				CurrentData = ValuePtr;
			}
			else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				UObject* Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
				if (!Object)
				{
					OutMessage = FString::Printf(TEXT("'%s' is None"), *Segments[Index]);
					return EReadResult::Unavailable;
				}
				CurrentStruct = Object->GetClass();
				CurrentData = Object;
			}
			else
			{
				OutMessage = FString::Printf(TEXT("'%s' is not a struct or object; cannot read '%s' inside it"), *Segments[Index], *Segments[Index + 1]);
				return EReadResult::Error;
			}
		}

		if (!Observe_ReadValue(Property, ValuePtr, Out))
		{
			OutMessage = FString::Printf(TEXT("Property '%s' has unsupported type %s. Supported: bool, numbers, enums, string/name/text, object references. For structs use a dotted path (e.g. Stats.Level)."),
				*Property->GetName(), *Property->GetCPPType());
			return EReadResult::Error;
		}
		return EReadResult::Ok;
	}

	bool Observe_IsValidOperator(const FString& Op)
	{
		return Op == TEXT("==") || Op == TEXT("!=") || Op == TEXT("<") || Op == TEXT("<=")
			|| Op == TEXT(">") || Op == TEXT(">=") || Op == TEXT("contains");
	}

	bool Observe_CompareNumbers(double A, const FString& Op, double B, double Tolerance)
	{
		if (Op == TEXT("==")) { return FMath::Abs(A - B) <= Tolerance; }
		if (Op == TEXT("!=")) { return FMath::Abs(A - B) > Tolerance; }
		if (Op == TEXT("<")) { return A < B; }
		if (Op == TEXT("<=")) { return A <= B + Tolerance; }
		if (Op == TEXT(">")) { return A > B; }
		return A >= B - Tolerance;
	}

	/** Compare an observed value with the JSON `value` parameter. Returns false and sets OutError if they are not comparable. */
	bool Observe_Compare(const FObservedValue& Actual, const FString& Op, const TSharedPtr<FJsonValue>& Expected, double Tolerance, bool& bOutMet, FString& OutError)
	{
		if (!Expected.IsValid() || Expected->IsNull())
		{
			OutError = TEXT("value is required");
			return false;
		}

		// Numbers (and bools / numeric strings when the property is numeric or bool)
		bool bExpectedIsNumber = false;
		double ExpectedNumber = 0.0;
		if (Expected->Type == EJson::Number) { bExpectedIsNumber = true; ExpectedNumber = Expected->AsNumber(); }
		else if (Expected->Type == EJson::Boolean) { bExpectedIsNumber = true; ExpectedNumber = Expected->AsBool() ? 1.0 : 0.0; }
		else if (Expected->Type == EJson::String && FCString::IsNumeric(*Expected->AsString())) { bExpectedIsNumber = true; ExpectedNumber = FCString::Atod(*Expected->AsString()); }

		const bool bActualIsNumeric = Actual.bHasNumber || Actual.bHasBool;

		if (Expected->Type == EJson::String && Actual.bHasString)
		{
			const FString& Wanted = Expected->AsString();
			FString Short = Actual.String;
			int32 ScopeIndex = INDEX_NONE;
			if (Short.FindLastChar(TEXT(':'), ScopeIndex))
			{
				Short = Short.Mid(ScopeIndex + 1);
			}
			if (Op == TEXT("contains"))
			{
				bOutMet = Actual.String.Contains(Wanted, ESearchCase::IgnoreCase);
				return true;
			}
			if (Op == TEXT("==") || Op == TEXT("!="))
			{
				const bool bEqual = Actual.String.Equals(Wanted, ESearchCase::IgnoreCase) || Short.Equals(Wanted, ESearchCase::IgnoreCase);
				bOutMet = (Op == TEXT("==")) ? bEqual : !bEqual;
				return true;
			}
			OutError = FString::Printf(TEXT("Operator '%s' cannot compare strings; use ==, != or contains"), *Op);
			return false;
		}

		if (bExpectedIsNumber && bActualIsNumeric)
		{
			if (Op == TEXT("contains"))
			{
				OutError = TEXT("Operator 'contains' needs a string value and a string property");
				return false;
			}
			const double ActualNumber = Actual.bHasNumber ? Actual.Number : (Actual.Bool ? 1.0 : 0.0);
			bOutMet = Observe_CompareNumbers(ActualNumber, Op, ExpectedNumber, Tolerance);
			return true;
		}

		OutError = TEXT("The property type and the value type cannot be compared (e.g. a string value for a number property)");
		return false;
	}

	/** Apply "negate", store the final result and common fields. */
	TSharedPtr<FJsonObject> Observe_Finish(const TSharedPtr<FJsonObject>& Result, bool bRawMet, bool bNegate)
	{
		Result->SetBoolField(TEXT("met"), bRawMet != bNegate);
		Result->SetBoolField(TEXT("pie_running"), FMCPPIE::IsRunning());
		if (FMCPPIE::IsRunning())
		{
			Result->SetNumberField(TEXT("game_time"), GEditor->PlayWorld->GetTimeSeconds());
		}
		return Result;
	}

	bool Observe_TypeNeedsPIE(const FString& Type)
	{
		return Type != TEXT("log_contains") && Type != TEXT("pie_stopped");
	}

	bool Observe_GetVector(const TSharedPtr<FJsonObject>& Params, const FString& Field, FVector& OutVector)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Params->TryGetArrayField(Field, Array) || !Array || Array->Num() != 3)
		{
			return false;
		}
		OutVector = FVector((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber(), (*Array)[2]->AsNumber());
		return true;
	}
}

// ============================================================================
// FCheckConditionAction
// ============================================================================

bool FCheckConditionAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString Type;
	if (!GetRequiredString(Params, TEXT("type"), Type, OutError))
	{
		return false;
	}

	static const TCHAR* const KnownTypes[] = { TEXT("game_time"), TEXT("actor_exists"), TEXT("actor_property"), TEXT("actor_distance"), TEXT("log_contains"), TEXT("pie_stopped") };
	bool bKnown = false;
	for (const TCHAR* Known : KnownTypes)
	{
		bKnown |= Type == Known;
	}
	if (!bKnown)
	{
		OutError = FString::Printf(TEXT("Unknown condition type '%s'. Use game_time, actor_exists, actor_property, actor_distance, log_contains or pie_stopped."), *Type);
		return false;
	}

	if (Observe_TypeNeedsPIE(Type) && !FMCPPIE::IsRunning())
	{
		OutError = FMCPPIE::IsStartPending()
			? TEXT("PIE is still starting. Wait until get_pie_status reports 'running'.")
			: TEXT("No PIE session is running (it may have ended). Call start_pie first.");
		return false;
	}

	const FString Op = GetOptionalString(Params, TEXT("operator"), Type == TEXT("actor_distance") ? TEXT("<=") : TEXT("=="));
	if (!Observe_IsValidOperator(Op))
	{
		OutError = FString::Printf(TEXT("Invalid operator '%s'. Use ==, !=, <, <=, >, >= or contains."), *Op);
		return false;
	}

	FString Ignored;
	if (Type == TEXT("game_time"))
	{
		if (!Params->HasTypedField<EJson::Number>(TEXT("seconds")))
		{
			OutError = TEXT("game_time needs 'seconds' (absolute game time)");
			return false;
		}
	}
	else if (Type == TEXT("actor_exists"))
	{
		return GetRequiredString(Params, TEXT("actor_name"), Ignored, OutError);
	}
	else if (Type == TEXT("actor_property"))
	{
		if (!GetRequiredString(Params, TEXT("actor_name"), Ignored, OutError) || !GetRequiredString(Params, TEXT("property_name"), Ignored, OutError))
		{
			return false;
		}
		if (!Params->HasField(TEXT("value")))
		{
			OutError = TEXT("actor_property needs 'value' to compare with");
			return false;
		}
	}
	else if (Type == TEXT("actor_distance"))
	{
		if (!GetRequiredString(Params, TEXT("actor_name"), Ignored, OutError))
		{
			return false;
		}
		if (GetOptionalString(Params, TEXT("target_actor_name")).IsEmpty() && !Params->HasField(TEXT("location")))
		{
			OutError = TEXT("actor_distance needs 'target_actor_name' or 'location' [x, y, z]");
			return false;
		}
		FVector Location;
		if (Params->HasField(TEXT("location")) && !Observe_GetVector(Params, TEXT("location"), Location))
		{
			OutError = TEXT("location must be an array of 3 numbers [x, y, z]");
			return false;
		}
		if (!Params->HasTypedField<EJson::Number>(TEXT("value")))
		{
			OutError = TEXT("actor_distance needs a numeric 'value' (distance in cm)");
			return false;
		}
	}
	else if (Type == TEXT("log_contains"))
	{
		return GetRequiredString(Params, TEXT("text"), Ignored, OutError);
	}
	return true;
}

TSharedPtr<FJsonObject> FCheckConditionAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString Type = GetOptionalString(Params, TEXT("type"));
	const bool bNegate = GetOptionalBool(Params, TEXT("negate"), false);
	const FString Op = GetOptionalString(Params, TEXT("operator"), Type == TEXT("actor_distance") ? TEXT("<=") : TEXT("=="));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), Type);

	if (Type == TEXT("pie_stopped"))
	{
		return CreateSuccessResponse(Observe_Finish(Result, !FMCPPIE::IsRunning(), bNegate));
	}

	if (Type == TEXT("log_contains"))
	{
		FMCPLogQuery Query;
		Query.SinceId = static_cast<int64>(GetOptionalNumber(Params, TEXT("since_id"), 0.0));
		Query.Limit = 1;
		Query.MinSeverity = ELogVerbosity::VeryVerbose;
		Query.Contains = GetOptionalString(Params, TEXT("text"));
		if (const TArray<TSharedPtr<FJsonValue>>* Categories = GetOptionalArray(Params, TEXT("categories")))
		{
			for (const TSharedPtr<FJsonValue>& Category : *Categories)
			{
				Query.Categories.Add(Category->AsString());
			}
		}

		TArray<FMCPLogEntry> Entries;
		int64 NextId = 0;
		bool bTruncated = false;
		bool bGap = false;
		FMCPLogCapture::Get().Query(Query, Entries, NextId, bTruncated, bGap);

		const bool bFound = Entries.Num() > 0;
		if (bFound)
		{
			Result->SetStringField(TEXT("matched_message"), Entries[0].Message);
			Result->SetNumberField(TEXT("matched_id"), static_cast<double>(Entries[0].Id));
		}
		Result->SetNumberField(TEXT("next_id"), static_cast<double>(FMCPLogCapture::Get().GetLatestId()));
		return CreateSuccessResponse(Observe_Finish(Result, bFound, bNegate));
	}

	UWorld* World = GEditor->PlayWorld;

	if (Type == TEXT("game_time"))
	{
		const double Seconds = GetOptionalNumber(Params, TEXT("seconds"), 0.0);
		return CreateSuccessResponse(Observe_Finish(Result, World->GetTimeSeconds() >= Seconds, bNegate));
	}

	const FString ActorName = GetOptionalString(Params, TEXT("actor_name"));
	AActor* Actor = FMCPCommonUtils::FindActor(World, ActorName);

	if (Type == TEXT("actor_exists"))
	{
		return CreateSuccessResponse(Observe_Finish(Result, Actor != nullptr, bNegate));
	}

	if (!Actor)
	{
		Result->SetStringField(TEXT("reason"), FString::Printf(TEXT("Actor '%s' not found (yet)"), *ActorName));
		return CreateSuccessResponse(Observe_Finish(Result, false, bNegate));
	}

	if (Type == TEXT("actor_distance"))
	{
		FVector TargetLocation = FVector::ZeroVector;
		const FString TargetName = GetOptionalString(Params, TEXT("target_actor_name"));
		if (!TargetName.IsEmpty())
		{
			AActor* Target = FMCPCommonUtils::FindActor(World, TargetName);
			if (!Target)
			{
				Result->SetStringField(TEXT("reason"), FString::Printf(TEXT("Actor '%s' not found (yet)"), *TargetName));
				return CreateSuccessResponse(Observe_Finish(Result, false, bNegate));
			}
			TargetLocation = Target->GetActorLocation();
		}
		else
		{
			Observe_GetVector(Params, TEXT("location"), TargetLocation);
		}
		const double Distance = FVector::Dist(Actor->GetActorLocation(), TargetLocation);
		const double Limit = GetOptionalNumber(Params, TEXT("value"), 0.0);
		Result->SetNumberField(TEXT("distance"), Distance);
		return CreateSuccessResponse(Observe_Finish(Result, Observe_CompareNumbers(Distance, Op, Limit, 0.0), bNegate));
	}

	// actor_property
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
			return CreateErrorResponse(FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *ComponentName, *Actor->GetName()), TEXT("not_found"));
		}
	}

	FObservedValue Observed;
	FString Message;
	const EReadResult ReadResult = Observe_ReadPath(Target, GetOptionalString(Params, TEXT("property_name")), Observed, Message);
	if (ReadResult == EReadResult::Error)
	{
		return CreateErrorResponse(Message, TEXT("validation_failed"));
	}
	if (ReadResult == EReadResult::Unavailable)
	{
		Result->SetStringField(TEXT("reason"), Message);
		return CreateSuccessResponse(Observe_Finish(Result, false, bNegate));
	}

	Result->SetField(TEXT("current_value"), Observed.ToJson());
	bool bMet = false;
	if (!Observe_Compare(Observed, Op, Params->TryGetField(TEXT("value")), GetOptionalNumber(Params, TEXT("tolerance"), 0.0001), bMet, Message))
	{
		return CreateErrorResponse(Message, TEXT("validation_failed"));
	}
	return CreateSuccessResponse(Observe_Finish(Result, bMet, bNegate));
}

// ============================================================================
// FTakeScreenshotAction
// ============================================================================

namespace
{
	/** Pick the viewport to capture. Sets OutTarget to "pie" or "editor". */
	FViewport* Observe_ResolveViewport(const FString& Requested, FString& OutTarget, FString& OutError)
	{
		const bool bWantPIE = Requested == TEXT("pie") || (Requested == TEXT("auto") && FMCPPIE::IsRunning());
		if (bWantPIE)
		{
			if (!FMCPPIE::IsRunning())
			{
				OutError = TEXT("No PIE session is running. Call start_pie first, or use target='editor'.");
				return nullptr;
			}
			UGameViewportClient* GameViewport = GEditor->PlayWorld->GetGameViewport();
			if (!GameViewport || !GameViewport->Viewport)
			{
				OutError = TEXT("The PIE game has no viewport yet (still starting, or the window is minimised).");
				return nullptr;
			}
			OutTarget = TEXT("pie");
			return GameViewport->Viewport;
		}

		FViewport* Viewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
		if (!Viewport)
		{
			OutError = TEXT("No active editor viewport is available.");
			return nullptr;
		}
		OutTarget = TEXT("editor");
		return Viewport;
	}

	/** Box-filter downscale. */
	void Observe_Downscale(const TArray<FColor>& Source, int32 SrcWidth, int32 SrcHeight, int32 DstWidth, int32 DstHeight, TArray<FColor>& OutPixels)
	{
		OutPixels.SetNumUninitialized(DstWidth * DstHeight);
		for (int32 DstY = 0; DstY < DstHeight; ++DstY)
		{
			const int32 Y0 = static_cast<int32>(static_cast<int64>(DstY) * SrcHeight / DstHeight);
			const int32 Y1 = FMath::Clamp(static_cast<int32>(static_cast<int64>(DstY + 1) * SrcHeight / DstHeight), Y0 + 1, SrcHeight);
			for (int32 DstX = 0; DstX < DstWidth; ++DstX)
			{
				const int32 X0 = static_cast<int32>(static_cast<int64>(DstX) * SrcWidth / DstWidth);
				const int32 X1 = FMath::Clamp(static_cast<int32>(static_cast<int64>(DstX + 1) * SrcWidth / DstWidth), X0 + 1, SrcWidth);
				uint32 R = 0, G = 0, B = 0;
				for (int32 Y = Y0; Y < Y1; ++Y)
				{
					for (int32 X = X0; X < X1; ++X)
					{
						const FColor& Pixel = Source[Y * SrcWidth + X];
						R += Pixel.R;
						G += Pixel.G;
						B += Pixel.B;
					}
				}
				const uint32 Count = static_cast<uint32>((Y1 - Y0) * (X1 - X0));
				OutPixels[DstY * DstWidth + DstX] = FColor(static_cast<uint8>(R / Count), static_cast<uint8>(G / Count), static_cast<uint8>(B / Count), 255);
			}
		}
	}
}

bool FTakeScreenshotAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	const FString Target = GetOptionalString(Params, TEXT("target"), TEXT("auto"));
	if (Target != TEXT("auto") && Target != TEXT("pie") && Target != TEXT("editor"))
	{
		OutError = FString::Printf(TEXT("Invalid target '%s'. Use auto, pie or editor."), *Target);
		return false;
	}
	if (GetOptionalNumber(Params, TEXT("max_width"), 960.0) < 0.0)
	{
		OutError = TEXT("max_width must not be negative (0 = full size)");
		return false;
	}
	FString TargetName;
	return Observe_ResolveViewport(Target, TargetName, OutError) != nullptr;
}

TSharedPtr<FJsonObject> FTakeScreenshotAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString TargetName;
	FString Error;
	FViewport* Viewport = Observe_ResolveViewport(GetOptionalString(Params, TEXT("target"), TEXT("auto")), TargetName, Error);
	if (!Viewport)
	{
		return CreateErrorResponse(Error, TEXT("not_found"));
	}

	const FIntPoint Size = Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return CreateErrorResponse(TEXT("The viewport has no size (window minimised or hidden?)"), TEXT("execution_failed"));
	}

	TArray<FColor> Pixels;
	if (!Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
	{
		return CreateErrorResponse(TEXT("Could not read pixels from the viewport"), TEXT("execution_failed"));
	}
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}

	int32 Width = Size.X;
	int32 Height = Size.Y;
	const int32 MaxWidth = static_cast<int32>(GetOptionalNumber(Params, TEXT("max_width"), 960.0));
	if (MaxWidth > 0 && Width > MaxWidth)
	{
		const int32 NewHeight = FMath::Max(1, static_cast<int32>(static_cast<int64>(Height) * MaxWidth / Width));
		TArray<FColor> Scaled;
		Observe_Downscale(Pixels, Width, Height, MaxWidth, NewHeight, Scaled);
		Pixels = MoveTemp(Scaled);
		Width = MaxWidth;
		Height = NewHeight;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), static_cast<int64>(Pixels.Num()) * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
	{
		return CreateErrorResponse(TEXT("Could not encode the screenshot as PNG"), TEXT("execution_failed"));
	}
	const TArray64<uint8> Png = Wrapper->GetCompressed();

	// File name: shot_<time>[_label].png (label restricted to safe characters)
	FString Label;
	for (const TCHAR Character : GetOptionalString(Params, TEXT("label")))
	{
		if (FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_'))
		{
			Label.AppendChar(Character);
		}
	}
	const FDateTime Now = FDateTime::Now();
	const FString FileName = FString::Printf(TEXT("shot_%s_%03d%s%s.png"), *Now.ToString(TEXT("%Y%m%d_%H%M%S")), Now.GetMillisecond(),
		Label.IsEmpty() ? TEXT("") : TEXT("_"), *Label);

	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"), TEXT("Screenshots")));
	IFileManager& FileManager = IFileManager::Get();
	FileManager.MakeDirectory(*Directory, true);
	const FString FilePath = FPaths::Combine(Directory, FileName);

	TUniquePtr<FArchive> Writer(FileManager.CreateFileWriter(*FilePath));
	if (!Writer.IsValid())
	{
		return CreateErrorResponse(FString::Printf(TEXT("Could not write %s"), *FilePath), TEXT("execution_failed"));
	}
	Writer->Serialize(const_cast<uint8*>(Png.GetData()), Png.Num());
	Writer->Close();

	// Keep the folder small: delete the oldest files beyond 100.
	TArray<FString> Existing;
	FileManager.FindFiles(Existing, *FPaths::Combine(Directory, TEXT("*.png")), true, false);
	if (Existing.Num() > 100)
	{
		Existing.Sort();
		for (int32 Index = 0; Index < Existing.Num() - 100; ++Index)
		{
			FileManager.Delete(*FPaths::Combine(Directory, Existing[Index]));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("file"), FilePath);
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	Result->SetStringField(TEXT("target"), TargetName);
	if (FMCPPIE::IsRunning())
	{
		Result->SetNumberField(TEXT("game_time"), GEditor->PlayWorld->GetTimeSeconds());
	}
	return CreateSuccessResponse(Result);
}
