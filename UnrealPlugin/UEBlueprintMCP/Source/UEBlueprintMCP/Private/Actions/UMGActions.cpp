// Copyright (c) 2025 zolnoor. All rights reserved.

#include "Actions/UMGActions.h"
#include "MCPCommonUtils.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"

// Helper to find widget blueprint in common paths
static UWidgetBlueprint* FindWidgetBlueprintByName(const FString& BlueprintName)
{
	TArray<FString> SearchPaths = {
		FString::Printf(TEXT("/Game/UI/%s"), *BlueprintName),
		FString::Printf(TEXT("/Game/Widgets/%s"), *BlueprintName),
		FString::Printf(TEXT("/Game/%s"), *BlueprintName)
	};
	if (BlueprintName.StartsWith(TEXT("/")))
	{
		SearchPaths.Insert(BlueprintName, 0);
	}

	for (const FString& Path : SearchPaths)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			UWidgetBlueprint* Widget = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(Path));
			if (Widget)
			{
				return Widget;
			}
		}
	}
	// Anywhere else: a name found by the asset registry, or a full path such as /Game/TopDown/Widgets/Menu
	return Cast<UWidgetBlueprint>(FMCPCommonUtils::FindBlueprint(BlueprintName));
}

// =============================================================================
// FCreateUMGWidgetBlueprintAction
// =============================================================================

bool FCreateUMGWidgetBlueprintAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FCreateUMGWidgetBlueprintAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));

	// Get optional path parameter
	FString PackagePath = TEXT("/Game/UI/");
	FString PathParam;
	if (Params->TryGetStringField(TEXT("path"), PathParam))
	{
		PackagePath = PathParam;
		if (!PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath += TEXT("/");
		}
	}

	FString AssetName = BlueprintName;
	FString FullPath = PackagePath + AssetName;

	// Duplicate an existing Widget Blueprint instead of creating an empty one
	FString DuplicateFrom;
	if (Params->TryGetStringField(TEXT("duplicate_from"), DuplicateFrom) && !DuplicateFrom.IsEmpty())
	{
		UWidgetBlueprint* SourceBlueprint = FindWidgetBlueprintByName(DuplicateFrom);
		if (!SourceBlueprint)
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
				TEXT("Source Widget Blueprint '%s' not found in /Game/UI, /Game/Widgets, or /Game"), *DuplicateFrom));
		}
		if (UEditorAssetLibrary::DoesAssetExist(FullPath))
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
				TEXT("Asset '%s' already exists; choose another widget_name or path"), *FullPath));
		}

		UObject* DuplicatedAsset = UEditorAssetLibrary::DuplicateAsset(SourceBlueprint->GetPathName(), FullPath);
		UWidgetBlueprint* DuplicatedBlueprint = Cast<UWidgetBlueprint>(DuplicatedAsset);
		if (!DuplicatedBlueprint)
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
				TEXT("Failed to duplicate '%s' to '%s'"), *SourceBlueprint->GetPathName(), *FullPath));
		}
		UEditorAssetLibrary::SaveAsset(FullPath, false);

		TSharedPtr<FJsonObject> DupResult = MakeShared<FJsonObject>();
		DupResult->SetBoolField(TEXT("success"), true);
		DupResult->SetStringField(TEXT("name"), BlueprintName);
		DupResult->SetStringField(TEXT("path"), FullPath);
		DupResult->SetStringField(TEXT("duplicated_from"), SourceBlueprint->GetPathName());
		return DupResult;
	}

	// Aggressive cleanup: remove any existing widget blueprint
	TArray<FString> PathsToCheck = {
		FullPath,
		TEXT("/Game/Widgets/") + AssetName,
		TEXT("/Game/UI/") + AssetName
	};

	for (const FString& CheckPath : PathsToCheck)
	{
		// Delete from disk first
		if (UEditorAssetLibrary::DoesAssetExist(CheckPath))
		{
			UE_LOG(LogTemp, Log, TEXT("Widget Blueprint exists at '%s', deleting from disk"), *CheckPath);
			UEditorAssetLibrary::DeleteAsset(CheckPath);
		}

		// Clean up from memory
		UPackage* ExistingPackage = FindPackage(nullptr, *CheckPath);
		if (ExistingPackage)
		{
			UBlueprint* ExistingBP = FindObject<UBlueprint>(ExistingPackage, *AssetName);
			if (!ExistingBP)
			{
				ExistingBP = FindObject<UBlueprint>(ExistingPackage, nullptr);
			}

			if (ExistingBP)
			{
				UE_LOG(LogTemp, Log, TEXT("Widget Blueprint '%s' found in memory, cleaning up"), *AssetName);
				FString TempName = FString::Printf(TEXT("%s_OLD_%d"), *AssetName, FMath::Rand());
				ExistingBP->Rename(*TempName, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
				ExistingBP->ClearFlags(RF_Public | RF_Standalone);
				MCPCompat::MarkGarbage(ExistingBP);
			}

			ExistingPackage->ClearFlags(RF_Public | RF_Standalone);
			MCPCompat::MarkGarbage(ExistingPackage);
		}
	}

	// Force garbage collection
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	// Create package
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create package"));
	}

	// Double-check cleanup worked
	if (FindObject<UBlueprint>(Package, *AssetName))
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Failed to clean up existing Widget Blueprint '%s'. Try restarting the editor."), *AssetName));
	}

	// Optional parent: a UserWidget subclass by name or path (e.g. /Script/MyModule.MyWidget)
	UClass* ParentClass = UUserWidget::StaticClass();
	FString ParentParam;
	if (Params->TryGetStringField(TEXT("parent_class"), ParentParam) && !ParentParam.IsEmpty())
	{
		UClass* Found = ParentParam.Contains(TEXT("/")) ? LoadObject<UClass>(nullptr, *ParentParam) : FindFirstObject<UClass>(*ParentParam, EFindFirstObjectOptions::NativeFirst);
		if (!Found || !Found->IsChildOf(UUserWidget::StaticClass()))
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Parent class '%s' not found or not a UserWidget"), *ParentParam));
		}
		ParentClass = Found;
	}

	// Create Widget Blueprint
	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName("CreateUMGWidget")
	);

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(NewBlueprint);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Widget Blueprint"));
	}

	// Add default Canvas Panel
	if (!WidgetBlueprint->WidgetTree->RootWidget)
	{
		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
	}

	// Register and compile
	FAssetRegistryModule::AssetCreated(WidgetBlueprint);
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	// Save immediately
	UEditorAssetLibrary::SaveAsset(FullPath, false);

	UE_LOG(LogTemp, Log, TEXT("Widget Blueprint '%s' created at '%s'"), *BlueprintName, *FullPath);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("name"), BlueprintName);
	ResultObj->SetStringField(TEXT("path"), FullPath);
	return ResultObj;
}

