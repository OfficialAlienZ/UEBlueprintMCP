// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/AssetInspectionActions.h"
#include "MCPCompat.h"
#include "MCPInspectionUtils.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Engine/CurveTable.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "UObject/UnrealType.h"

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> AssetInspect_FJsonArray;

	TSharedPtr<FJsonValue> AssetInspect_AsValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	// ---- Enum ----

	UEnum* AssetInspect_ResolveEnum(const FString& EnumName)
	{
		if (EnumName.StartsWith(TEXT("/")))
		{
			FString ObjectPath = EnumName;
			if (!ObjectPath.Contains(TEXT(".")))
			{
				FString ShortName;
				ObjectPath.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				ObjectPath = ObjectPath + TEXT(".") + ShortName;
			}
			return LoadObject<UEnum>(nullptr, *ObjectPath);
		}

		TArray<UClass*> Classes;
		Classes.Add(UUserDefinedEnum::StaticClass());
		if (UEnum* Asset = Cast<UEnum>(FMCPInspectionUtils::FindAsset(EnumName, Classes)))
		{
			return Asset;
		}
		return MCPCompat::FindEnum(EnumName);
	}

	// ---- Curves ----

	UCurveTable* AssetInspect_AsCurveTable(UObject* Asset)
	{
		return Cast<UCurveTable>(Asset);
	}

	TArray<UClass*> AssetInspect_CurveClasses()
	{
		TArray<UClass*> Classes;
		Classes.Add(UCurveFloat::StaticClass());
		Classes.Add(UCurveVector::StaticClass());
		Classes.Add(UCurveLinearColor::StaticClass());
		Classes.Add(UCurveTable::StaticClass());
		return Classes;
	}

	// ---- Data tables ----

	TArray<UClass*> AssetInspect_DataTableClasses()
	{
		TArray<UClass*> Classes;
		Classes.Add(UDataTable::StaticClass());
		Classes.Add(UCompositeDataTable::StaticClass());
		return Classes;
	}
}

// ============================================================================
// get_enum_info
// ============================================================================

bool FGetEnumInfoAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString EnumName;
	if (!GetRequiredString(Params, TEXT("enum_name"), EnumName, OutError))
	{
		return false;
	}
	if (!AssetInspect_ResolveEnum(EnumName))
	{
		OutError = FString::Printf(TEXT("Enum '%s' not found. Pass a User Defined Enum asset name or path (e.g. /Game/Data/E_State) or a native enum name (e.g. ECollisionChannel)."), *EnumName);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetEnumInfoAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const UEnum* Enum = AssetInspect_ResolveEnum(GetOptionalString(Params, TEXT("enum_name")));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Enum->GetName());
	Result->SetStringField(TEXT("path"), Enum->GetPathName());
	Result->SetBoolField(TEXT("is_user_defined"), Enum->IsA<UUserDefinedEnum>());
	Result->SetBoolField(TEXT("is_bitflags"), Enum->HasMetaData(TEXT("Bitflags")));

	AssetInspect_FJsonArray Entries;
	// The last entry is the auto-generated _MAX one.
	const int32 Count = Enum->NumEnums() - 1;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		Entry->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
		Entry->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
		Entry->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
		const FString ToolTip = Enum->GetToolTipTextByIndex(Index).ToString();
		if (!ToolTip.IsEmpty())
		{
			Entry->SetStringField(TEXT("tooltip"), ToolTip);
		}
		if (Enum->HasMetaData(TEXT("Hidden"), Index))
		{
			Entry->SetBoolField(TEXT("hidden"), true);
		}
		Entries.Add(AssetInspect_AsValue(Entry));
	}
	Result->SetNumberField(TEXT("count"), Count);
	Result->SetArrayField(TEXT("entries"), Entries);
	return CreateSuccessResponse(Result);
}

// ============================================================================
// get_curve_info
// ============================================================================

