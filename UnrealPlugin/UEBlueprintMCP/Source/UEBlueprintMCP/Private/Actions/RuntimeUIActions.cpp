// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/RuntimeUIActions.h"
#include "Actions/PIEActions.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Widgets/SWidget.h"

namespace
{
	struct FFoundControl
	{
		UUserWidget* Owner = nullptr;
		UWidget* Control = nullptr;
		FString Text;
		bool bEnabled = true;
		bool bVisible = true;
	};

	/** First text found in the control's subtree (a button's label). */
	FString UI_FindText(UWidget* Widget)
	{
		if (UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			return Text->GetText().ToString();
		}
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
			{
				const FString Found = UI_FindText(Panel->GetChildAt(i));
				if (!Found.IsEmpty())
				{
					return Found;
				}
			}
		}
		return FString();
	}

	/** True when the widget and all its Slate ancestors are visible. */
	bool UI_IsShown(UWidget* Widget)
	{
		TSharedPtr<SWidget> Slate = Widget->GetCachedWidget();
		if (!Slate.IsValid())
		{
			return false;
		}
		for (; Slate.IsValid(); Slate = Slate->GetParentWidget())
		{
			if (!Slate->GetVisibility().IsVisible())
			{
				return false;
			}
		}
		return true;
	}

	/** Controls of all live user widgets in the PIE world, optionally filtered by owner name / class. */
	void UI_Collect(const FString& WidgetFilter, bool bIncludeHidden, TArray<FFoundControl>& Out, TArray<UUserWidget*>& OutOwners)
	{
		UWorld* World = GEditor ? static_cast<UWorld*>(GEditor->PlayWorld) : nullptr;
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* Owner = *It;
			if (Owner->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Owner->GetWorld() != World || !Owner->WidgetTree)
			{
				continue;
			}
			if (!WidgetFilter.IsEmpty()
				&& !Owner->GetName().Contains(WidgetFilter) && !Owner->GetClass()->GetName().Contains(WidgetFilter))
			{
				continue;
			}
			const bool bOwnerShown = UI_IsShown(Owner);
			if (!bOwnerShown && !bIncludeHidden)
			{
				continue;
			}
			bool bAny = false;
			Owner->WidgetTree->ForEachWidget([&](UWidget* Widget)
			{
				if (!Cast<UButton>(Widget) && !Cast<UCheckBox>(Widget))
				{
					return;
				}
				FFoundControl Found;
				Found.Owner = Owner;
				Found.Control = Widget;
				Found.Text = UI_FindText(Widget);
				Found.bEnabled = Widget->GetIsEnabled();
				Found.bVisible = bOwnerShown && UI_IsShown(Widget);
				if (bIncludeHidden || (Found.bVisible && Found.bEnabled))
				{
					Out.Add(Found);
					bAny = true;
				}
			});
			if (bAny)
			{
				OutOwners.AddUnique(Owner);
			}
		}
	}

	bool UI_RequirePIE(FString& OutError)
	{
		if (FMCPPIE::IsRunning())
		{
			return true;
		}
		OutError = FMCPPIE::IsStartPending()
			? TEXT("PIE is still starting. Wait until get_pie_status reports 'running'.")
			: TEXT("No PIE session is running. Call start_pie first.");
		return false;
	}

	AActor* UI_FindActor(UWorld* World, APlayerController* PC, const FString& Name)
	{
		if (Name.Equals(TEXT("@player_pawn"), ESearchCase::IgnoreCase))
		{
			return PC->GetPawn();
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase) || It->GetActorLabel().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

// ============================================================================
// list_widgets
// ============================================================================

bool FListWidgetsAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	return UI_RequirePIE(OutError);
}

