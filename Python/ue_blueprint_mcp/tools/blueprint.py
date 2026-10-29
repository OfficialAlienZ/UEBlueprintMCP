"""
Blueprint tools - Blueprint creation, components, compilation, and properties.
"""

import asyncio
import json
from typing import Any
from mcp.types import Tool, TextContent

from ..connection import get_connection


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
    """Get all blueprint tools."""
    return [
        # Blueprint creation and management
        Tool(
            name="create_blueprint",
            description="Create a new Blueprint class.",
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "Name of the Blueprint"},
                    "parent_class": {"type": "string", "description": "Parent class (Actor, Pawn, GameStateBase, etc.)"}
                },
                "required": ["name", "parent_class"]
            }
        ),
        Tool(
            name="compile_blueprint",
            description="Compile a Blueprint. Returns error details if compilation fails.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint to compile"}
                },
                "required": ["blueprint_name"]
            }
        ),
        Tool(
            name="get_blueprint_info",
            description=(
                "Read-only introspection of a Blueprint: parent class chain, interfaces, graph index, "
                "variables (type, default value, flags, replication), functions (inputs/outputs/locals), "
                "macros, event dispatchers, events/custom events, components (hierarchy + changed properties), "
                "class defaults, timelines (length/loop/autoplay and every float/vector/color/event track with its "
                "curve keys) and widget tree. Use it to document a Blueprint or to find out whether a "
                "called function is local, a custom event, or inherited. Graph names returned here can be "
                "passed as graph_name to find_blueprint_nodes / get_node_pins (works for functions, macros, "
                "dispatcher signatures and collapsed graphs). A map's Level Blueprint is addressed as "
                "'level:<Map>' (map short name or full path, e.g. 'level:<MapName>' or 'level:/Game/<Path>/<MapName>'; 'level:' = the open map)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Blueprint name or full asset path (e.g. /Game/Folder/BP_Name), or 'level:<Map>' for a Level Blueprint"},
                    "sections": {
                        "type": "array",
                        "items": {
                            "type": "string",
                            "enum": [
                                "parents", "interfaces", "graphs", "variables", "functions", "macros",
                                "dispatchers", "events", "components", "class_defaults", "timelines", "widgets"
                            ]
                        },
                        "description": "Sections to include (default: all). Summary fields are always returned."
                    },
                    "include_component_properties": {
                        "type": "boolean",
                        "description": "Include component properties that differ from class defaults (default true)"
                    }
                },
                "required": ["blueprint_name"]
            }
        ),
        Tool(
            name="get_behavior_tree_info",
            description=(
                "Read-only introspection of a Behavior Tree or Blackboard asset. For a Behavior Tree returns the "
                "Blackboard (keys with types), root decorators and the full node tree: each node has class, name, "
                "static description (human readable, e.g. the blackboard condition), changed properties, services, "
                "and ordered children (left to right = execution order). Decorators sit on the connection to a "
                "child and include inverse_condition, flow_abort_mode and decorator_logic (And/Or/Not). Passing a "
                "Blackboard asset name returns only its keys. Blueprint-based tasks/decorators/services include a "
                "'blueprint' path; inspect them with get_blueprint_info."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "asset_name": {"type": "string", "description": "Behavior Tree or Blackboard name, or full asset path (e.g. /Game/AI/BT_Enemy)"},
                    "include_properties": {
                        "type": "boolean",
                        "description": "Include node/key properties that differ from class defaults (default true)"
                    }
                },
                "required": ["asset_name"]
            }
        ),
        Tool(
            name="get_enum_info",
            description=(
                "Read-only introspection of an Enum: a User Defined Enum asset (e.g. /Game/Data/E_State) or a "
                "native enum (e.g. ECollisionChannel). Returns every entry with index, internal name, "
                "display_name (the name authored in the editor), numeric value, tooltip and hidden flag. "
                "The auto-generated _MAX entry is omitted."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "enum_name": {"type": "string", "description": "Enum asset name or full path, or native enum name"}
                },
                "required": ["enum_name"]
            }
        ),
        Tool(
            name="get_curve_info",
            description=(
                "Read-only introspection of a Curve asset: CurveFloat, CurveVector (x/y/z), CurveLinearColor "
                "(r/g/b/a) or CurveTable (one curve per row). Each curve lists its keys (time, value, interp, "
                "tangents for cubic keys) and the pre/post extrapolation modes."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "curve_name": {"type": "string", "description": "Curve asset name or full path (e.g. /Game/Curves/C_Speed)"}
                },
                "required": ["curve_name"]
            }
        ),
        Tool(
            name="get_data_table_info",
            description=(
                "Read-only introspection of a Data Table asset. Returns the row struct, its columns (authored "
                "name + type) and the rows in table order. Values of User Defined Structs use authored field "
                "names and User Defined Enums use their display names. Rows are paginated (default 200)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "table_name": {"type": "string", "description": "Data Table name or full path (e.g. /Game/Data/DT_Items)"},
                    "include_rows": {"type": "boolean", "description": "Include row values (default true). When false only row_names are returned"},
                    "row_names": {"type": "array", "items": {"type": "string"}, "description": "Only return these rows"},
                    "offset": {"type": "integer", "description": "Rows to skip (default 0)"},
                    "limit": {"type": "integer", "description": "Max rows to return (default 200)"}
                },
                "required": ["table_name"]
            }
        ),
        Tool(
            name="get_object_properties",
            description=(
                "Read-only export of EVERY reflected property (UPROPERTY) of an object as Export Text, like the "
                "editor's Details panel copy. Getter counterpart of set_actor_property / set_blueprint_property. "
                "Targets (exactly one): object_path (full path, e.g. /Script/EngineSettings.Default__GameMapsSettings "
                "or an asset path; the shortcut 'world_settings' is the open level's World Settings with "
                "DefaultGameMode etc.), actor_name (placed actor, optionally component_name) or blueprint_name "
                "(class default object, optionally a component template via component_name; accepts 'level:<MapName>'). "
                "Use property_names to fetch only some properties."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "object_path": {"type": "string", "description": "Full object path, or 'world_settings'"},
                    "actor_name": {"type": "string", "description": "Actor in the open level"},
                    "blueprint_name": {"type": "string", "description": "Blueprint name/path or 'level:<Map>'; exports its class defaults"},
                    "component_name": {"type": "string", "description": "Component of actor_name / blueprint_name"},
                    "property_names": {"type": "array", "items": {"type": "string"}, "description": "Only these properties (case-insensitive)"},
                    "editable_only": {"type": "boolean", "description": "Only editor-exposed properties (default false)"}
                }
            }
        ),
        Tool(
            name="get_project_config",
            description=(
                "Read-only access to the project's ini configuration. Example: default map = file 'Engine', "
                "section '/Script/EngineSettings.GameMapsSettings', key 'GameDefaultMap' (also EditorStartupMap, "
                "GlobalDefaultGameMode). Without 'section' lists the section names (filter with 'contains'); without "
                "'key' returns every key of the section; repeated keys (+Array=...) are returned as arrays. "
                "layer 'merged' (default) is the config as the editor sees it; 'project_default' is the raw "
                "Config/Default<File>.ini with its +/-/. operators."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "file": {
                        "type": "string",
                        "enum": ["Engine", "Game", "Input", "Editor", "EditorPerProjectUserSettings", "GameUserSettings", "Scalability"],
                        "description": "Config file (Engine = DefaultEngine.ini, ...)"
                    },
                    "section": {"type": "string", "description": "Section name, e.g. /Script/EngineSettings.GameMapsSettings"},
                    "key": {"type": "string", "description": "Key inside the section, e.g. GameDefaultMap"},
                    "layer": {"type": "string", "enum": ["merged", "project_default"], "description": "Default merged"},
                    "contains": {"type": "string", "description": "When listing sections, only names containing this text"}
                },
                "required": ["file"]
            }
        ),
        Tool(
            name="set_blueprint_property",
            description="Set a property on a Blueprint class default object.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint"},
                    "property_name": {"type": "string", "description": "Name of the property"},
                    "property_value": {"type": "string", "description": "Value to set"}
                },
                "required": ["blueprint_name", "property_name", "property_value"]
            }
        ),

        # Components
        Tool(
            name="add_component_to_blueprint",
            description="Add a component to a Blueprint.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint"},
                    "component_type": {"type": "string", "description": "Type of component (StaticMeshComponent, BoxComponent, etc.)"},
                    "component_name": {"type": "string", "description": "Name for the component"},
                    "location": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z]"},
                    "rotation": {"type": "array", "items": {"type": "number"}, "description": "[pitch, yaw, roll]"},
                    "scale": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z]"},
                    "component_properties": {"type": "object", "description": "Additional properties to set"}
                },
                "required": ["blueprint_name", "component_type", "component_name"]
            }
        ),
        Tool(
            name="set_static_mesh_properties",
            description="Set static mesh, material, and overlay material on a StaticMeshComponent.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint"},
                    "component_name": {"type": "string", "description": "Name of the component"},
                    "static_mesh": {"type": "string", "description": "Path to static mesh asset"},
                    "material": {"type": "string", "description": "Path to material asset"},
                    "overlay_material": {"type": "string", "description": "Path to overlay material (for outline effects)"}
                },
                "required": ["blueprint_name", "component_name"]
            }
        ),
        Tool(
            name="set_component_property",
            description="Set a property on a component in a Blueprint.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint"},
                    "component_name": {"type": "string", "description": "Name of the component"},
                    "property_name": {"type": "string", "description": "Name of the property"},
                    "property_value": {"type": "string", "description": "Value to set"}
                },
                "required": ["blueprint_name", "component_name", "property_name", "property_value"]
            }
        ),
        Tool(
            name="set_physics_properties",
            description="Set physics properties on a component.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint"},
                    "component_name": {"type": "string", "description": "Name of the component"},
                    "simulate_physics": {"type": "boolean", "description": "Enable physics simulation"},
                    "gravity_enabled": {"type": "boolean", "description": "Enable gravity"},
                    "mass": {"type": "number", "description": "Mass in kg"},
                    "linear_damping": {"type": "number", "description": "Linear damping"},
                    "angular_damping": {"type": "number", "description": "Angular damping"}
                },
                "required": ["blueprint_name", "component_name"]
            }
        ),

        # Spawning
        Tool(
            name="spawn_blueprint_actor",
            description="Spawn an instance of a Blueprint in the level.",
            inputSchema={
                "type": "object",
                "properties": {
                    "blueprint_name": {"type": "string", "description": "Name of the Blueprint to spawn"},
                    "actor_name": {"type": "string", "description": "Name for the spawned actor"},
                    "location": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z]"},
                    "rotation": {"type": "array", "items": {"type": "number"}, "description": "[pitch, yaw, roll]"}
                },
                "required": ["blueprint_name", "actor_name"]
            }
        ),

        # Materials
        Tool(
            name="create_colored_material",
            description="Create a simple colored material asset.",
            inputSchema={
                "type": "object",
                "properties": {
                    "material_name": {"type": "string", "description": "Name for the material"},
                    "color": {"type": "array", "items": {"type": "number"}, "description": "[R, G, B] values 0.0-1.0"}
                },
                "required": ["material_name"]
            }
        ),
    ]


TOOL_HANDLERS = {
    "create_blueprint": "create_blueprint",
    "compile_blueprint": "compile_blueprint",
    "get_blueprint_info": "get_blueprint_info",
    "get_behavior_tree_info": "get_behavior_tree_info",
    "get_enum_info": "get_enum_info",
    "get_curve_info": "get_curve_info",
    "get_data_table_info": "get_data_table_info",
    "get_object_properties": "get_object_properties",
    "get_project_config": "get_project_config",
    "set_blueprint_property": "set_blueprint_property",
    "add_component_to_blueprint": "add_component_to_blueprint",
    "set_static_mesh_properties": "set_static_mesh_properties",
    "set_component_property": "set_component_property",
    "set_physics_properties": "set_physics_properties",
    "spawn_blueprint_actor": "spawn_blueprint_actor",
    "create_colored_material": "create_colored_material",
}


async def handle_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    """Handle a blueprint tool call."""
    command_type = TOOL_HANDLERS.get(name)
    if not command_type:
        return [TextContent(type="text", text=f'{{"success": false, "error": "Unknown tool: {name}"}}')]

    return await _send_command(command_type, arguments if arguments else None)
