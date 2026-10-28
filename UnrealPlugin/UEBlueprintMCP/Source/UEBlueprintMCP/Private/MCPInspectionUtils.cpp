// Copyright (c) 2025 zolnoor. All rights reserved.

#include "MCPInspectionUtils.h"
#include "MCPCompat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"

namespace
{
	typedef TArray<TSharedPtr<FJsonValue>> FInspectJsonArray;

	TSharedPtr<FJsonValue> InspectValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	FInspectJsonArray InspectVectorArray(double X, double Y, double Z)
	{
		FInspectJsonArray Array;
		Array.Add(MakeShared<FJsonValueNumber>(X));
		Array.Add(MakeShared<FJsonValueNumber>(Y));
		Array.Add(MakeShared<FJsonValueNumber>(Z));
		return Array;
	}

	const TCHAR* InspectInterpModeToString(ERichCurveInterpMode Mode)
	{
		switch (Mode)
		{
		case RCIM_Linear: return TEXT("Linear");
		case RCIM_Constant: return TEXT("Constant");
		case RCIM_Cubic: return TEXT("Cubic");
		default: return TEXT("None");
		}
	}

	const TCHAR* InspectExtrapToString(ERichCurveExtrapolation Mode)
	{
		switch (Mode)
		{
		case RCCE_Cycle: return TEXT("Cycle");
		case RCCE_CycleWithOffset: return TEXT("CycleWithOffset");
		case RCCE_Oscillate: return TEXT("Oscillate");
		case RCCE_Linear: return TEXT("Linear");
		case RCCE_Constant: return TEXT("Constant");
		default: return TEXT("None");
		}
	}

	/** Stock JSON export, except User Defined Struct / Enum values use their authored names. */
	TSharedPtr<FJsonValue> InspectExportOverride(FProperty* Property, const void* Value)
	{
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct && StructProperty->Struct->IsA<UUserDefinedStruct>())
			{
				return InspectValue(FMCPInspectionUtils::StructValueToJson(StructProperty->Struct, Value));
			}
		}

		const UEnum* Enum = nullptr;
		int64 EnumValue = 0;
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
			EnumValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			Enum = ByteProperty->Enum;
			EnumValue = ByteProperty->GetPropertyValue(Value);
		}
		if (Enum && Enum->IsA<UUserDefinedEnum>())
		{
			return MakeShared<FJsonValueString>(Enum->GetDisplayNameTextByValue(EnumValue).ToString());
		}
		return nullptr;
	}

	const TCHAR* InspectCreationMethodToString(EComponentCreationMethod Method)
	{
		switch (Method)
		{
		case EComponentCreationMethod::Native: return TEXT("Native");
		case EComponentCreationMethod::SimpleConstructionScript: return TEXT("Blueprint");
		case EComponentCreationMethod::UserConstructionScript: return TEXT("ConstructionScript");
		case EComponentCreationMethod::Instance: return TEXT("Instance");
		default: return TEXT("Unknown");
		}
	}

	void InspectWidgetToJson(UWidget* Widget, const UPanelWidget* Parent, int32 Index, int32 Depth, bool bIncludeProperties, FInspectJsonArray& OutWidgets)
	{
		if (!Widget || Depth > 64)
		{
			return;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Widget->GetName());
		Entry->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		if (const UBlueprint* WidgetClassBP = Cast<UBlueprint>(Widget->GetClass()->ClassGeneratedBy))
		{
			Entry->SetStringField(TEXT("blueprint"), WidgetClassBP->GetPathName());
		}
		if (Parent)
		{
			Entry->SetStringField(TEXT("parent"), Parent->GetName());
			Entry->SetNumberField(TEXT("slot_index"), Index);
		}
		Entry->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

		if (Widget->Slot)
		{
			TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
			SlotObject->SetStringField(TEXT("class"), Widget->Slot->GetClass()->GetName());
			if (bIncludeProperties)
			{
				SlotObject->SetObjectField(TEXT("properties"), FMCPInspectionUtils::ChangedProperties(Widget->Slot, Widget->Slot->GetClass()->GetDefaultObject()));
			}
			Entry->SetObjectField(TEXT("slot"), SlotObject);
		}
		if (bIncludeProperties)
		{
			Entry->SetObjectField(TEXT("properties"), FMCPInspectionUtils::ChangedProperties(Widget, Widget->GetClass()->GetDefaultObject()));
		}
		OutWidgets.Add(InspectValue(Entry));

		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ++ChildIndex)
			{
				InspectWidgetToJson(Panel->GetChildAt(ChildIndex), Panel, ChildIndex, Depth + 1, bIncludeProperties, OutWidgets);
			}
		}
	}
}