// =============================================================================
// FAddTextBlockToWidgetAction
// =============================================================================

bool FAddTextBlockToWidgetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("text_block_name")))
	{
		OutError = TEXT("Missing 'text_block_name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FAddTextBlockToWidgetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	FString WidgetName = Params->GetStringField(TEXT("text_block_name"));

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found in /Game/UI, /Game/Widgets, or /Game"), *BlueprintName));
	}

	// Optional parameters
	FString InitialText = TEXT("New Text Block");
	Params->TryGetStringField(TEXT("text"), InitialText);

	FVector2D Position(0.0f, 0.0f);
	if (Params->HasField(TEXT("position")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PosArray;
		if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray->Num() >= 2)
		{
			Position.X = (*PosArray)[0]->AsNumber();
			Position.Y = (*PosArray)[1]->AsNumber();
		}
	}

	// Create Text Block
	UTextBlock* TextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *WidgetName);
	if (!TextBlock)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Text Block widget"));
	}

	TextBlock->SetText(FText::FromString(InitialText));

	double FontSize = 0.0;
	if (Params->TryGetNumberField(TEXT("font_size"), FontSize) && FontSize > 0.0)
	{
		FSlateFontInfo Font = TextBlock->GetFont();
		Font.Size = FontSize;
		TextBlock->SetFont(Font);
	}
	const TArray<TSharedPtr<FJsonValue>>* ColorArray;
	if (Params->TryGetArrayField(TEXT("color"), ColorArray) && ColorArray->Num() >= 3)
	{
		const float Alpha = ColorArray->Num() >= 4 ? (*ColorArray)[3]->AsNumber() : 1.f;
		TextBlock->SetColorAndOpacity(FSlateColor(FLinearColor((*ColorArray)[0]->AsNumber(), (*ColorArray)[1]->AsNumber(), (*ColorArray)[2]->AsNumber(), Alpha)));
	}

	// Add to the root canvas, or to the panel named by parent_name (e.g. one page of a widget switcher)
	UPanelWidget* Parent = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->RootWidget);
	FString ParentName;
	if (Params->TryGetStringField(TEXT("parent_name"), ParentName) && !ParentName.IsEmpty())
	{
		Parent = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->FindWidget(FName(*ParentName)));
		if (!Parent)
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Panel '%s' not found"), *ParentName));
		}
	}
	if (!Parent)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Root panel not found"));
	}

	if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Parent))
	{
		UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(TextBlock);
		PanelSlot->SetPosition(Position);
		const TArray<TSharedPtr<FJsonValue>>* SizeArray;
		if (Params->TryGetArrayField(TEXT("size"), SizeArray) && SizeArray->Num() >= 2)
		{
			PanelSlot->SetSize(FVector2D((*SizeArray)[0]->AsNumber(), (*SizeArray)[1]->AsNumber()));
		}
		else
		{
			PanelSlot->SetAutoSize(true);
		}
	}
	else
	{
		Parent->AddChild(TextBlock);
	}
	// Exposed as a variable so the graph can set its text
	TextBlock->bIsVariable = true;

	// Compile and save
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(WidgetBlueprint->GetPathName(), false);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetStringField(TEXT("text"), InitialText);
	return ResultObj;
}

