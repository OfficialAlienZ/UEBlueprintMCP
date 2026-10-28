"""
Editor tools - Level actors, viewport, and general editor operations.
"""

import asyncio
import json
from typing import Any
from mcp.types import Tool, TextContent

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
            description="Set the transform of an actor.",
            inputSchema={
                "type": "object",
                "properties": {
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
                "properties changed from the archetype) and the actor properties overridden from the class defaults."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Name of the actor"},
                    "include_properties": {"type": "boolean", "description": "Include changed component/actor properties (default true)"}
                },
                "required": ["name"]
            }
        ),
        Tool(
            name="set_actor_property",
            description="Set a property on an actor.",
            inputSchema={
                "type": "object",
                "properties": {
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
                "paused, actor_count while running) and the current log_cursor. If a start request never began, "
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
}


async def handle_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    """Handle an editor tool call."""
    command_type = TOOL_HANDLERS.get(name)
    if not command_type:
        return [TextContent(type="text", text=f'{{"success": false, "error": "Unknown tool: {name}"}}')]

    return await _send_command(command_type, arguments if arguments else None)
