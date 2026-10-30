"""
Editor tools - Level actors, viewport, and general editor operations.
"""

import asyncio
import base64
import json
import time
from typing import Any
from mcp.types import Tool, TextContent, ImageContent

from ..connection import get_connection, CommandResult


def _send_command_sync(command_type: str, params: dict | None = None) -> list[TextContent]:
    """Helper to send command and format response (synchronous)."""
    conn = get_connection()
    if not conn.is_connected:
        conn.connect()
    result = conn.send_command(command_type, params)
    return [TextContent(type="text", text=json.dumps(result.to_dict(), indent=2))]


async def _send_command(command_type: str, params: dict | None = None) -> list[TextContent]:
    """Helper to send command and format response (async-safe)."""
    return await asyncio.to_thread(_send_command_sync, command_type, params)


WORLD_PROPERTY = {
    "type": "string",
    "enum": ["editor", "pie", "auto"],
    "description": (
        "Which world to act on (default editor). 'pie' = the running Play-In-Editor game (start_pie first): "
        "live runtime state, and actor names may also be @player_pawn, @player_controller, @player_state, "
        "@hud, @game_mode, @game_state, @level_script. 'auto' = PIE if running, else editor."
    ),
}


CONDITION_PROPERTIES = {
    "type": {"type": "string", "enum": ["game_time", "actor_exists", "actor_property", "actor_distance", "log_contains", "pie_stopped"], "description": "Condition type"},
    "negate": {"type": "boolean", "description": "Invert the result (default false)"},
    "actor_name": {"type": "string", "description": "Actor name/label in the PIE world or @player_pawn etc."},
    "component_name": {"type": "string", "description": "actor_property: read the property from this component of the actor"},
    "property_name": {"type": "string", "description": "actor_property: property name or dotted path (e.g. Health, Stats.Level)"},
    "operator": {"type": "string", "enum": ["==", "!=", "<", "<=", ">", ">=", "contains"], "description": "Comparison (default ==; actor_distance default <=)"},
    "value": {"description": "actor_property: expected number/bool/string; actor_distance: distance in cm"},
    "target_actor_name": {"type": "string", "description": "actor_distance: the other actor"},
    "location": {"type": "array", "items": {"type": "number"}, "description": "actor_distance: [x, y, z] world location instead of target_actor_name"},
    "seconds": {"type": "number", "description": "game_time: absolute game time in seconds (get_pie_status time_seconds)"},
    "elapsed": {"type": "number", "description": "game_time (wait_for_condition only): game seconds from now"},
    "text": {"type": "string", "description": "log_contains: text to find in an Output Log line"},
    "since_id": {"type": "integer", "description": "log_contains: only consider log lines after this id"},
    "categories": {"type": "array", "items": {"type": "string"}, "description": "log_contains: only these log categories"},
}