// =============================================================================
// FAddWidgetToWidgetAction
// =============================================================================

bool FAddWidgetToWidgetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")) || !Params->HasField(TEXT("widget_class")))
	{
		OutError = TEXT("Missing 'widget_name' or 'widget_class' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("name")) && !Params->HasField(TEXT("replace")))
	{
		OutError = TEXT("Give 'name' (new widget) or 'replace' (widget to put it in place of)");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FAddWidgetToWidgetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	const FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}
	UWidgetTree* Tree = WidgetBlueprint->WidgetTree;

	// Class: UMG class name (Image, Border...), class path, or a Widget Blueprint name/path
	const FString ClassParam = Params->GetStringField(TEXT("widget_class"));
	UClass* WidgetClass = ClassParam.Contains(TEXT(".")) ? LoadObject<UClass>(nullptr, *ClassParam) : FindFirstObject<UClass>(*ClassParam, EFindFirstObjectOptions::NativeFirst);
	if (!WidgetClass)
	{
		if (UWidgetBlueprint* ClassBlueprint = FindWidgetBlueprintByName(ClassParam))
		{
			WidgetClass = ClassBlueprint->GeneratedClass;
		}
	}
	if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget class '%s' not found"), *ClassParam));
	}

	// Widget to replace: the new one takes its slot (and its name unless 'name' is given)
	FString ReplaceName;
	UWidget* Old = nullptr;
	if (Params->TryGetStringField(TEXT("replace"), ReplaceName) && !ReplaceName.IsEmpty())
	{
		Old = Tree->FindWidget(FName(*ReplaceName));
		if (!Old || !Old->GetParent())
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget '%s' not found or has no parent panel"), *ReplaceName));
		}
	}
	FString NewName = ReplaceName;
	Params->TryGetStringField(TEXT("name"), NewName);
	if (Old && Old->GetFName() == FName(*NewName))
	{
		// Free the name for the new widget
		Old->Rename(*MakeUniqueObjectName(Tree, Old->GetClass(), FName(*(NewName + TEXT("_Replaced")))).ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	}
	else if (Tree->FindWidget(FName(*NewName)))
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("A widget named '%s' already exists"), *NewName));
	}

	UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WidgetClass, FName(*NewName));
	if (!NewWidget)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create the widget"));
	}

	// Properties as Unreal text, e.g. {"bFollowPlayer": "False", "Visibility": "Collapsed"}
	TArray<FString> PropertyErrors;
	const TSharedPtr<FJsonObject>* Properties;
	if (Params->TryGetObjectField(TEXT("properties"), Properties))
	{
		for (const auto& Pair : (*Properties)->Values)
		{
			FProperty* Property = FindFProperty<FProperty>(WidgetClass, *FString(Pair.Key));
			FString Text;
			if (!Property || !Pair.Value->TryGetString(Text) || !Property->ImportText_InContainer(*Text, NewWidget, NewWidget, PPF_None))
			{
				PropertyErrors.Add(FString(Pair.Key));
			}
		}
	}

	if (Old)
	{
		UPanelWidget* OldParent = Old->GetParent();
		OldParent->ReplaceChild(Old, NewWidget);
		Tree->RemoveWidget(Old);
	}
	else
	{
		UPanelWidget* Parent = Cast<UPanelWidget>(Tree->RootWidget);
		FString ParentName;
		if (Params->TryGetStringField(TEXT("parent_name"), ParentName) && !ParentName.IsEmpty())
		{
			Parent = Cast<UPanelWidget>(Tree->FindWidget(FName(*ParentName)));
		}
		if (!Parent)
		{
			return FMCPCommonUtils::CreateErrorResponse(TEXT("Parent panel not found"));
		}
		if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Parent))
		{
			UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(NewWidget);
			const TArray<TSharedPtr<FJsonValue>>* PosArray;
			if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray->Num() >= 2)
			{
				PanelSlot->SetPosition(FVector2D((*PosArray)[0]->AsNumber(), (*PosArray)[1]->AsNumber()));
			}
			const TArray<TSharedPtr<FJsonValue>>* SizeArray;
			if (Params->TryGetArrayField(TEXT("size"), SizeArray) && SizeArray->Num() >= 2)
			{
				PanelSlot->SetSize(FVector2D((*SizeArray)[0]->AsNumber(), (*SizeArray)[1]->AsNumber()));
			}
			// [MinX, MinY, MaxX, MaxY], e.g. [1, 0, 1, 0] = top right corner
			const TArray<TSharedPtr<FJsonValue>>* AnchorArray;
			if (Params->TryGetArrayField(TEXT("anchors"), AnchorArray) && AnchorArray->Num() >= 4)
			{
				PanelSlot->SetAnchors(FAnchors((*AnchorArray)[0]->AsNumber(), (*AnchorArray)[1]->AsNumber(), (*AnchorArray)[2]->AsNumber(), (*AnchorArray)[3]->AsNumber()));
			}
			const TArray<TSharedPtr<FJsonValue>>* AlignmentArray;
			if (Params->TryGetArrayField(TEXT("alignment"), AlignmentArray) && AlignmentArray->Num() >= 2)
			{
				PanelSlot->SetAlignment(FVector2D((*AlignmentArray)[0]->AsNumber(), (*AlignmentArray)[1]->AsNumber()));
			}
		}
		else
		{
			Parent->AddChild(NewWidget);
		}
	}
	bool bIsVariable = true;
	Params->TryGetBoolField(TEXT("is_variable"), bIsVariable);
	NewWidget->bIsVariable = bIsVariable;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(WidgetBlueprint->GetPathName(), false);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("name"), NewWidget->GetName());
	ResultObj->SetStringField(TEXT("class"), WidgetClass->GetName());
	if (Old)
	{
		ResultObj->SetStringField(TEXT("replaced"), ReplaceName);
	}
	if (PropertyErrors.Num() > 0)
	{
		ResultObj->SetStringField(TEXT("properties_not_set"), FString::Join(PropertyErrors, TEXT(", ")));
	}
	return ResultObj;
}