TSharedPtr<FJsonObject> FMCPInspectionUtils::ChangedProperties(UObject* Object, UObject* Baseline, const UClass* ExcludeOwner)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Object || !Baseline)
	{
		return Result;
	}

	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}
		if (ExcludeOwner && Property->GetOwnerClass() == ExcludeOwner)
		{
			continue;
		}
		if (Property->Identical_InContainer(Object, Baseline))
		{
			continue;
		}
		FString Text;
		Property->ExportText_InContainer(0, Text, Object, nullptr, nullptr, PPF_None);
		Result->SetStringField(Property->GetName(), Text);
	}
	return Result;
}

TSharedPtr<FJsonObject> FMCPInspectionUtils::RichCurveToJson(const FRichCurve& Curve)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	FInspectJsonArray Keys;
	for (const FRichCurveKey& Key : Curve.GetConstRefOfKeys())
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetNumberField(TEXT("time"), Key.Time);
		KeyObj->SetNumberField(TEXT("value"), Key.Value);
		KeyObj->SetStringField(TEXT("interp"), InspectInterpModeToString(Key.InterpMode));
		if (Key.InterpMode == RCIM_Cubic)
		{
			KeyObj->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
			KeyObj->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
		}
		Keys.Add(InspectValue(KeyObj));
	}
	Result->SetArrayField(TEXT("keys"), Keys);
	Result->SetStringField(TEXT("pre_extrapolation"), InspectExtrapToString(Curve.PreInfinityExtrap));
	Result->SetStringField(TEXT("post_extrapolation"), InspectExtrapToString(Curve.PostInfinityExtrap));
	return Result;
}

TSharedPtr<FJsonObject> FMCPInspectionUtils::SimpleCurveToJson(const FSimpleCurve& Curve)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	FInspectJsonArray Keys;
	for (const FSimpleCurveKey& Key : Curve.GetConstRefOfKeys())
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetNumberField(TEXT("time"), Key.Time);
		KeyObj->SetNumberField(TEXT("value"), Key.Value);
		Keys.Add(InspectValue(KeyObj));
	}
	Result->SetArrayField(TEXT("keys"), Keys);
	Result->SetStringField(TEXT("interp"), InspectInterpModeToString(Curve.GetKeyInterpMode()));
	Result->SetStringField(TEXT("pre_extrapolation"), InspectExtrapToString(Curve.PreInfinityExtrap));
	Result->SetStringField(TEXT("post_extrapolation"), InspectExtrapToString(Curve.PostInfinityExtrap));
	return Result;
}

UObject* FMCPInspectionUtils::FindAsset(const FString& NameOrPath, const TArray<UClass*>& Classes)
{
	if (NameOrPath.IsEmpty())
	{
		return nullptr;
	}

	if (NameOrPath.StartsWith(TEXT("/")))
	{
		FString ObjectPath = NameOrPath;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			FString ShortName;
			ObjectPath.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			ObjectPath = ObjectPath + TEXT(".") + ShortName;
		}
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		if (Asset)
		{
			for (const UClass* Class : Classes)
			{
				if (Asset->IsA(Class)) return Asset;
			}
		}
		return nullptr;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	for (UClass* Class : Classes)
	{
		TArray<FAssetData> AssetList;
		MCPCompat::GetAssetsByClass(AssetRegistryModule.Get(), Class, AssetList);
		for (const FAssetData& AssetData : AssetList)
		{
			if (AssetData.AssetName.ToString() == NameOrPath)
			{
				return AssetData.GetAsset();
			}
		}
	}
	return nullptr;
}

TSharedPtr<FJsonValue> FMCPInspectionUtils::PropertyValueToJson(FProperty* Property, const void* ValuePtr)
{
	FJsonObjectConverter::CustomExportCallback Callback;
	Callback.BindStatic(&InspectExportOverride);
	return FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr, 0, 0, &Callback);
}

TSharedPtr<FJsonObject> FMCPInspectionUtils::StructValueToJson(const UStruct* Struct, const void* StructPtr)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}
		TSharedPtr<FJsonValue> Value = PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(StructPtr));
		if (Value.IsValid())
		{
			Result->SetField(Struct->GetAuthoredNameForField(Property), Value);
		}
	}
	return Result;
}

