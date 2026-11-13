// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/DataTableActions.h"
#include "MCPInspectionUtils.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "JsonObjectConverter.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Authored (editor) column name first, then the internal property name; case-insensitive. */
	FProperty* DT_FindField(const UStruct* Struct, const FString& Name)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (Struct->GetAuthoredNameForField(*It).Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	bool DT_FindEnumValue(const UEnum* Enum, const FString& Text, int64& OutValue)
	{
		int32 Index = Enum->GetIndexByNameString(Text);
		if (Index == INDEX_NONE)
		{
			// User Defined Enums: match the display name authored in the editor.
			for (int32 EnumIndex = 0; EnumIndex < Enum->NumEnums() - 1; ++EnumIndex)
			{
				if (Enum->GetDisplayNameTextByIndex(EnumIndex).ToString().Equals(Text, ESearchCase::IgnoreCase))
				{
					Index = EnumIndex;
					break;
				}
			}
		}
		if (Index == INDEX_NONE)
		{
			return false;
		}
		OutValue = Enum->GetValueByIndex(Index);
		return true;
	}

	FString DT_EnumChoices(const UEnum* Enum)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			Names.Add(Enum->GetDisplayNameTextByIndex(Index).ToString());
		}
		return FString::Join(Names, TEXT(", "));
	}

	/**
	 * Write a JSON value into a property. Accepts the same shapes get_data_table_info returns: authored column
	 * names for structs (partial objects keep the other fields), enum names or display names, arrays.
	 * Text gets a localization key in the owner's package, like text typed in the editor.
	 */
	bool DT_SetValue(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, UObject* Owner, const FString& Path, FString& OutError)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			OutError = FString::Printf(TEXT("Column '%s': null is not a valid value."), *Path);
			return false;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (Value->Type == EJson::Object)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->AsObject()->Values)
				{
					FProperty* Inner = DT_FindField(StructProperty->Struct, Pair.Key);
					if (!Inner)
					{
						OutError = FString::Printf(TEXT("Column '%s': struct has no field '%s'."), *Path, *Pair.Key);
						return false;
					}
					if (!DT_SetValue(Inner, Inner->ContainerPtrToValuePtr<void>(ValuePtr), Pair.Value, Owner, Path + TEXT(".") + Pair.Key, OutError))
					{
						return false;
					}
				}
				return true;
			}
		}
		else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
				Helper.EmptyValues();
				Helper.AddValues(Items.Num());
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					if (!DT_SetValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Items[Index], Owner, FString::Printf(TEXT("%s[%d]"), *Path, Index), OutError))
					{
						return false;
					}
				}
				return true;
			}
		}
		else if (CastField<FTextProperty>(Property))
		{
			if (Value->Type == EJson::String)
			{
				FText* Text = static_cast<FText*>(ValuePtr);
				const FString NewString = Value->AsString();
				if (NewString.IsEmpty())
				{
					*Text = FText::GetEmpty();
					return true;
				}

				const FString Namespace = TextNamespaceUtil::EnsurePackageNamespace(Owner);
				FString Key = FTextInspector::GetKey(*Text).Get(FString());
				const bool bReuseKey = !Key.IsEmpty() && FTextInspector::GetNamespace(*Text).Get(FString()) == Namespace;
				if (!bReuseKey)
				{
					Key = FGuid::NewGuid().ToString();
				}
				*Text = FText::ChangeKey(FTextKey(Namespace), FTextKey(Key), FText::FromString(NewString));
				return true;
			}
		}
		else
		{
			const UEnum* Enum = nullptr;
			FNumericProperty* Numeric = nullptr;
			if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				Enum = EnumProperty->GetEnum();
				Numeric = EnumProperty->GetUnderlyingProperty();
			}
			else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				Enum = ByteProperty->Enum;
				Numeric = ByteProperty;
			}

			if (Enum && Numeric && Value->Type == EJson::String)
			{
				int64 EnumValue = 0;
				if (!DT_FindEnumValue(Enum, Value->AsString(), EnumValue))
				{
					OutError = FString::Printf(TEXT("Column '%s': '%s' is not a value of enum %s. Valid: %s"),
						*Path, *Value->AsString(), *Enum->GetName(), *DT_EnumChoices(Enum));
					return false;
				}
				Numeric->SetIntPropertyValue(ValuePtr, EnumValue);
				return true;
			}
		}

		if (!FJsonObjectConverter::JsonValueToUProperty(Value, Property, ValuePtr, 0, 0))
		{
			OutError = FString::Printf(TEXT("Column '%s': value cannot be converted to type %s."), *Path, *Property->GetCPPType());
			return false;
		}
		return true;
	}

	TArray<UDataTable*> DT_GetParentTables(UCompositeDataTable* Composite)
	{
		TArray<UDataTable*> Result;
		if (const FArrayProperty* ParentsProperty = FindFProperty<FArrayProperty>(UCompositeDataTable::StaticClass(), TEXT("ParentTables")))
		{
			FScriptArrayHelper Helper(ParentsProperty, ParentsProperty->ContainerPtrToValuePtr<void>(Composite));
			const FObjectProperty* Inner = CastField<FObjectProperty>(ParentsProperty->Inner);
			for (int32 Index = 0; Inner && Index < Helper.Num(); ++Index)
			{
				if (UDataTable* Parent = Cast<UDataTable>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index))))
				{
					Result.Add(Parent);
				}
			}
		}
		return Result;
	}

	/** The (non-composite) table that stores RowName; for composites the topmost parent that has it (parents later in the list override earlier ones). */
	UDataTable* DT_FindRowOwner(UDataTable* Table, FName RowName, int32 Depth = 0)
	{
		UCompositeDataTable* Composite = Cast<UCompositeDataTable>(Table);
		if (!Composite)
		{
			return Table->GetRowMap().Contains(RowName) ? Table : nullptr;
		}
		if (Depth > 16)
		{
			return nullptr;
		}
		const TArray<UDataTable*> Parents = DT_GetParentTables(Composite);
		for (int32 Index = Parents.Num() - 1; Index >= 0; --Index)
		{
			if (UDataTable* Owner = DT_FindRowOwner(Parents[Index], RowName, Depth + 1))
			{
				return Owner;
			}
		}
		return nullptr;
	}

	/** Resolve table_name/row_name/create_if_missing to the table that must be modified. */
	bool DT_Resolve(const FString& TableName, const FString& RowNameString, bool bCreate, UDataTable*& OutRequested, UDataTable*& OutTarget, FString& OutError)
	{
		TArray<UClass*> Classes;
		Classes.Add(UDataTable::StaticClass());
		Classes.Add(UCompositeDataTable::StaticClass());
		OutRequested = Cast<UDataTable>(FMCPInspectionUtils::FindAsset(TableName, Classes));
		if (!OutRequested)
		{
			OutError = FString::Printf(TEXT("Data Table '%s' not found. Pass the asset name or a full path such as /Game/Data/DT_Items."), *TableName);
			return false;
		}
		if (!OutRequested->GetRowStruct())
		{
			OutError = FString::Printf(TEXT("Data Table '%s' has no row struct assigned."), *TableName);
			return false;
		}

		const FName RowName(*RowNameString);
		if (UCompositeDataTable* Composite = Cast<UCompositeDataTable>(OutRequested))
		{
			OutTarget = DT_FindRowOwner(Composite, RowName);
			if (!OutTarget)
			{
				TArray<FString> ParentNames;
				for (const UDataTable* Parent : DT_GetParentTables(Composite))
				{
					ParentNames.Add(Parent->GetPathName());
				}
				OutError = FString::Printf(TEXT("'%s' is a Composite Data Table and has no row '%s'. Rows are stored in its parent tables (%s); target one of them to add a row."),
					*TableName, *RowNameString, *FString::Join(ParentNames, TEXT(", ")));
				return false;
			}
			return true;
		}

		OutTarget = OutRequested;
		if (!bCreate && !OutTarget->GetRowMap().Contains(RowName))
		{
			OutError = FString::Printf(TEXT("Row '%s' not found in '%s'. Use get_data_table_info with include_rows=false to list the row names, or pass create_if_missing=true."), *RowNameString, *TableName);
			return false;
		}
		return true;
	}
}