// =============================================================================
// FAddButtonToWidgetAction
// =============================================================================

bool FAddButtonToWidgetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("button_name")))
	{
		OutError = TEXT("Missing 'button_name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FAddButtonToWidgetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	FString WidgetName = Params->GetStringField(TEXT("button_name"));

	FString ButtonText = TEXT("Button");
	Params->TryGetStringField(TEXT("text"), ButtonText);

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found in /Game/UI, /Game/Widgets, or /Game"), *BlueprintName));
	}

	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
	if (!RootCanvas)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Root widget is not a Canvas Panel"));
	}

	// Create Button
	UButton* Button = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), *WidgetName);
	if (!Button)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Button widget"));
	}

	// Create text block for button label
	FString TextBlockName = WidgetName + TEXT("_Text");
	UTextBlock* ButtonTextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *TextBlockName);
	if (ButtonTextBlock)
	{
		ButtonTextBlock->SetText(FText::FromString(ButtonText));
		Button->AddChild(ButtonTextBlock);
	}

	// Add to canvas
	UCanvasPanelSlot* ButtonSlot = RootCanvas->AddChildToCanvas(Button);
	if (ButtonSlot)
	{
		const TArray<TSharedPtr<FJsonValue>>* Position;
		if (Params->TryGetArrayField(TEXT("position"), Position) && Position->Num() >= 2)
		{
			FVector2D Pos((*Position)[0]->AsNumber(), (*Position)[1]->AsNumber());
			ButtonSlot->SetPosition(Pos);
		}

		const TArray<TSharedPtr<FJsonValue>>* Size;
		if (Params->TryGetArrayField(TEXT("size"), Size) && Size->Num() >= 2)
		{
			FVector2D SizeVec((*Size)[0]->AsNumber(), (*Size)[1]->AsNumber());
			ButtonSlot->SetSize(SizeVec);
			ButtonSlot->SetAutoSize(false);
		}
	}

	// Compile and save
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(WidgetBlueprint->GetPathName(), false);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	return ResultObj;
}

