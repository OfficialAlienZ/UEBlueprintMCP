// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FFindInBlueprintsAction
 *
 * Read-only text search across all Blueprint assets (like the editor's global "Find in Blueprints").
 * Blueprints are loaded one by one (sorted by package name) and their graphs are scanned. The scan is
 * bounded by a time budget so the command never blocks the game thread past the server timeout; when the
 * budget (or the match limit) is reached the response carries "next_offset" to continue.
 *
 * Parameters:
 *   - query: Text to find (case-insensitive substring)
 *   - path (optional, default "/Game"): Package path prefix to search (recursive)
 *   - scope (optional): Array of what to search. Default: nodes, pin_defaults, variables, graphs
 *       nodes         node title, comment, function / variable / event / macro / cast target names (+ owners)
 *       pin_defaults  default values of unlinked input pins (string, text, object / class)
 *       pins          pin names and pin type class names (noisy; opt-in)
 *       variables     Blueprint variable names and type class names
 *       graphs        graph names (functions, macros, dispatcher signatures, ...)
 *   - case_sensitive (optional, default false)
 *   - limit (optional, default 50): Stop starting new Blueprints once this many matches are collected
 *   - offset (optional, default 0): Index of the first Blueprint asset to scan (use next_offset to continue)
 *   - time_budget_seconds (optional, default 10, max 18)
 *
 * Node matches carry the same fields as find_blueprint_nodes (node_guid, node_class, node_title, ...) plus
 * "graph" and "matched_on", so they can be passed straight to get_node_pins / find_blueprint_nodes.
 */
class UEBLUEPRINTMCP_API FFindInBlueprintsAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("find_in_blueprints"); }
	virtual bool RequiresSave() const override { return false; }
};