TSharedPtr<FJsonObject> FListWidgetsAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	TArray<FFoundControl> Controls;
	TArray<UUserWidget*> Owners;
	UI_Collect(GetOptionalString(Params, TEXT("widget")), GetOptionalBool(Params, TEXT("include_hidden"), false), Controls, Owners);

	TArray<TSharedPtr<FJsonValue>> WidgetArray;
	for (UUserWidget* Owner : Owners)
	{
		TArray<TSharedPtr<FJsonValue>> Buttons, Checks;
		for (const FFoundControl& Found : Controls)
		{
			if (Found.Owner != Owner)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Found.Control->GetName());
			if (!Found.Text.IsEmpty())
			{
				Item->SetStringField(TEXT("text"), Found.Text);
			}
			if (!Found.bEnabled)
			{
				Item->SetBoolField(TEXT("enabled"), false);
			}
			if (!Found.bVisible)
			{
				Item->SetBoolField(TEXT("visible"), false);
			}
			if (UCheckBox* Check = Cast<UCheckBox>(Found.Control))
			{
				Item->SetBoolField(TEXT("checked"), Check->IsChecked());
				Checks.Add(MakeShared<FJsonValueObject>(Item));
			}
			else
			{
				Buttons.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("widget"), Owner->GetName());
		Entry->SetStringField(TEXT("class"), Owner->GetClass()->GetName());
		Entry->SetBoolField(TEXT("in_viewport"), Owner->IsInViewport());
		if (Buttons.Num())
		{
			Entry->SetArrayField(TEXT("buttons"), Buttons);
		}
		if (Checks.Num())
		{
			Entry->SetArrayField(TEXT("checkboxes"), Checks);
		}
		WidgetArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("widgets"), WidgetArray);
	Result->SetNumberField(TEXT("count"), WidgetArray.Num());
	return CreateSuccessResponse(Result);
}

// ============================================================================
// click_widget
// ============================================================================

bool FClickWidgetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!UI_RequirePIE(OutError))
	{
		return false;
	}
	if (GetOptionalString(Params, TEXT("name")).IsEmpty() && GetOptionalString(Params, TEXT("text")).IsEmpty())
	{
		OutError = TEXT("name or text is required (see list_widgets)");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FClickWidgetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString Name = GetOptionalString(Params, TEXT("name"));
	const FString Text = GetOptionalString(Params, TEXT("text"));

	TArray<FFoundControl> Controls, Matches;
	TArray<UUserWidget*> Owners;
	UI_Collect(GetOptionalString(Params, TEXT("widget")), false, Controls, Owners);
	for (const FFoundControl& Found : Controls)
	{
		const bool bNameOk = Name.IsEmpty() || Found.Control->GetName().Equals(Name, ESearchCase::IgnoreCase);
		const bool bTextOk = Text.IsEmpty() || Found.Text.Contains(Text, ESearchCase::IgnoreCase);
		if (bNameOk && bTextOk)
		{
			Matches.Add(Found);
		}
	}

	if (Matches.Num() != 1)
	{
		FString List;
		for (const FFoundControl& Found : (Matches.Num() ? Matches : Controls))
		{
			List += FString::Printf(TEXT("%s%s.%s('%s')"), List.IsEmpty() ? TEXT("") : TEXT(", "),
				*Found.Owner->GetName(), *Found.Control->GetName(), *Found.Text);
		}
		return CreateErrorResponse(Matches.Num() == 0
			? FString::Printf(TEXT("No visible enabled control matches. Available: %s"), *List)
			: FString::Printf(TEXT("%d controls match, narrow with widget/name: %s"), Matches.Num(), *List), TEXT("not_found"));
	}

	UWidget* Control = Matches[0].Control;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("widget"), Matches[0].Owner->GetName());
	Result->SetStringField(TEXT("clicked"), Control->GetName());
	if (UCheckBox* Check = Cast<UCheckBox>(Control))
	{
		const bool bState = Params->HasField(TEXT("checked")) ? GetOptionalBool(Params, TEXT("checked")) : !Check->IsChecked();
		Check->SetIsChecked(bState);
		Check->OnCheckStateChanged.Broadcast(bState);
		Result->SetBoolField(TEXT("checked"), bState);
	}
	else if (UButton* Button = Cast<UButton>(Control))
	{
		Button->OnPressed.Broadcast();
		Button->OnReleased.Broadcast();
		Button->OnClicked.Broadcast();
	}
	return CreateSuccessResponse(Result);
}