// =============================================================================
// Shared helpers for widget-tree edits
// =============================================================================

static UPanelWidget* ResolveParentPanel(UWidgetBlueprint* WidgetBlueprint, const FString& ParentName, FString& OutError)
{
	UWidget* ParentWidget = ParentName.IsEmpty()
		? WidgetBlueprint->WidgetTree->RootWidget.Get()
		: WidgetBlueprint->WidgetTree->FindWidget(FName(*ParentName));
	if (!ParentWidget)
	{
		OutError = ParentName.IsEmpty()
			? TEXT("Widget Blueprint has no root widget")
			: FString::Printf(TEXT("Parent widget '%s' not found in the widget tree"), *ParentName);
		return nullptr;
	}
	UPanelWidget* Panel = Cast<UPanelWidget>(ParentWidget);
	if (!Panel)
	{
		OutError = FString::Printf(TEXT("Parent widget '%s' is not a panel and cannot contain children"), *ParentWidget->GetName());
		return nullptr;
	}
	// UE4 Designer replaces existing content of single-child containers; never do that here
	if (!Panel->CanAddMoreChildren())
	{
		OutError = FString::Printf(TEXT("Parent widget '%s' cannot accept more children"), *Panel->GetName());
		return nullptr;
	}
	return Panel;
}

static void ApplyCanvasSlotLayout(UPanelSlot* Slot, const TSharedPtr<FJsonObject>& Params)
{
	UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot);
	if (!CanvasSlot)
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Position;
	if (Params->TryGetArrayField(TEXT("position"), Position) && Position->Num() >= 2)
	{
		CanvasSlot->SetPosition(FVector2D((*Position)[0]->AsNumber(), (*Position)[1]->AsNumber()));
	}
	const TArray<TSharedPtr<FJsonValue>>* Size;
	if (Params->TryGetArrayField(TEXT("size"), Size) && Size->Num() >= 2)
	{
		CanvasSlot->SetSize(FVector2D((*Size)[0]->AsNumber(), (*Size)[1]->AsNumber()));
		CanvasSlot->SetAutoSize(false);
	}
}

static void CompileAndSaveWidgetBlueprint(UWidgetBlueprint* WidgetBlueprint)
{
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(WidgetBlueprint->GetPathName(), false);
}

// =============================================================================
// FAddImageToWidgetAction
// =============================================================================

bool FAddImageToWidgetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("image_name")))
	{
		OutError = TEXT("Missing 'image_name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FAddImageToWidgetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	FString ImageName = Params->GetStringField(TEXT("image_name"));

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found in /Game/UI, /Game/Widgets, or /Game"), *BlueprintName));
	}
	if (WidgetBlueprint->WidgetTree->FindWidget(FName(*ImageName)))
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("A widget named '%s' already exists in '%s'"), *ImageName, *BlueprintName));
	}

	FString Error;
	UPanelWidget* Parent = ResolveParentPanel(WidgetBlueprint, Params->HasField(TEXT("parent_widget")) ? Params->GetStringField(TEXT("parent_widget")) : FString(), Error);
	if (!Parent)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error);
	}

	UImage* Image = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), *ImageName);
	if (!Image)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Image widget"));
	}

	FString BrushPath;
	if (Params->TryGetStringField(TEXT("brush_path"), BrushPath) && !BrushPath.IsEmpty())
	{
		UObject* BrushAsset = UEditorAssetLibrary::LoadAsset(BrushPath);
		if (UMaterialInterface* Material = Cast<UMaterialInterface>(BrushAsset))
		{
			Image->SetBrushFromMaterial(Material);
		}
		else if (UTexture2D* Texture = Cast<UTexture2D>(BrushAsset))
		{
			Image->SetBrushFromTexture(Texture, false);
		}
		else
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
				TEXT("brush_path '%s' is not a Texture2D or Material asset"), *BrushPath));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Color;
	if (Params->TryGetArrayField(TEXT("color"), Color) && Color->Num() >= 3)
	{
		const float A = Color->Num() >= 4 ? (*Color)[3]->AsNumber() : 1.0f;
		Image->SetColorAndOpacity(FLinearColor((*Color)[0]->AsNumber(), (*Color)[1]->AsNumber(), (*Color)[2]->AsNumber(), A));
	}

	UPanelSlot* Slot = Parent->AddChild(Image);
	if (!Slot)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add Image to parent panel"));
	}
	ApplyCanvasSlotLayout(Slot, Params);

	CompileAndSaveWidgetBlueprint(WidgetBlueprint);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), ImageName);
	ResultObj->SetStringField(TEXT("parent"), Parent->GetName());
	return ResultObj;
}