bool FSetDataTableRowAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	FString TableName, RowName;
	if (!GetRequiredString(Params, TEXT("table_name"), TableName, OutError) ||
		!GetRequiredString(Params, TEXT("row_name"), RowName, OutError))
	{
		return false;
	}

	UDataTable* Requested = nullptr;
	UDataTable* Target = nullptr;
	if (!DT_Resolve(TableName, RowName, GetOptionalBool(Params, TEXT("create_if_missing")) && !GetOptionalBool(Params, TEXT("delete_row")), Requested, Target, OutError))
	{
		return false;
	}

	if (GetOptionalBool(Params, TEXT("delete_row")))
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* Values = nullptr;
	if (!Params->TryGetObjectField(TEXT("values"), Values) || !Values || (*Values)->Values.Num() == 0)
	{
		OutError = TEXT("'values' is required: an object of column name -> value (see get_data_table_info for the column names).");
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
	{
		if (!DT_FindField(Target->GetRowStruct(), Pair.Key))
		{
			TArray<FString> Columns;
			for (TFieldIterator<FProperty> It(Target->GetRowStruct()); It; ++It)
			{
				Columns.Add(Target->GetRowStruct()->GetAuthoredNameForField(*It));
			}
			OutError = FString::Printf(TEXT("Column '%s' does not exist in row struct %s. Columns: %s"),
				*Pair.Key, *Target->GetRowStruct()->GetName(), *FString::Join(Columns, TEXT(", ")));
			return false;
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FSetDataTableRowAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString TableName = GetOptionalString(Params, TEXT("table_name"));
	const FString RowNameString = GetOptionalString(Params, TEXT("row_name"));
	const FName RowName(*RowNameString);
	const bool bDelete = GetOptionalBool(Params, TEXT("delete_row"));

	UDataTable* Requested = nullptr;
	UDataTable* Table = nullptr;
	FString Error;
	if (!DT_Resolve(TableName, RowNameString, GetOptionalBool(Params, TEXT("create_if_missing")) && !bDelete, Requested, Table, Error))
	{
		return CreateErrorResponse(Error, TEXT("not_found"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("table"), Requested->GetPathName());
	Result->SetStringField(TEXT("edited_table"), Table->GetPathName());
	Result->SetStringField(TEXT("row_name"), RowNameString);

	if (bDelete)
	{
		Table->Modify();
		Table->RemoveRow(RowName);
		Table->HandleDataTableChanged();
		Table->MarkPackageDirty();
		Result->SetBoolField(TEXT("deleted"), true);
		return CreateSuccessResponse(Result);
	}

	const UScriptStruct* RowStruct = Table->GetRowStruct();
	uint8* const* ExistingRow = Table->GetRowMap().Find(RowName);
	const bool bExists = ExistingRow && *ExistingRow;

	// Work on a copy so a failing column leaves the row untouched.
	FStructOnScope Working(RowStruct);
	if (bExists)
	{
		RowStruct->CopyScriptStruct(Working.GetStructMemory(), *ExistingRow);
	}

	const TSharedPtr<FJsonObject>* Values = nullptr;
	Params->TryGetObjectField(TEXT("values"), Values);
	TArray<FString> Changed;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
	{
		FProperty* Property = DT_FindField(RowStruct, Pair.Key);
		if (!DT_SetValue(Property, Property->ContainerPtrToValuePtr<void>(Working.GetStructMemory()), Pair.Value, Table, Pair.Key, Error))
		{
			return CreateErrorResponse(Error, TEXT("validation_failed"));
		}
		Changed.Add(RowStruct->GetAuthoredNameForField(Property));
	}

	Table->Modify();
	if (bExists)
	{
		// Copy in place: keeps the row's memory (and map order) stable.
		RowStruct->CopyScriptStruct(*ExistingRow, Working.GetStructMemory());
	}
	else
	{
		// AddRow copies RowStruct->GetStructureSize() bytes out of the source, so a full-size buffer is passed through the FTableRowBase reference.
		Table->AddRow(RowName, *reinterpret_cast<const FTableRowBase*>(Working.GetStructMemory()));
	}
	Table->HandleDataTableChanged(RowName);
	Table->MarkPackageDirty();

	Result->SetBoolField(TEXT("created"), !bExists);
	TArray<TSharedPtr<FJsonValue>> ChangedValues;
	for (const FString& Column : Changed)
	{
		ChangedValues.Add(MakeShared<FJsonValueString>(Column));
	}
	Result->SetArrayField(TEXT("changed_columns"), ChangedValues);
	if (uint8* const* NewRow = Table->GetRowMap().Find(RowName))
	{
		Result->SetObjectField(TEXT("values"), FMCPInspectionUtils::StructValueToJson(RowStruct, *NewRow));
	}
	return CreateSuccessResponse(Result);
}
