// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FListWidgetsAction (list_widgets)
 *
 * List the UMG widgets alive in the running PIE world and the buttons / check boxes they contain, so a
 * test can find what to click. Only controls that are currently shown and enabled are listed unless include_hidden.
 *
 * Parameters:
 *   - widget (optional): only user widgets whose name or class contains this text
 *   - include_hidden (optional, default false): also list hidden / collapsed / disabled controls
 * Returns widgets: [{widget, class, in_viewport, buttons:[{name, text, enabled, visible}], checkboxes:[...]}].
 */
class UEBLUEPRINTMCP_API FListWidgetsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("list_widgets"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FClickWidgetAction (click_widget)
 *
 * Click a UMG button in the running PIE world by calling its OnPressed / OnReleased / OnClicked events
 * (what a real click does), or set a check box. Needs no mouse and no window focus.
 *
 * Parameters:
 *   - name (optional): exact control name (as listed by list_widgets)
 *   - text (optional): the control's label (case-insensitive, contains). name or text is required
 *   - widget (optional): restrict to user widgets whose name or class contains this text
 *   - checked (optional): for a check box, the state to set (default: toggle)
 * Fails when no visible, enabled control matches or when several do (the error lists them).
 */
class UEBLUEPRINTMCP_API FClickWidgetAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("click_widget"); }
	virtual bool RequiresSave() const override { return false; }
};

/**
 * FSetMousePositionAction (set_mouse_position)
 *
 * Move the PIE mouse cursor, for games that aim with the cursor (top-down shooters). Combine with
 * inject_input LeftMouseButton to fire.
 *
 * Parameters (one of):
 *   - x, y: viewport pixels
 *   - normalized: [x, y] in 0..1 of the viewport
 *   - actor_name: aim at this PIE actor (name / label / @player_pawn)
 *   - location: [x, y, z] world location to aim at
 *   - player_index (optional, default 0)
 */
class UEBLUEPRINTMCP_API FSetMousePositionAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("set_mouse_position"); }
	virtual bool RequiresSave() const override { return false; }
};