// =============================================================================
// FDuplicateWidgetAction
// =============================================================================

bool FDuplicateWidgetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	for (const TCHAR* Field : { TEXT("widget_name"), TEXT("source_widget"), TEXT("new_name") })
	{
		if (!Params->HasField(Field))
		{
			OutError = FString::Printf(TEXT("Missing '%s' parameter"), Field);
			return false;
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FDuplicateWidgetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	FString SourceName = Params->GetStringField(TEXT("source_widget"));
	FString NewName = Params->GetStringField(TEXT("new_name"));

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found in /Game/UI, /Game/Widgets, or /Game"), *BlueprintName));
	}

	UWidgetTree* Tree = WidgetBlueprint->WidgetTree;
	UWidget* Source = Tree->FindWidget(FName(*SourceName));
	if (!Source)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget '%s' not found in '%s'"), *SourceName, *BlueprintName));
	}
	if (Tree->FindWidget(FName(*NewName)))
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("A widget named '%s' already exists in '%s'"), *NewName, *BlueprintName));
	}
	if (const UPanelWidget* SourcePanel = Cast<UPanelWidget>(Source))
	{
		if (SourcePanel->GetChildrenCount() > 0)
		{
			return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
				TEXT("Widget '%s' has children; only leaf widgets (Image, TextBlock, ProgressBar, empty panels...) can be duplicated"), *SourceName));
		}
	}

	FString Error;
	UPanelWidget* Parent = nullptr;
	if (Params->HasField(TEXT("parent_widget")))
	{
		Parent = ResolveParentPanel(WidgetBlueprint, Params->GetStringField(TEXT("parent_widget")), Error);
	}
	else if (Source->GetParent())
	{
		Parent = ResolveParentPanel(WidgetBlueprint, Source->GetParent()->GetName(), Error);
	}
	else
	{
		Error = TEXT("Source widget has no parent (it is the root); pass 'parent_widget'");
	}
	if (!Parent)
	{
		return FMCPCommonUtils::CreateErrorResponse(Error);
	}

	UWidget* NewWidget = Cast<UWidget>(StaticDuplicateObject(Source, Tree, FName(*NewName)));
	if (!NewWidget)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to duplicate widget"));
	}
	NewWidget->SetFlags(RF_Transactional);
	NewWidget->Slot = nullptr; // duplicated pointer still references the source's slot

	UPanelSlot* NewSlot = Parent->AddChild(NewWidget);
	if (!NewSlot)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to add duplicate to parent panel"));
	}

	// Copy slot layout from the source (skip the owner/content back-references)
	UPanelSlot* SourceSlot = Source->Slot;
	if (SourceSlot && SourceSlot->GetClass() == NewSlot->GetClass())
	{
		for (TFieldIterator<FProperty> It(SourceSlot->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->GetFName() == TEXT("Parent") || Property->GetFName() == TEXT("Content")
				|| Property->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}
			Property->CopyCompleteValue_InContainer(NewSlot, SourceSlot);
		}
	}
	ApplyCanvasSlotLayout(NewSlot, Params);

	CompileAndSaveWidgetBlueprint(WidgetBlueprint);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), NewName);
	ResultObj->SetStringField(TEXT("class"), NewWidget->GetClass()->GetName());
	ResultObj->SetStringField(TEXT("parent"), Parent->GetName());
	return ResultObj;
}

// =============================================================================
// FRemoveWidgetAction
// =============================================================================

bool FRemoveWidgetAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("target_widget")))
	{
		OutError = TEXT("Missing 'target_widget' parameter");
		return false;
	}
	return true;
}

