"""
UE Blueprint MCP Server - Main entry point.

Registers all tools with the MCP SDK and handles the server lifecycle.
"""

import asyncio
import logging
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent, ImageContent

from . import compact
from .connection import get_connection, PersistentUnrealConnection, CommandResult

# Import tool modules
from .tools import blueprint, editor, nodes, project, umg, materials

logger = logging.getLogger(__name__)

# Create MCP server instance
server = Server("ue-blueprint-mcp", instructions=(
    "Results are compact JSON: no 'success' key unless false, parameter echoes dropped, node GUIDs shown as #N "
    "(pass #N back anywhere a node id goes). Options on every tool: fields=['a','b.c'] returns only those paths; "
    "raw=true returns the untouched result. batch returns only ids/counts/errors of write commands (get_/find_ "
    "results stay whole); full=true for everything. Read Blueprints with get_graph_connections."
))


def _result_to_text(result: CommandResult) -> list[TextContent]:
    """Convert CommandResult to MCP text content."""
    import json
    return [TextContent(type="text", text=compact.dumps(result.to_dict()))]


def _send_command_sync(command_type: str, params: dict | None = None) -> list[TextContent]:
    """Helper to send command and format response (synchronous)."""
    conn = get_connection()
    if not conn.is_connected:
        conn.connect()
    result = conn.send_command(command_type, compact.strip_local(params))
    return [TextContent(type="text", text=compact.dumps(result.to_dict(), command_type, params))]


async def _send_command(command_type: str, params: dict | None = None) -> list[TextContent]:
    """Helper to send command and format response (async-safe)."""
    return await asyncio.to_thread(_send_command_sync, command_type, params)


def _ping_sync() -> bool:
    """Synchronous ping helper."""
    conn = get_connection()
    if not conn.is_connected:
        conn.connect()
    return conn.ping()


# =============================================================================
# Connection Tools
# =============================================================================

@server.list_tools()
async def list_tools() -> list[Tool]:
    """List all available tools."""
    tools = []

    # Connection tools
    tools.append(Tool(
        name="ping",
        description="Test connection to Unreal Engine",
        inputSchema={"type": "object", "properties": {}}
    ))

    tools.append(Tool(
        name="get_context",
        description="Get current editor context (active blueprint, graph, etc.)",
        inputSchema={"type": "object", "properties": {}}
    ))

    # Add tools from all modules
    tools.extend(editor.get_tools())
    tools.extend(blueprint.get_tools())
    tools.extend(nodes.get_tools())
    tools.extend(project.get_tools())
    tools.extend(umg.get_tools())
    tools.extend(materials.get_tools())

    for tool in tools:
        _slim_schema(tool.inputSchema)
    return tools


# Tool definitions are loaded into the model's context, so self-explaining parameters lose their descriptions
# and the long repeated ones get a short form (the details live in the server instructions).
_OBVIOUS_PARAMS = {
    "blueprint_name", "actor_name", "node_id", "source_node_id", "target_node_id", "pin_name", "source_pin",
    "target_pin", "widget_name", "material_name", "function_name", "variable_name", "component_name",
    "location", "rotation", "scale", "name",
}
_SHORT_DESCRIPTIONS = {
    "node_position": "[X, Y]; omit to auto-place",
    "position": "[X, Y]; omit to auto-place",
    "graph_name": "Function graph (default EventGraph)",
    "world": "editor (default), pie or auto; in pie actor names may be @player_pawn, @player_controller, @game_mode...",
}


def _slim_schema(schema: dict) -> None:
    for key, prop in (schema.get("properties") or {}).items():
        if not isinstance(prop, dict):
            continue
        if key in _OBVIOUS_PARAMS and len(prop.get("description", "")) < 80:
            prop.pop("description", None)
        elif key in _SHORT_DESCRIPTIONS and "description" in prop:
            prop["description"] = _SHORT_DESCRIPTIONS[key]
        if prop.get("type") == "object" and "properties" in prop and key == "condition":
            prop.pop("properties")  # same parameters as check_condition
            prop["description"] = "check_condition parameters"


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent | ImageContent]:
    """Route tool calls to appropriate handlers."""

    # Connection tools
    if name == "ping":
        success = await asyncio.to_thread(_ping_sync)
        return [TextContent(type="text", text=f'{{"success": {str(success).lower()}, "pong": {str(success).lower()}}}')]

    if name == "get_context":
        return await _send_command("get_context")

    # Route to tool modules
    if name in editor.TOOL_HANDLERS:
        return await editor.handle_tool(name, arguments)

    if name in blueprint.TOOL_HANDLERS:
        return await blueprint.handle_tool(name, arguments)

    if name in nodes.TOOL_HANDLERS:
        return await nodes.handle_tool(name, arguments)

    if name in project.TOOL_HANDLERS:
        return await project.handle_tool(name, arguments)

    if name in umg.TOOL_HANDLERS:
        return await umg.handle_tool(name, arguments)

    if name in materials.TOOL_HANDLERS:
        return await materials.handle_tool(name, arguments)

    return [TextContent(type="text", text=f'{{"success": false, "error": "Unknown tool: {name}"}}')]


async def run_server():
    """Run the MCP server."""
    logger.info("Starting UE Blueprint MCP server...")

    # Establish connection to Unreal
    conn = get_connection()
    conn.connect()

    async with stdio_server() as (read_stream, write_stream):
        await server.run(read_stream, write_stream, server.create_initialization_options())


def main():
    """Main entry point."""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
    )

    try:
        asyncio.run(run_server())
    except KeyboardInterrupt:
        logger.info("Server stopped by user")
    finally:
        # Clean up connection
        conn = get_connection()
        conn.disconnect()


if __name__ == "__main__":
    main()