def get_tools() -> list[Tool]:
    """Get all editor tools."""
    return [
        # Level actors
        Tool(
            name="get_actors_in_level",
            description=(
                "List actors in the current level (paginated, sorted by name). "
                "Returns name and class only by default. On large levels, call with "
                "summary_only=true first to see actor counts per class, then narrow "
                "with class_filter. Response includes total_count and truncated; "
                "use offset to fetch further pages."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "world": WORLD_PROPERTY,
                    "class_filter": {"type": "string", "description": "Only actors of this class or a subclass (e.g. StaticMeshActor, PointLight, BP_Enemy)"},
                    "limit": {"type": "integer", "description": "Max actors to return (default 100, max 1000)"},
                    "offset": {"type": "integer", "description": "Number of matching actors to skip (default 0)"},
                    "include_transform": {"type": "boolean", "description": "Include location/rotation/scale (default false)"},
                    "include_details": {"type": "boolean", "description": "Include level placement details per actor: label, level, folder, tags, attachment, components with relative transforms (default false)"},
                    "summary_only": {"type": "boolean", "description": "Return only per-class actor counts (default false)"}
                }
            }
        ),
        Tool(
            name="find_actors_by_name",
            description="Find actors by name pattern (paginated, sorted by name). Supports the same filtering and paging options as get_actors_in_level.",
            inputSchema={
                "type": "object",
                "properties": {
                    "world": WORLD_PROPERTY,
                    "pattern": {"type": "string", "description": "Name pattern to search for"},
                    "class_filter": {"type": "string", "description": "Only actors of this class or a subclass"},
                    "limit": {"type": "integer", "description": "Max actors to return (default 100, max 1000)"},
                    "offset": {"type": "integer", "description": "Number of matching actors to skip (default 0)"},
                    "include_transform": {"type": "boolean", "description": "Include location/rotation/scale (default false)"},
                    "include_details": {"type": "boolean", "description": "Include level placement details per actor: label, level, folder, tags, attachment, components with relative transforms (default false)"},
                    "summary_only": {"type": "boolean", "description": "Return only per-class actor counts (default false)"}
                },
                "required": ["pattern"]
            }
        ),
        Tool(
            name="spawn_actor",
            description="Create a new actor in the current level.",
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Name to give the new actor (must be unique)"},
                    "type": {"type": "string", "description": "Type of actor (e.g. StaticMeshActor, PointLight)"},
                    "location": {
                        "type": "array",
                        "items": {"type": "number"},
                        "description": "[x, y, z] world location"
                    },
                    "rotation": {
                        "type": "array",
                        "items": {"type": "number"},
                        "description": "[pitch, yaw, roll] in degrees"
                    }
                },
                "required": ["name", "type"]
            }
        ),
        Tool(
            name="spawn_blueprint_actor",
            description="Spawn an actor from a Blueprint.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint to spawn from"},
                    "actor_name": {"type": "string", "description": "Name to give the spawned actor"},
                    "location": {
                        "type": "array",
                        "items": {"type": "number"},
                        "description": "[x, y, z] world location"
                    },
                    "rotation": {
                        "type": "array",
                        "items": {"type": "number"},
                        "description": "[pitch, yaw, roll] in degrees"
                    }
                },
                "required": ["blueprint_name", "actor_name"]
            }
        ),
        Tool(
            name="delete_actor",
            description="Delete an actor by name.",
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Name of the actor to delete"}
                },
                "required": ["name"]
            }
        ),
        Tool(
            name="set_actor_transform",
            description="Set the transform of an actor. With world='pie' teleports a live runtime actor (the editor level is untouched).",
            inputSchema={
                "type": "object",
                "properties": {
                    "world": WORLD_PROPERTY,
                    "name": {"type": "string", "description": "Name of the actor"},
                    "location": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z]"},
                    "rotation": {"type": "array", "items": {"type": "number"}, "description": "[pitch, yaw, roll]"},
                    "scale": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z]"}
                },
                "required": ["name"]
            }
        ),
        Tool(
            name="get_actor_properties",
            description=(
                "Get an actor's level placement: name, label, class (and Blueprint path), level, folder, tags, "
                "transform, attachment, every component (class, creation method, attachment, relative transform, "
                "properties changed from the archetype) and the actor properties overridden from the class defaults. "
                "With include_all_properties (or property_names) also returns all_properties: every reflected "
                "property as Export Text. Use name 'WorldSettings' for the level's World Settings (DefaultGameMode, ...)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "world": WORLD_PROPERTY,
                    "name": {"type": "string", "description": "Name of the actor"},
                    "include_properties": {"type": "boolean", "description": "Include changed component/actor properties (default true)"},
                    "include_all_properties": {"type": "boolean", "description": "Add all_properties with every UPROPERTY as Export Text (default false)"},
                    "property_names": {"type": "array", "items": {"type": "string"}, "description": "Only these properties in all_properties (implies include_all_properties)"},
                    "editable_only": {"type": "boolean", "description": "Restrict all_properties to editor-exposed properties (default false)"}
                },
                "required": ["name"]
            }
        ),
        Tool(
            name="set_actor_property",
            description="Set a property on an actor. With world='pie' changes the live value in the running game (cheat / set up a test scenario); it is lost when PIE ends.",
            inputSchema={
                "type": "object",
                "properties": {
                    "world": WORLD_PROPERTY,
                    "name": {"type": "string", "description": "Name of the actor"},
                    "property_name": {"type": "string", "description": "Name of the property"},
                    "property_value": {"type": "string", "description": "Value to set"}
                },
                "required": ["name", "property_name", "property_value"]
            }
        ),

        # Viewport
        Tool(
            name="focus_viewport",
            description="Focus the viewport on a specific actor or location.",
            inputSchema={
                "type": "object",
                "properties": {
                    "target": {"type": "string", "description": "Name of actor to focus on"},
                    "location": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z]"},
                    "distance": {"type": "number", "description": "Distance from target"},
                    "orientation": {"type": "array", "items": {"type": "number"}, "description": "[pitch, yaw, roll]"}
                }
            }
        ),
        Tool(
            name="get_viewport_transform",
            description="Get the current viewport camera location and rotation.",
            inputSchema={"type": "object", "properties": {}}
        ),
        Tool(
            name="set_viewport_transform",
            description="Set the viewport camera location and/or rotation directly.",
            inputSchema={
                "type": "object",
                "properties": {
                    "location": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z]"},
                    "rotation": {"type": "array", "items": {"type": "number"}, "description": "[pitch, yaw, roll]"}
                }
            }
        ),

        # Utility
        Tool(
            name="save_all",
            description="Save all dirty packages (blueprints, levels, assets).",
            inputSchema={"type": "object", "properties": {}}
        ),

        # Play-In-Editor and runtime feedback
        Tool(
            name="start_pie",
            description=(
                "Start a Play-In-Editor session. Returns immediately (state 'starting'); poll get_pie_status "
                "until state is 'running'. Returns log_cursor, to pass as since_id to get_output_log to read "
                "only what the session logs. While PIE runs or starts, asset-modifying commands (spawn, "
                "set property, compile, save...) are rejected with error_type 'pie_active'; call stop_pie first."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "simulate": {"type": "boolean", "description": "Simulate In Editor instead of Play In Editor (default false)"},
                    "clear_log": {"type": "boolean", "description": "Clear the captured Output Log before starting (default false)"}
                }
            }
        ),
        Tool(
            name="stop_pie",
            description="Stop the running Play-In-Editor session. Poll get_pie_status until state is 'stopped'.",
            inputSchema={"type": "object", "properties": {}}
        ),
        Tool(
            name="get_pie_status",
            description=(
                "Get Play-In-Editor state: 'stopped', 'starting' or 'running' (plus world, net_mode, time_seconds, "
                "paused, actor_count and a 'player' object with the first player's controller/pawn name, class and "
                "location while running) and the current log_cursor. If a start request never began, "
                "a warning is returned."
            ),
            inputSchema={"type": "object", "properties": {}}
        ),
        Tool(
            name="get_output_log",
            description=(
                "Read captured Unreal Output Log lines (ring buffer of the last 5000 lines). Without since_id "
                "returns the most recent matching lines; with since_id returns newer lines oldest-first. "
                "Pass the returned next_id as since_id on the next call to read incrementally. "
                "dropped=true means lines were lost between since_id and the buffer start; "
                "truncated=true means more matches exist than limit."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "since_id": {"type": "integer", "description": "Only entries with id greater than this (use next_id from a previous call)"},
                    "limit": {"type": "integer", "description": "Max entries to return (default 200, max 1000)"},
                    "min_verbosity": {"type": "string", "enum": ["Error", "Warning", "Display", "Log", "Verbose", "VeryVerbose"], "description": "Include this severity or worse (default Log)"},
                    "categories": {"type": "array", "items": {"type": "string"}, "description": "Only these log categories, e.g. [\"LogBlueprintUserMessages\", \"LogTemp\"]"},
                    "contains": {"type": "string", "description": "Case-insensitive substring filter on the message"},
                    "include_mcp": {"type": "boolean", "description": "Include this plugin's own 'UEBlueprintMCP:' lines (default false)"},
                    "clear": {"type": "boolean", "description": "Clear the buffer after reading (default false)"}
                }
            }
        ),
        Tool(
            name="execute_console_command",
            description=(
                "Run a console command in the PIE world (if running) or the editor world, e.g. 'stat fps', "
                "'ke * MyEvent', 'slomo 0.5'. Returns handled and any direct output; many commands write to the "
                "Output Log instead, so follow up with get_output_log. quit/exit/crash/py are rejected."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "command": {"type": "string", "description": "Console command to execute"},
                    "world": {"type": "string", "enum": ["auto", "pie", "editor"], "description": "Target world (default auto: PIE if running, else editor)"}
                },
                "required": ["command"]
            }
        ),
        Tool(
            name="call_actor_function",
            description=(
                "Call a Blueprint function or custom event on a live actor (or one of its components) in the running "
                "PIE game. Use it to drive and test gameplay: trigger an ability, apply damage, read a value through a "
                "getter, start a state. Requires start_pie. Returns the function's outputs (return_value and out "
                "params). Omitted args are zero-initialised (Blueprint default values are not applied). If the "
                "function name is wrong, the error lists the Blueprint functions/events of the class."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "actor_name": {"type": "string", "description": "Actor name/label in the PIE world, or @player_pawn, @player_controller, @player_state, @hud, @game_mode, @game_state, @level_script"},
                    "function_name": {"type": "string", "description": "Function or custom event name (case-insensitive)"},
                    "component_name": {"type": "string", "description": "Call the function on this component of the actor instead"},
                    "args": {"type": "object", "description": "Parameter name -> value. Numbers, bools, strings, enums by name, structs as JSON objects, vectors/rotators as [x, y, z], actors by name/label/@alias"}
                },
                "required": ["actor_name", "function_name"]
            }
        ),
        Tool(
            name="inject_input",
            description=(
                "Feed keyboard/mouse/gamepad input to the player controller of the running PIE game, as if a device "
                "produced it (Input Action/Axis events and legacy bindings fire). Requires start_pie. Timed inputs run "
                "in game time and return immediately, so check get_pie_status time_seconds (or sleep) to know when "
                "they are done. Does not drive UMG widgets (menus/buttons) - call their functions or use "
                "call_actor_function instead. Works while the PIE window is unfocused; if the editor is in the "
                "background the game may be throttled (disable 'Use Less CPU when in Background' in Editor Preferences). "
                "Examples: hold W for 2s = {key:'W', action:'press', duration:2}; jump = {key:'SpaceBar'}; "
                "turn camera = {key:'MouseX', value:3, duration:0.5}; stick right = {key:'Gamepad_LeftX', action:'press', value:1}."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "key": {"type": "string", "description": "Unreal key name: W, SpaceBar, LeftShift, LeftMouseButton, MouseX, MouseY, Gamepad_LeftX, Gamepad_FaceButton_Bottom... (required except for release_all)"},
                    "action": {"type": "string", "enum": ["tap", "press", "release", "release_all"], "description": "tap = press then release after duration (default 0.1s); press = hold (released after duration if given); release; release_all (every held key)"},
                    "value": {"type": "number", "description": "Analog/axis value (default 1). A per-frame delta for mouse axes; axis keys re-send the value every frame while held"},
                    "duration": {"type": "number", "description": "Seconds of game time to hold the input"},
                    "player_index": {"type": "integer", "description": "Local player (default 0)"}
                }
            }
        ),
        Tool(
            name="check_condition",
            description=(
                "Evaluate ONE condition against the running PIE game right now and return met true/false plus the "
                "evaluated values (never blocks). Use it to assert expected behaviour, e.g. a property value after an "
                "action. Types: game_time (seconds = absolute game time), actor_exists (actor_name), actor_property "
                "(actor_name [+component_name], property_name, operator, value; dotted paths reach into structs/objects), "
                "actor_distance (actor_name + target_actor_name or location [x,y,z]; value + operator, default <=), "
                "log_contains (text, optional since_id/categories), pie_stopped. 'negate' inverts the result. Errors "
                "when PIE is not running (except log_contains / pie_stopped) or when a property does not exist. A "
                "missing actor is met=false with a reason. Use wait_for_condition to wait."
            ),
            inputSchema={
                "type": "object",
                "properties": CONDITION_PROPERTIES,
                "required": ["type"]
            }
        ),
        Tool(
            name="wait_for_condition",
            description=(
                "Poll a check_condition until it is met or the real-time timeout passes. Use it instead of sleeping "
                "after inject_input / call_actor_function: e.g. wait until the enemy's Health <= 0, the player is "
                "within 200cm of a goal, or the log shows a line. 'condition' has the check_condition parameters; for "
                "game_time you can pass 'elapsed' (game seconds from now) instead of 'seconds'. 'abort_conditions' (a "
                "list of conditions, e.g. the player dying or the log showing an error) end the wait early with "
                "met=false and aborted_by. Returns the last check result plus waited_seconds, timed_out. Stops with an "
                "error if PIE ends meanwhile."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "condition": {"type": "object", "description": "check_condition parameters (type, actor_name, property_name, operator, value, ...)", "properties": CONDITION_PROPERTIES},
                    "timeout_seconds": {"type": "number", "description": "Real-time limit (default 30; for game_time with elapsed: elapsed*2+10). Max 600"},
                    "poll_interval": {"type": "number", "description": "Seconds between checks (default 0.25)"},
                    "abort_conditions": {"type": "array", "items": {"type": "object"}, "description": "Conditions that stop the wait early when met"}
                },
                "required": ["condition"]
            }
        ),
        Tool(
            name="take_screenshot",
            description=(
                "Capture the PIE game view (or the editor viewport) and return it as an image, also saved as a PNG "
                "under <Project>/Saved/MCP/Screenshots. Use it to check what the player sees (UI, visual glitches) "
                "and to attach evidence to bug reports. The editor window must be visible, not minimised."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "target": {"type": "string", "enum": ["auto", "pie", "editor"], "description": "auto (default): PIE if running, else the editor viewport"},
                    "max_width": {"type": "integer", "description": "Downscale to at most this width (default 960, 0 = full size)"},
                    "label": {"type": "string", "description": "Text added to the file name (letters, digits, - and _)"}
                }
            }
        ),
        Tool(
            name="list_automation_tests",
            description=(
                "List Unreal Automation Framework tests (same list as Window > Test Automation): engine tests, "
                "'Project.Functional Tests.*' (AFunctionalTest actors placed in maps - no C++ needed), "
                "'Project.Maps.*'. Returns total, per-category counts and tests with the 'name' to pass to "
                "run_automation_tests. Start with no filter to see the categories, then filter (e.g. 'Functional Tests')."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "filter": {"type": "string", "description": "Case-insensitive substring of the test path"},
                    "limit": {"type": "integer", "description": "Max tests to return (default 100, max 1000)"},
                    "offset": {"type": "integer", "description": "Tests to skip (default 0)"}
                }
            }
        ),
        Tool(
            name="run_automation_tests",
            description=(
                "Start automation tests (asynchronous): they run one after another in the editor; poll "
                "get_automation_results until state is 'idle'. Functional tests open their map and run PIE "
                "themselves, so PIE must be stopped first. Give exact names/paths ('tests') and/or a substring "
                "'filter'. cancel=true stops a running batch."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "tests": {"type": "array", "items": {"type": "string"}, "description": "Exact test names or paths from list_automation_tests"},
                    "filter": {"type": "string", "description": "Run every test whose path contains this (case-insensitive)"},
                    "max_tests": {"type": "integer", "description": "Refuse to start if more tests match (default 100)"},
                    "timeout_seconds": {"type": "number", "description": "Per-test real-time limit (default 300)"},
                    "cancel": {"type": "boolean", "description": "Stop the running batch"}
                }
            }
        ),
        Tool(
            name="get_automation_results",
            description=(
                "Progress and results of the last run_automation_tests batch: state (running/idle), completed/total, "
                "passed/failed, the running test, and per test success, duration, error/warning counts and events "
                "(message, context, file, line). Also returns log_cursor: pass it as since_id to get_output_log for "
                "the batch's log output (e.g. LogFunctionalTest)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "include_info": {"type": "boolean", "description": "Also include Info events (default false: errors and warnings only)"}
                }
            }
        ),
    ]


