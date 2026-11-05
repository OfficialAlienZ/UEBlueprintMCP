"""
UMG tools - Widget Blueprint creation and manipulation.
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
    """Get all UMG tools."""
    return [
        Tool(
            name="create_umg_widget_blueprint",
            description="Create a new UMG Widget Blueprint, or duplicate an existing one with duplicate_from.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the widget blueprint"},
                    "duplicate_from": {"type": "string", "description": "Existing Widget Blueprint to duplicate (name, path relative to /Game, or /Game/... path). Fails if the destination already exists."},
                    "parent_class": {"type": "string", "description": "Parent class (default: UserWidget)"},
                    "path": {"type": "string", "description": "Content browser path (default: /Game/UI)"}
                },
                "required": ["widget_name"]
            }
        ),
        Tool(
            name="add_text_block_to_widget",
            description="Add a Text Block widget to a UMG Widget Blueprint.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "text_block_name": {"type": "string", "description": "Name for the Text Block"},
                    "text": {"type": "string", "description": "Initial text content"},
                    "position": {"type": "array", "items": {"type": "number"}, "description": "[X, Y] position"},
                    "size": {"type": "array", "items": {"type": "number"}, "description": "[Width, Height]"},
                    "font_size": {"type": "integer", "description": "Font size in points"},
                    "color": {"type": "array", "items": {"type": "number"}, "description": "[R, G, B, A] values 0.0-1.0"}
                },
                "required": ["widget_name", "text_block_name"]
            }
        ),
        Tool(
            name="add_button_to_widget",
            description="Add a Button widget to a UMG Widget Blueprint.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "button_name": {"type": "string", "description": "Name for the Button"},
                    "text": {"type": "string", "description": "Button text"},
                    "position": {"type": "array", "items": {"type": "number"}, "description": "[X, Y] position"},
                    "size": {"type": "array", "items": {"type": "number"}, "description": "[Width, Height]"},
                    "font_size": {"type": "integer", "description": "Font size"},
                    "color": {"type": "array", "items": {"type": "number"}, "description": "[R, G, B, A] text color"},
                    "background_color": {"type": "array", "items": {"type": "number"}, "description": "[R, G, B, A] background"}
                },
                "required": ["widget_name", "button_name"]
            }
        ),
        Tool(
            name="add_image_to_widget",
            description="Add an Image widget to a UMG Widget Blueprint (root Canvas Panel by default, or inside parent_widget). Optionally set the brush from a Texture2D/Material asset and a tint.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "image_name": {"type": "string", "description": "Name for the Image"},
                    "parent_widget": {"type": "string", "description": "Name of an existing panel widget to add the image to (default: root)"},
                    "brush_path": {"type": "string", "description": "Texture2D or Material asset path, e.g. /Game/UI/T_Circle"},
                    "color": {"type": "array", "items": {"type": "number"}, "description": "[R, G, B, A] tint 0.0-1.0"},
                    "position": {"type": "array", "items": {"type": "number"}, "description": "[X, Y] (Canvas Panel parents only)"},
                    "size": {"type": "array", "items": {"type": "number"}, "description": "[Width, Height] (Canvas Panel parents only)"}
                },
                "required": ["widget_name", "image_name"]
            }
        ),
        Tool(
            name="duplicate_widget",
            description="Duplicate a leaf widget (Image, TextBlock, ProgressBar...) inside a Widget Blueprint, copying its properties and slot layout. Widgets with children are rejected.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "source_widget": {"type": "string", "description": "Widget to copy (e.g., CooldownCircle)"},
                    "new_name": {"type": "string", "description": "Name of the copy"},
                    "parent_widget": {"type": "string", "description": "Panel to put the copy in (default: same parent as the source)"},
                    "position": {"type": "array", "items": {"type": "number"}, "description": "[X, Y] override (Canvas Panel parents only)"},
                    "size": {"type": "array", "items": {"type": "number"}, "description": "[Width, Height] override (Canvas Panel parents only)"}
                },
                "required": ["widget_name", "source_widget", "new_name"]
            }
        ),
        Tool(
            name="remove_widget",
            description="Remove a widget and all its children from a Widget Blueprint (root cannot be removed). Graph nodes referencing removed widgets will cause compile errors.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "target_widget": {"type": "string", "description": "Name of the widget to remove"}
                },
                "required": ["widget_name", "target_widget"]
            }
        ),
        Tool(
            name="bind_widget_event",
            description="Bind an event on a widget component (e.g., button OnClicked). Creates a Component Bound Event node in the graph that fires when the event occurs.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "widget_component_name": {"type": "string", "description": "Name of the widget component (e.g., RestartButton)"},
                    "event_name": {"type": "string", "description": "Event to bind (OnClicked, OnPressed, OnReleased, etc.)"}
                },
                "required": ["widget_name", "widget_component_name", "event_name"]
            }
        ),
        Tool(
            name="add_widget_to_viewport",
            description="Add a Widget Blueprint instance to the viewport.",
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "z_order": {"type": "integer", "description": "Z-order (higher = on top)"}
                },
                "required": ["widget_name"]
            }
        ),
        Tool(
            name="set_text_block_binding",
            description=(
                "Bind a widget property to a function the way the Designer's 'Bind' dropdown does (no member variable "
                "is created). Creates a pure function 'Get_<Widget>_<Property>' with the correct return type, registers "
                "the binding and returns function_name plus entry_node_id/result_node_id so the body can be built with "
                "the node commands (graph_name = function_name) and wired into the result node's ReturnValue pin. "
                "Use this for values that change at runtime; for static values use set_component_property instead."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "widget_name": {"type": "string", "description": "Name of the Widget Blueprint"},
                    "text_block_name": {"type": "string", "description": "Name of the widget to bind (any widget with a bindable property, not only Text Blocks)"},
                    "binding_type": {"type": "string", "description": "Widget property to bind: Text (default), Visibility, ColorAndOpacity, IsEnabled, ToolTipText, Percent... Lists the bindable ones on error."},
                    "function_name": {"type": "string", "description": "Optional. Existing function to bind (must match the signature), or the name for the new function. Default: Get_<Widget>_<Property>"},
                    "remove": {"type": "boolean", "description": "Remove the binding for this widget property instead of adding one"}
                },
                "required": ["widget_name", "text_block_name"]
            }
        ),
    ]


TOOL_HANDLERS = {
    "create_umg_widget_blueprint": "create_umg_widget_blueprint",
    "add_text_block_to_widget": "add_text_block_to_widget",
    "add_button_to_widget": "add_button_to_widget",
    "add_image_to_widget": "add_image_to_widget",
    "duplicate_widget": "duplicate_widget",
    "remove_widget": "remove_widget",
    "bind_widget_event": "bind_widget_event",
    "add_widget_to_viewport": "add_widget_to_viewport",
    "set_text_block_binding": "set_text_block_binding",
}


async def handle_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    """Handle a UMG tool call."""
    command_type = TOOL_HANDLERS.get(name)
    if not command_type:
        return [TextContent(type="text", text=f'{{"success": false, "error": "Unknown tool: {name}"}}')]

    return await _send_command(command_type, arguments if arguments else None)
