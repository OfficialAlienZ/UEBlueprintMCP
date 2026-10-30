---
name: unreal-playtest
description: "Use this skill to play-test an Unreal Engine game through UEBlueprintMCP: start Play-In-Editor, drive the game (input, function calls, state changes), check expected behaviour, capture screenshots and logs, run Automation / Functional Tests, and report bugs. Use when the user asks to test, play, QA, verify, reproduce or find bugs in their game, or to run test scenarios."
---

# Autonomous play-testing with UEBlueprintMCP

Works with Blueprint-only projects. Tool reference: `docs/SKILL.md` ("Play-In-Editor & Runtime Feedback").

## Loop

1. **Prepare**: open the map to test (it must be the editor's current level), `compile_blueprint` anything changed, `get_context` / `get_output_log` for existing errors.
2. **Start**: `start_pie` (`clear_log: true`). Note the returned `log_cursor`. `wait_for_condition` with `{type: "actor_exists", actor_name: "@player_pawn"}` (or poll `get_pie_status` until `running`).
3. **Observe**: `get_actors_in_level` / `get_actor_properties` / `get_object_properties` with `world: "pie"`. Learn the Blueprint API first with `get_blueprint_info` (variables, functions, events).
4. **Act**: `inject_input` (movement, jump, fire), `call_actor_function` (abilities, UI-less triggers, getters), `set_actor_property` / `set_actor_transform` with `world: "pie"` (teleport, set health, set up a scenario).
5. **Verify**: `wait_for_condition` for the expected outcome with `abort_conditions` for failure states; `check_condition` for instant assertions; `take_screenshot` for visual state.
6. **Collect**: `get_output_log` with `since_id` = the cursor from step 2, `min_verbosity: "Warning"`. Any Error line or Blueprint runtime error ("Accessed None", "Blueprint Runtime Error") is a finding even if the scenario passed.
7. **Stop**: `stop_pie`. Edits to assets are rejected (`pie_active`) until PIE is stopped.

## Scenario template

```
Scenario: <name>
Setup:    teleport/set state via set_actor_* (world=pie), or play to the point
Steps:    inject_input / call_actor_function ...
Expect:   wait_for_condition { actor_property ... } with timeout
Abort on: player dead, log_contains "Error"
Evidence: take_screenshot (label=<scenario>), log excerpt
```

Cover: the happy path, boundaries (zero/max values, empty inventories), repeated actions, interrupting an action midway, invalid order of actions, and respawn/restart (`stop_pie` then `start_pie` and re-check state is reset).

## Tips

- Prefer `wait_for_condition` over fixed delays. Timed `inject_input` runs in game time and returns immediately; wait with `{type: "game_time", elapsed: N}`.
- `inject_input` goes through the player controller, not Slate: it cannot click UMG buttons. Call the button's handler or the owning Blueprint's function with `call_actor_function` and verify the widget state with `get_object_properties`.
- `call_actor_function` zero-initialises omitted arguments (Blueprint default values are not applied): pass every argument that matters.
- Actor names in PIE can differ from the editor (spawned actors get suffixes). Use `find_actors_by_name` with `world: "pie"` or the aliases `@player_pawn`, `@player_controller`, `@game_mode`...
- If game time barely advances, disable Editor Preferences > Performance > "Use Less CPU when in Background". `take_screenshot` needs a visible, non-minimised editor window.
- Properties set through `set_actor_property` in PIE are lost when PIE ends.

## Automation / Functional Tests

For repeatable tests that live in the project (Blueprint `AFunctionalTest` actors placed in a map, Gauntlet-free):

1. `list_automation_tests` (no filter to see categories, then `filter: "Functional Tests"`).
2. `stop_pie`, then `run_automation_tests` with `tests` or `filter`.
3. Poll `get_automation_results` until `state` is `idle`. Each failed test has `events` (message, context, file, line). Read the batch's log with `get_output_log` `since_id: log_cursor`.

Use them as regression checks after fixing a bug found by exploratory play.

## Bug report format

```
Title:     <one line>
Severity:  crash | blocker | major | minor | cosmetic
Steps:     1. ... (exact tool calls / inputs, map, starting state)
Expected:  ...
Actual:    ... (values from check_condition / get_actor_properties)
Evidence:  screenshot path(s), log lines (with category), automation test name
Frequency: always / N of M runs
```

Reproduce a bug twice from a fresh `start_pie` before reporting it, and say which part is a game bug and which is a test-setup limitation (for example a UMG button that cannot be clicked through input).