# Map tool names to command types
TOOL_HANDLERS = {
    "get_actors_in_level": "get_actors_in_level",
    "find_actors_by_name": "find_actors_by_name",
    "spawn_actor": "spawn_actor",
    "spawn_blueprint_actor": "spawn_blueprint_actor",
    "delete_actor": "delete_actor",
    "set_actor_transform": "set_actor_transform",
    "get_actor_properties": "get_actor_properties",
    "set_actor_property": "set_actor_property",
    "focus_viewport": "focus_viewport",
    "get_viewport_transform": "get_viewport_transform",
    "set_viewport_transform": "set_viewport_transform",
    "save_all": "save_all",
    "start_pie": "start_pie",
    "stop_pie": "stop_pie",
    "get_pie_status": "get_pie_status",
    "get_output_log": "get_output_log",
    "execute_console_command": "execute_console_command",
    "call_actor_function": "call_actor_function",
    "inject_input": "inject_input",
    "check_condition": "check_condition",
    "wait_for_condition": "check_condition",
    "take_screenshot": "take_screenshot",
    "list_automation_tests": "list_automation_tests",
    "run_automation_tests": "run_automation_tests",
    "get_automation_results": "get_automation_results",
}

MAX_WAIT_SECONDS = 600.0


def _wait_for_condition_sync(arguments: dict[str, Any]) -> dict:
    """Poll check_condition from a worker thread (the C++ side never blocks)."""
    conn = get_connection()
    if not conn.is_connected:
        conn.connect()

    def check(params: dict) -> dict:
        return conn.send_command("check_condition", params).to_dict()

    condition = dict(arguments.get("condition") or {})
    if not condition.get("type"):
        return {"success": False, "error": "condition.type is required"}
    abort_conditions = [c for c in (arguments.get("abort_conditions") or []) if isinstance(c, dict)]

    default_timeout = 30.0
    if condition.get("type") == "game_time" and "elapsed" in condition and "seconds" not in condition:
        try:
            elapsed = float(condition.pop("elapsed"))
        except (TypeError, ValueError):
            return {"success": False, "error": "condition.elapsed must be a number"}
        now = check({"type": "game_time", "seconds": 0})
        if not now.get("success"):
            return now
        condition["seconds"] = float(now.get("game_time", 0.0)) + elapsed
        default_timeout = elapsed * 2 + 10

    timeout = min(float(arguments.get("timeout_seconds") or default_timeout), MAX_WAIT_SECONDS)
    poll = min(max(float(arguments.get("poll_interval") or 0.25), 0.05), 5.0)

    start = time.monotonic()
    while True:
        result = check(condition)
        waited = round(time.monotonic() - start, 2)
        if not result.get("success"):
            result["waited_seconds"] = waited
            return result
        if result.get("met"):
            return {**result, "waited_seconds": waited, "timed_out": False}

        for abort in abort_conditions:
            abort_result = check(abort)
            if not abort_result.get("success"):
                abort_result["waited_seconds"] = waited
                return abort_result
            if abort_result.get("met"):
                return {**result, "met": False, "aborted_by": abort, "abort_result": abort_result,
                        "waited_seconds": waited, "timed_out": False}

        if time.monotonic() - start >= timeout:
            return {**result, "waited_seconds": waited, "timed_out": True}
        time.sleep(poll)