// ============================================================================
// set_mouse_position
// ============================================================================

bool FSetMousePositionAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!UI_RequirePIE(OutError))
	{
		return false;
	}
	const bool bPixels = Params->HasField(TEXT("x")) && Params->HasField(TEXT("y"));
	if (!bPixels && !Params->HasField(TEXT("normalized")) && !Params->HasField(TEXT("actor_name")) && !Params->HasField(TEXT("location")))
	{
		OutError = TEXT("Give x and y, normalized, actor_name or location.");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FSetMousePositionAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	UWorld* World = GEditor->PlayWorld;
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, static_cast<int32>(GetOptionalNumber(Params, TEXT("player_index"), 0.0)));
	if (!PC)
	{
		return CreateErrorResponse(TEXT("No player controller (game still loading?)"), TEXT("not_found"));
	}

	int32 SizeX = 0, SizeY = 0;
	PC->GetViewportSize(SizeX, SizeY);
	FVector2D Target;

	FVector Location = FVector::ZeroVector;
	bool bProject = false;
	if (Params->HasField(TEXT("actor_name")))
	{
		AActor* Actor = UI_FindActor(World, PC, GetOptionalString(Params, TEXT("actor_name")));
		if (!Actor)
		{
			return CreateErrorResponse(TEXT("Actor not found in the PIE world"), TEXT("not_found"));
		}
		Location = Actor->GetActorLocation();
		bProject = true;
	}
	else if (const TArray<TSharedPtr<FJsonValue>>* Loc = GetOptionalArray(Params, TEXT("location")))
	{
		if (Loc->Num() < 3)
		{
			return CreateErrorResponse(TEXT("location needs [x, y, z]"), TEXT("validation_failed"));
		}
		Location = FVector((*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber());
		bProject = true;
	}

	if (bProject)
	{
		if (!PC->ProjectWorldLocationToScreen(Location, Target, false))
		{
			return CreateErrorResponse(TEXT("The target is behind the camera"), TEXT("validation_failed"));
		}
	}
	else if (const TArray<TSharedPtr<FJsonValue>>* Norm = GetOptionalArray(Params, TEXT("normalized")))
	{
		if (Norm->Num() < 2)
		{
			return CreateErrorResponse(TEXT("normalized needs [x, y]"), TEXT("validation_failed"));
		}
		Target = FVector2D((*Norm)[0]->AsNumber() * SizeX, (*Norm)[1]->AsNumber() * SizeY);
	}
	else
	{
		Target = FVector2D(GetOptionalNumber(Params, TEXT("x")), GetOptionalNumber(Params, TEXT("y")));
	}

	const int32 PixelX = FMath::Clamp(FMath::RoundToInt(Target.X), 0, FMath::Max(SizeX - 1, 0));
	const int32 PixelY = FMath::Clamp(FMath::RoundToInt(Target.Y), 0, FMath::Max(SizeY - 1, 0));
	PC->SetMouseLocation(PixelX, PixelY);

	float ReadX = 0.f, ReadY = 0.f;
	const bool bRead = PC->GetMousePosition(ReadX, ReadY);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("requested"), TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueNumber>(PixelX), MakeShared<FJsonValueNumber>(PixelY)});
	if (bRead)
	{
		Result->SetArrayField(TEXT("mouse"), TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(FMath::RoundToInt(ReadX)), MakeShared<FJsonValueNumber>(FMath::RoundToInt(ReadY))});
	}
	Result->SetArrayField(TEXT("viewport"), TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueNumber>(SizeX), MakeShared<FJsonValueNumber>(SizeY)});
	return CreateSuccessResponse(Result);
}