static void CollectWidgetAndDescendants(UWidget* Widget, TArray<UWidget*>& Out)
{
	Out.Add(Widget);
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			CollectWidgetAndDescendants(Panel->GetChildAt(i), Out);
		}
	}
}

TSharedPtr<FJsonObject> FRemoveWidgetAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	FString TargetName = Params->GetStringField(TEXT("target_widget"));

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found in /Game/UI, /Game/Widgets, or /Game"), *BlueprintName));
	}

	UWidgetTree* Tree = WidgetBlueprint->WidgetTree;
	UWidget* Target = Tree->FindWidget(FName(*TargetName));
	if (!Target)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget '%s' not found in '%s'"), *TargetName, *BlueprintName));
	}
	if (Target == Tree->RootWidget)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("The root widget cannot be removed; remove its children instead"));
	}

	TArray<UWidget*> ToRemove;
	CollectWidgetAndDescendants(Target, ToRemove);

	TArray<TSharedPtr<FJsonValue>> RemovedNames;
	for (UWidget* Widget : ToRemove)
	{
		RemovedNames.Add(MakeShared<FJsonValueString>(Widget->GetName()));
	}

	// Detach from the tree first, then move the removed widgets out so their names can be reused
	Tree->Modify();
	Tree->RemoveWidget(Target);
	for (UWidget* Widget : ToRemove)
	{
		Widget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
		Widget->ClearFlags(RF_Public | RF_Standalone);
	}

	CompileAndSaveWidgetBlueprint(WidgetBlueprint);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetArrayField(TEXT("removed"), RemovedNames);
	return ResultObj;
}

// =============================================================================
// FBindWidgetEventAction
// =============================================================================

bool FBindWidgetEventAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("widget_component_name")))
	{
		OutError = TEXT("Missing 'widget_component_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("event_name")))
	{
		OutError = TEXT("Missing 'event_name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FBindWidgetEventAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	FString WidgetComponentName = Params->GetStringField(TEXT("widget_component_name"));
	FString EventName = Params->GetStringField(TEXT("event_name"));

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	// Find the widget in the WidgetTree
	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetComponentName);
	if (!Widget)
	{
		TArray<FString> AvailableWidgets;
		WidgetBlueprint->WidgetTree->ForEachWidget([&AvailableWidgets](UWidget* W) {
			if (W) AvailableWidgets.Add(W->GetName());
		});
		FString WidgetList = FString::Join(AvailableWidgets, TEXT(", "));
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget '%s' not found. Available: %s"), *WidgetComponentName, *WidgetList));
	}

	// Verify delegate exists
	FMulticastDelegateProperty* DelegateProp = nullptr;
	for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass()); It; ++It)
	{
		if (It->GetFName() == FName(*EventName))
		{
			DelegateProp = *It;
			break;
		}
	}

	if (!DelegateProp)
	{
		TArray<FString> AvailableDelegates;
		for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass()); It; ++It)
		{
			AvailableDelegates.Add(It->GetName());
		}
		FString DelegateList = FString::Join(AvailableDelegates, TEXT(", "));
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Delegate '%s' not found. Available: %s"), *EventName, *DelegateList));
	}

	// Get event graph
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(WidgetBlueprint);
	if (!EventGraph)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to find event graph"));
	}

	// Check if Component Bound Event node already exists for this widget/delegate combo
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		UK2Node_ComponentBoundEvent* ExistingEvent = Cast<UK2Node_ComponentBoundEvent>(Node);
		if (ExistingEvent &&
			ExistingEvent->ComponentPropertyName == FName(*WidgetComponentName) &&
			ExistingEvent->DelegatePropertyName == DelegateProp->GetFName())
		{
			// Already exists - return the existing node
			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetBoolField(TEXT("already_exists"), true);
			ResultObj->SetStringField(TEXT("widget_name"), WidgetComponentName);
			ResultObj->SetStringField(TEXT("event_name"), EventName);
			ResultObj->SetStringField(TEXT("node_id"), ExistingEvent->NodeGuid.ToString());
			return ResultObj;
		}
	}

	// Calculate position for new node (below existing nodes)
	float MaxY = 0.0f;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		MaxY = FMath::Max(MaxY, (float)Node->NodePosY);
	}

	// Create Component Bound Event node - this is the proper way to handle widget events
	UK2Node_ComponentBoundEvent* EventNode = NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
	EventNode->ComponentPropertyName = FName(*WidgetComponentName);
	EventNode->DelegatePropertyName = DelegateProp->GetFName();
	EventNode->DelegateOwnerClass = Widget->GetClass();

	EventGraph->AddNode(EventNode, false, false);
	EventNode->CreateNewGuid();
	EventNode->NodePosX = 200;
	EventNode->NodePosY = (int32)(MaxY + 200);
	EventNode->AllocateDefaultPins();

	UE_LOG(LogTemp, Log, TEXT("Created Component Bound Event: %s.%s"), *WidgetComponentName, *EventName);

	// Compile and save
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(WidgetBlueprint->GetPathName(), false);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetComponentName);
	ResultObj->SetStringField(TEXT("event_name"), EventName);
	ResultObj->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
	return ResultObj;
}