def _take_screenshot_sync(arguments: dict[str, Any]) -> list[TextContent | ImageContent]:
    """Capture a screenshot and return it as image content next to the JSON metadata."""
    conn = get_connection()
    if not conn.is_connected:
        conn.connect()
    result = conn.send_command("take_screenshot", arguments if arguments else None).to_dict()
    content: list[TextContent | ImageContent] = []
    file_path = result.get("file") if result.get("success") else None
    if file_path:
        try:
            with open(file_path, "rb") as f:
                data = base64.b64encode(f.read()).decode("ascii")
            content.append(ImageContent(type="image", data=data, mimeType="image/png"))
        except OSError as e:
            result["image_error"] = f"Screenshot saved but could not be read by the MCP server: {e}"
    return [TextContent(type="text", text=json.dumps(result, indent=2))] + content


async def handle_tool(name: str, arguments: dict[str, Any]) -> list[TextContent | ImageContent]:
    """Handle an editor tool call."""
    command_type = TOOL_HANDLERS.get(name)
    if not command_type:
        return [TextContent(type="text", text=f'{{"success": false, "error": "Unknown tool: {name}"}}')]

    if name == "wait_for_condition":
        result = await asyncio.to_thread(_wait_for_condition_sync, arguments or {})
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    if name == "take_screenshot":
        return await asyncio.to_thread(_take_screenshot_sync, arguments or {})

    return await _send_command(command_type, arguments if arguments else None)