bool FGetCurveInfoAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString CurveName;
	if (!GetRequiredString(Params, TEXT("curve_name"), CurveName, OutError))
	{
		return false;
	}
	if (!FMCPInspectionUtils::FindAsset(CurveName, AssetInspect_CurveClasses()))
	{
		OutError = FString::Printf(TEXT("Curve '%s' not found. Pass a Curve Float/Vector/LinearColor/Table asset name or a full path such as /Game/Curves/C_Speed."), *CurveName);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetCurveInfoAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UObject* Asset = FMCPInspectionUtils::FindAsset(GetOptionalString(Params, TEXT("curve_name")), AssetInspect_CurveClasses());

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Asset->GetName());
	Result->SetStringField(TEXT("path"), Asset->GetPathName());
	Result->SetStringField(TEXT("asset_type"), Asset->GetClass()->GetName());

	if (const UCurveFloat* CurveFloat = Cast<UCurveFloat>(Asset))
	{
		Result->SetObjectField(TEXT("curve"), FMCPInspectionUtils::RichCurveToJson(CurveFloat->FloatCurve));
	}
	else if (const UCurveVector* CurveVector = Cast<UCurveVector>(Asset))
	{
		static const TCHAR* Channels[] = { TEXT("x"), TEXT("y"), TEXT("z") };
		TSharedPtr<FJsonObject> Curves = MakeShared<FJsonObject>();
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Curves->SetObjectField(Channels[Index], FMCPInspectionUtils::RichCurveToJson(CurveVector->FloatCurves[Index]));
		}
		Result->SetObjectField(TEXT("curves"), Curves);
	}
	else if (const UCurveLinearColor* CurveColor = Cast<UCurveLinearColor>(Asset))
	{
		static const TCHAR* Channels[] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
		TSharedPtr<FJsonObject> Curves = MakeShared<FJsonObject>();
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Curves->SetObjectField(Channels[Index], FMCPInspectionUtils::RichCurveToJson(CurveColor->FloatCurves[Index]));
		}
		Result->SetObjectField(TEXT("curves"), Curves);
	}
	else if (UCurveTable* CurveTable = AssetInspect_AsCurveTable(Asset))
	{
		TSharedPtr<FJsonObject> Rows = MakeShared<FJsonObject>();
		if (CurveTable->HasRichCurves())
		{
			Result->SetStringField(TEXT("curve_table_mode"), TEXT("RichCurves"));
			for (const TPair<FName, FRichCurve*>& Row : CurveTable->GetRichCurveRowMap())
			{
				if (Row.Value) Rows->SetObjectField(Row.Key.ToString(), FMCPInspectionUtils::RichCurveToJson(*Row.Value));
			}
		}
		else
		{
			Result->SetStringField(TEXT("curve_table_mode"), TEXT("SimpleCurves"));
			for (const TPair<FName, FSimpleCurve*>& Row : CurveTable->GetSimpleCurveRowMap())
			{
				if (Row.Value) Rows->SetObjectField(Row.Key.ToString(), FMCPInspectionUtils::SimpleCurveToJson(*Row.Value));
			}
		}
		Result->SetNumberField(TEXT("row_count"), Rows->Values.Num());
		Result->SetObjectField(TEXT("rows"), Rows);
	}
	return CreateSuccessResponse(Result);
}

// ============================================================================
// get_data_table_info
// ============================================================================

bool FGetDataTableInfoAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString TableName;
	if (!GetRequiredString(Params, TEXT("table_name"), TableName, OutError))
	{
		return false;
	}
	if (!FMCPInspectionUtils::FindAsset(TableName, AssetInspect_DataTableClasses()))
	{
		OutError = FString::Printf(TEXT("Data Table '%s' not found. Pass the asset name or a full path such as /Game/Data/DT_Items."), *TableName);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FGetDataTableInfoAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UDataTable* Table = Cast<UDataTable>(FMCPInspectionUtils::FindAsset(GetOptionalString(Params, TEXT("table_name")), AssetInspect_DataTableClasses()));
	const bool bIncludeRows = GetOptionalBool(Params, TEXT("include_rows"), true);
	const int32 Offset = FMath::Max(0, static_cast<int32>(GetOptionalNumber(Params, TEXT("offset"), 0)));
	const int32 Limit = FMath::Max(0, static_cast<int32>(GetOptionalNumber(Params, TEXT("limit"), 200)));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Table->GetName());
	Result->SetStringField(TEXT("path"), Table->GetPathName());

	const UScriptStruct* RowStruct = Table->GetRowStruct();
	if (!RowStruct)
	{
		Result->SetStringField(TEXT("row_struct"), TEXT(""));
		Result->SetStringField(TEXT("warning"), TEXT("The Data Table has no row struct assigned."));
		return CreateSuccessResponse(Result);
	}
	Result->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());

	AssetInspect_FJsonArray Columns;
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		TSharedPtr<FJsonObject> Column = MakeShared<FJsonObject>();
		Column->SetStringField(TEXT("name"), RowStruct->GetAuthoredNameForField(*It));
		Column->SetStringField(TEXT("type"), It->GetCPPType());
		Columns.Add(AssetInspect_AsValue(Column));
	}
	Result->SetArrayField(TEXT("columns"), Columns);

	TArray<FName> RowNames = Table->GetRowNames();
	Result->SetNumberField(TEXT("row_count"), RowNames.Num());

	TArray<FString> Requested;
	if (const TArray<TSharedPtr<FJsonValue>>* RequestedValues = GetOptionalArray(Params, TEXT("row_names")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *RequestedValues)
		{
			Requested.Add(Value->AsString());
		}
	}

	if (!bIncludeRows)
	{
		AssetInspect_FJsonArray Names;
		for (const FName& RowName : RowNames)
		{
			Names.Add(MakeShared<FJsonValueString>(RowName.ToString()));
		}
		Result->SetArrayField(TEXT("row_names"), Names);
		return CreateSuccessResponse(Result);
	}

	AssetInspect_FJsonArray Rows;
	int32 Matched = 0;
	for (const FName& RowName : RowNames)
	{
		if (Requested.Num() > 0 && !Requested.Contains(RowName.ToString()))
		{
			continue;
		}
		if (Requested.Num() == 0)
		{
			if (Matched < Offset) { ++Matched; continue; }
			if (Rows.Num() >= Limit) { break; }
		}
		++Matched;

		uint8* const* RowData = Table->GetRowMap().Find(RowName);
		if (!RowData || !*RowData)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), RowName.ToString());
		Row->SetObjectField(TEXT("values"), FMCPInspectionUtils::StructValueToJson(RowStruct, *RowData));
		Rows.Add(AssetInspect_AsValue(Row));
	}
	Result->SetArrayField(TEXT("rows"), Rows);
	if (Requested.Num() == 0 && Offset + Rows.Num() < RowNames.Num())
	{
		Result->SetBoolField(TEXT("truncated"), true);
		Result->SetNumberField(TEXT("next_offset"), Offset + Rows.Num());
	}
	return CreateSuccessResponse(Result);
}