TSharedPtr<FJsonObject> FMCPInspectionUtils::ActorDetailsToJson(AActor* Actor, bool bIncludeProperties)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Actor)
	{
		return Result;
	}

	Result->SetStringField(TEXT("name"), Actor->GetName());
	Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	Result->SetStringField(TEXT("class_path"), Actor->GetClass()->GetPathName());
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy))
	{
		Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	}
	Result->SetStringField(TEXT("path"), Actor->GetPathName());
	if (const ULevel* Level = Actor->GetLevel())
	{
		Result->SetStringField(TEXT("level"), Level->GetOutermost()->GetName());
	}
	const FString Folder = Actor->GetFolderPath().ToString();
	if (!Folder.IsEmpty())
	{
		Result->SetStringField(TEXT("folder"), Folder);
	}

	FInspectJsonArray Tags;
	for (const FName& Tag : Actor->Tags)
	{
		Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Result->SetArrayField(TEXT("tags"), Tags);

	const FVector Location = Actor->GetActorLocation();
	const FRotator Rotation = Actor->GetActorRotation();
	const FVector Scale = Actor->GetActorScale3D();
	Result->SetArrayField(TEXT("location"), InspectVectorArray(Location.X, Location.Y, Location.Z));
	Result->SetArrayField(TEXT("rotation"), InspectVectorArray(Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
	Result->SetArrayField(TEXT("scale"), InspectVectorArray(Scale.X, Scale.Y, Scale.Z));
	Result->SetBoolField(TEXT("hidden_in_editor"), Actor->IsHiddenEd());
	Result->SetBoolField(TEXT("hidden_in_game"), Actor->IsHidden());

	if (const AActor* AttachParent = Actor->GetAttachParentActor())
	{
		Result->SetStringField(TEXT("attach_parent"), AttachParent->GetName());
		const FName Socket = Actor->GetAttachParentSocketName();
		if (!Socket.IsNone())
		{
			Result->SetStringField(TEXT("attach_socket"), Socket.ToString());
		}
	}

	FInspectJsonArray Components;
	TInlineComponentArray<UActorComponent*> ActorComponents;
	Actor->GetComponents(ActorComponents);
	for (UActorComponent* Component : ActorComponents)
	{
		if (!Component) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Component->GetName());
		Entry->SetStringField(TEXT("class"), Component->GetClass()->GetName());
		Entry->SetStringField(TEXT("created_by"), InspectCreationMethodToString(Component->CreationMethod));
		if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
		{
			if (Scene->GetAttachParent())
			{
				Entry->SetStringField(TEXT("attach_parent"), Scene->GetAttachParent()->GetName());
			}
			if (!Scene->GetAttachSocketName().IsNone())
			{
				Entry->SetStringField(TEXT("attach_socket"), Scene->GetAttachSocketName().ToString());
			}
			const FVector RelLocation = Scene->GetRelativeLocation();
			const FRotator RelRotation = Scene->GetRelativeRotation();
			const FVector RelScale = Scene->GetRelativeScale3D();
			Entry->SetArrayField(TEXT("relative_location"), InspectVectorArray(RelLocation.X, RelLocation.Y, RelLocation.Z));
			Entry->SetArrayField(TEXT("relative_rotation"), InspectVectorArray(RelRotation.Pitch, RelRotation.Yaw, RelRotation.Roll));
			Entry->SetArrayField(TEXT("relative_scale"), InspectVectorArray(RelScale.X, RelScale.Y, RelScale.Z));
		}
		if (bIncludeProperties)
		{
			Entry->SetObjectField(TEXT("properties"), ChangedProperties(Component, Component->GetArchetype()));
		}
		Components.Add(InspectValue(Entry));
	}
	Result->SetArrayField(TEXT("components"), Components);

	if (bIncludeProperties)
	{
		// Values set on this placed instance that differ from the class defaults.
		Result->SetObjectField(TEXT("overrides"), ChangedProperties(Actor, Actor->GetArchetype()));
	}
	return Result;
}

TSharedPtr<FJsonObject> FMCPInspectionUtils::WidgetBlueprintToJson(const UWidgetBlueprint* WidgetBlueprint, bool bIncludeProperties)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Widget tree in hierarchy order (parent before children, siblings in slot order).
	FInspectJsonArray Widgets;
	if (WidgetBlueprint->WidgetTree)
	{
		InspectWidgetToJson(WidgetBlueprint->WidgetTree->RootWidget, nullptr, 0, 0, bIncludeProperties, Widgets);
	}
	Result->SetArrayField(TEXT("widgets"), Widgets);

	// Designer property bindings (e.g. TextBlock.Text bound to a function or variable).
	FInspectJsonArray Bindings;
	for (const FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("widget"), Binding.ObjectName);
		Entry->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
		Entry->SetStringField(TEXT("kind"), Binding.Kind == EBindingKind::Function ? TEXT("Function") : TEXT("Property"));
		Entry->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
		if (!Binding.SourceProperty.IsNone())
		{
			Entry->SetStringField(TEXT("source_property"), Binding.SourceProperty.ToString());
		}
		Bindings.Add(InspectValue(Entry));
	}
	Result->SetArrayField(TEXT("property_bindings"), Bindings);

	FInspectJsonArray Animations;
	for (const UWidgetAnimation* Animation : WidgetBlueprint->Animations)
	{
		if (!Animation) continue;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Animation->GetName());
		Entry->SetNumberField(TEXT("start_time"), Animation->GetStartTime());
		Entry->SetNumberField(TEXT("end_time"), Animation->GetEndTime());
		FInspectJsonArray BoundWidgets;
		for (const FWidgetAnimationBinding& Binding : Animation->GetBindings())
		{
			BoundWidgets.Add(MakeShared<FJsonValueString>(Binding.WidgetName.ToString()));
		}
		Entry->SetArrayField(TEXT("bound_widgets"), BoundWidgets);
		Animations.Add(InspectValue(Entry));
	}
	Result->SetArrayField(TEXT("widget_animations"), Animations);

	return Result;
}
