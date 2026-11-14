// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorAction.h"

/**
 * FRunScenarioAction (run_scenario)
 *
 * Starts a scripted, repeatable test in a fixed test map: opens PIE on `map` (restarting it if needed),
 * starts the game the way the menu does (controller event StartGame -> real player pawn), spawns mobs through
 * the game's own path (SpawnActor facing the player, then Initialize(stats, spawner, seed), so the summon fade
 * and InitiateBehaviour timer run) and samples them every `interval` game seconds for `duration` seconds.
 * Never blocks: poll scenario_report (the MCP server's run_scenario tool does this and returns the report).
 *
 * Parameters (all optional):
 *   - map: default /Game/ZZ_ClaudeTest/ClaudeTestLevel
 *   - duration (default 6), interval (default 1) in game seconds
 *   - player: { location:[x,y], props:["Prop"...] extra properties to sample }
 *   - mob_stats: object merged over the Spawner defaults (maxHP 150, attackSpeed 50, armor 0, hpVamp 0, movementSpeed 300, damage 15, cdReduc 0, critChance 0)
 *   - mobs: [{ type: zombie|bat|pigly|skullghost|<class path>, count (1), location:[x,y] | distance + angle (deg, around the player),
 *              spacing (cm between mobs of a group, 150), z (spawn height, default 169 like Spawner_BP), seed (default 1), props:["Prop"...] }]
 *   - spawn: [{ name, class, location:[x,y,z], rotation:[p,y,r] }] extra actors (barrel, bookcase...)
 *   - actions: [{ t (game s after start), target: player|controller|<spawn name>|<mob label>, call: "Function", args:{...},
 *                 set:{ Property:value }, read:["Property"...] }]  (struct values as JSON objects merge into the current value)
 *   - keep_open (default false): leave PIE running after the scenario
 *   - log_contains: extra substring to pick from the log (besides warnings and errors)
 */
class UEBLUEPRINTMCP_API FRunScenarioAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override;
	virtual FString GetActionName() const override { return TEXT("run_scenario"); }
	virtual bool RequiresSave() const override { return false; }
	virtual bool BlocksDuringPIE() const override { return false; }
};

/** scenario_report: state of the scenario ("running" with progress, or "done" with the compact text `report`). */
class UEBLUEPRINTMCP_API FScenarioReportAction : public FEditorAction
{
public:
	virtual TSharedPtr<FJsonObject> ExecuteInternal(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context) override;

protected:
	virtual bool Validate(const TSharedPtr<FJsonObject>& Params, FMCPEditorContext& Context, FString& OutError) override { return true; }
	virtual FString GetActionName() const override { return TEXT("scenario_report"); }
	virtual bool RequiresSave() const override { return false; }
	virtual bool BlocksDuringPIE() const override { return false; }
};