// =============================================================================
// FAddWidgetToViewportAction
// =============================================================================

bool FAddWidgetToViewportAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FAddWidgetToViewportAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	int32 ZOrder = 0;
	Params->TryGetNumberField(TEXT("z_order"), ZOrder);

	UClass* WidgetClass = WidgetBlueprint->GeneratedClass;
	if (!WidgetClass)
	{
		return FMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get widget class"));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
	ResultObj->SetStringField(TEXT("class_path"), WidgetClass->GetPathName());
	ResultObj->SetNumberField(TEXT("z_order"), ZOrder);
	ResultObj->SetStringField(TEXT("note"), TEXT("Widget class ready. Use CreateWidget and AddToViewport nodes in Blueprint."));
	return ResultObj;
}

// =============================================================================
// FSetTextBlockBindingAction
// =============================================================================

bool FSetTextBlockBindingAction::Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError)
{
	if (!Params->HasField(TEXT("widget_name")))
	{
		OutError = TEXT("Missing 'widget_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("text_block_name")))
	{
		OutError = TEXT("Missing 'text_block_name' parameter");
		return false;
	}
	if (!Params->HasField(TEXT("binding_property")))
	{
		OutError = TEXT("Missing 'binding_property' parameter");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FSetTextBlockBindingAction::ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context)
{
	FString BlueprintName = Params->GetStringField(TEXT("widget_name"));
	FString WidgetName = Params->GetStringField(TEXT("text_block_name"));
	FString BindingName = Params->GetStringField(TEXT("binding_property"));

	UWidgetBlueprint* WidgetBlueprint = FindWidgetBlueprintByName(BlueprintName);
	if (!WidgetBlueprint)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	// Create variable for binding
	FBlueprintEditorUtils::AddMemberVariable(
		WidgetBlueprint,
		FName(*BindingName),
		FEdGraphPinType(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType())
	);

	// Find the TextBlock widget
	UTextBlock* TextBlock = Cast<UTextBlock>(WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)));
	if (!TextBlock)
	{
		return FMCPCommonUtils::CreateErrorResponse(FString::Printf(
			TEXT("TextBlock '%s' not found"), *WidgetName));
	}

	// Create binding function
	const FString FunctionName = FString::Printf(TEXT("Get%s"), *BindingName);
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		WidgetBlueprint,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (FuncGraph)
	{
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(WidgetBlueprint, FuncGraph, false, nullptr);

		// Create entry node
		UK2Node_FunctionEntry* EntryNode = NewObject<UK2Node_FunctionEntry>(FuncGraph);
		FuncGraph->AddNode(EntryNode, false, false);
		EntryNode->NodePosX = 0;
		EntryNode->NodePosY = 0;
		EntryNode->FunctionReference.SetExternalMember(FName(*FunctionName), WidgetBlueprint->GeneratedClass);
		EntryNode->AllocateDefaultPins();

		// Create get variable node
		UK2Node_VariableGet* GetVarNode = NewObject<UK2Node_VariableGet>(FuncGraph);
		GetVarNode->VariableReference.SetSelfMember(FName(*BindingName));
		FuncGraph->AddNode(GetVarNode, false, false);
		GetVarNode->NodePosX = 200;
		GetVarNode->NodePosY = 0;
		GetVarNode->AllocateDefaultPins();

		// Connect nodes
		UEdGraphPin* EntryThenPin = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* GetVarOutPin = GetVarNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
		if (EntryThenPin && GetVarOutPin)
		{
			EntryThenPin->MakeLinkTo(GetVarOutPin);
		}
	}

	// Compile
	WidgetBlueprint->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("binding_name"), BindingName);
	return ResultObj;
}
