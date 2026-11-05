"""
Docs tools - detect Markdown docs that are older than the Unreal asset they describe.

Timestamps are compared as absolute instants (UTC), so editor/OS/CI timezones do not matter.
The asset side comes from the plugin (`get_asset_timestamps`, file mtime on disk, UTC). The doc side is,
in order of preference:

1. `asset_modified` in the doc's YAML front matter: the asset mtime the doc was last verified against.
   Robust against git checkouts / copies that reset the doc file's mtime.
2. The doc file's own mtime (fallback; only "asset newer than doc" counts as stale).

Front matter used by these tools:

    ---
    asset: /Game/Blueprints/BP_Enemy
    asset_modified: 2026-11-04T10:15:30.123Z
    ---
"""

import asyncio
import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from mcp.types import Tool, TextContent

from ..connection import get_connection

DOCS_ROOT_ENV = "UEBLUEPRINTMCP_DOCS_ROOT"
DEFAULT_TOLERANCE_SECONDS = 2.0
MAX_DOCS = 500
FRONT_MATTER_ASSET_KEY = "asset"
FRONT_MATTER_STAMP_KEY = "asset_modified"


def get_tools() -> list[Tool]:
    """Get all docs tools."""
    return [
        Tool(
            name="check_docs_freshness",
            description=(
                "Check whether Markdown docs are still up to date with the Unreal assets they describe, so a doc "
                "can be re-verified after the asset was edited outside the MCP. Compares timestamps in UTC "
                "(timezone safe). For each doc the asset is read from the item or the doc's front matter "
                "(`asset:`), and the asset's .uasset/.umap modified time is compared with the doc's front matter "
                "`asset_modified` (the asset time it was last verified against) or, if absent, the doc file's mtime. "
                "Status per doc: fresh, stale (asset changed after the doc; re-check the asset with the get_*_info "
                "tools, update the doc, then call again with mark_synced=true), doc_missing, asset_not_found, "
                "no_asset. Assets with unsaved editor changes are reported stale (dirty=true). "
                "Use 'items' for specific docs or 'scan_dir' to check every doc under a folder that has an "
                "`asset:` front matter key."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "items": {
                        "type": "array",
                        "items": {
                            "oneOf": [
                                {"type": "string", "description": "Doc path; the asset comes from its front matter"},
                                {
                                    "type": "object",
                                    "properties": {
                                        "doc": {"type": "string", "description": "Doc path (absolute or relative to docs_root)"},
                                        "asset": {"type": "string", "description": "Asset name or path (default: front matter `asset:`)"}
                                    },
                                    "required": ["doc"]
                                }
                            ]
                        },
                        "description": "Docs to check"
                    },
                    "scan_dir": {"type": "string", "description": "Folder (relative to docs_root) scanned recursively for *.md with an `asset:` front matter key"},
                    "docs_root": {"type": "string", "description": f"Docs repo root (default: env {DOCS_ROOT_ENV}, else the MCP server's working directory). Docs must live inside it"},
                    "tolerance_seconds": {"type": "number", "description": f"Differences up to this many seconds count as equal (default {DEFAULT_TOLERANCE_SECONDS})"},
                    "mark_synced": {
                        "type": "boolean",
                        "description": (
                            "After you verified/updated the docs against the asset: write `asset` and `asset_modified` "
                            "(the asset's current UTC mtime) into each doc's front matter. Skipped for docs whose asset has "
                            "unsaved editor changes or was not found. Never use it before actually re-checking the asset."
                        )
                    }
                }
            }
        ),
    ]


TOOL_HANDLERS = {
    "check_docs_freshness": "check_docs_freshness",
}


# --------------------------------------------------------------------------- helpers

def _parse_timestamp(value: str) -> datetime | None:
    """Parse an ISO 8601 timestamp to an aware datetime. Naive values are assumed to be UTC."""
    text = str(value).strip().strip("'\"")
    if not text:
        return None
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed


def _iso_utc(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _iso_local(moment: datetime) -> str:
    return moment.astimezone().isoformat(timespec="milliseconds")


_FRONT_MATTER_RE = re.compile(r"\A---[ \t]*\r?\n(.*?)\r?\n---[ \t]*(?:\r?\n|\Z)", re.DOTALL)


def _read_front_matter(text: str) -> dict[str, str]:
    match = _FRONT_MATTER_RE.match(text)
    if not match:
        return {}
    fields: dict[str, str] = {}
    for line in match.group(1).splitlines():
        key, sep, value = line.partition(":")
        if sep and key.strip() and not key.startswith((" ", "\t", "#")):
            fields[key.strip()] = value.strip().strip("'\"")
    return fields


def _write_front_matter(text: str, updates: dict[str, str]) -> str:
    """Set keys in the front matter (creating the block if needed), leaving the rest untouched."""
    newline = "\r\n" if "\r\n" in text else "\n"
    match = _FRONT_MATTER_RE.match(text)
    if not match:
        block = newline.join(f"{k}: {v}" for k, v in updates.items())
        return f"---{newline}{block}{newline}---{newline}{newline}{text}"

    lines = match.group(1).splitlines()
    pending = dict(updates)
    for index, line in enumerate(lines):
        key = line.partition(":")[0].strip()
        if key in pending and not line.startswith((" ", "\t", "#")):
            lines[index] = f"{key}: {pending.pop(key)}"
    lines.extend(f"{k}: {v}" for k, v in pending.items())
    new_block = f"---{newline}" + newline.join(lines) + f"{newline}---{newline}"
    return new_block + text[match.end():]


def _resolve_root(arguments: dict[str, Any]) -> Path:
    root = arguments.get("docs_root") or os.environ.get(DOCS_ROOT_ENV) or os.getcwd()
    return Path(root).expanduser().resolve()


def _resolve_doc(root: Path, doc: str) -> Path | None:
    """Resolve a doc path and make sure it is a Markdown file inside docs_root."""
    path = Path(doc).expanduser()
    path = (path if path.is_absolute() else root / path).resolve()
    if path.suffix.lower() != ".md" or not path.is_relative_to(root):
        return None
    return path


def _collect_items(root: Path, arguments: dict[str, Any]) -> tuple[list[dict], str | None]:
    entries: list[dict] = []
    for raw in arguments.get("items") or []:
        if isinstance(raw, str):
            entries.append({"doc": raw, "asset": None})
        elif isinstance(raw, dict) and raw.get("doc"):
            entries.append({"doc": str(raw["doc"]), "asset": raw.get("asset")})
        else:
            return [], "Each item must be a doc path string or an object with 'doc' (and optionally 'asset')."

    scan_dir = arguments.get("scan_dir")
    if scan_dir:
        folder = Path(scan_dir).expanduser()
        folder = (folder if folder.is_absolute() else root / folder).resolve()
        if not folder.is_dir() or not folder.is_relative_to(root):
            return [], f"scan_dir '{scan_dir}' is not a folder inside docs_root ({root})."
        for md in sorted(folder.rglob("*.md")):
            try:
                with md.open("r", encoding="utf-8", errors="replace") as handle:
                    head = handle.read(4096)
            except OSError:
                continue
            if FRONT_MATTER_ASSET_KEY in _read_front_matter(head):
                entries.append({"doc": str(md), "asset": None})

    if not entries:
        return [], "Nothing to check: pass 'items' and/or 'scan_dir' (with `asset:` front matter in the docs)."
    if len(entries) > MAX_DOCS:
        return [], f"Too many docs ({len(entries)}). Maximum is {MAX_DOCS} per call."
    return entries, None


def _fetch_asset_timestamps(assets: list[str]) -> dict:
    conn = get_connection()
    if not conn.is_connected:
        conn.connect()
    return conn.send_command("get_asset_timestamps", {"assets": assets}).to_dict()


# --------------------------------------------------------------------------- main logic

def _check_docs_freshness_sync(arguments: dict[str, Any]) -> dict:
    root = _resolve_root(arguments)
    entries, error = _collect_items(root, arguments)
    if error:
        return {"success": False, "error": error}

    try:
        tolerance = float(arguments.get("tolerance_seconds", DEFAULT_TOLERANCE_SECONDS))
    except (TypeError, ValueError):
        return {"success": False, "error": "tolerance_seconds must be a number"}
    mark_synced = bool(arguments.get("mark_synced"))

    # Read every doc and work out which asset it describes.
    checks: list[dict] = []
    for entry in entries:
        check: dict[str, Any] = {"doc": entry["doc"]}
        path = _resolve_doc(root, entry["doc"])
        if path is None:
            check.update(status="error", error=f"'{entry['doc']}' is not a .md file inside docs_root ({root}).")
        elif not path.is_file():
            check.update(status="doc_missing", doc=str(path.relative_to(root)))
        else:
            check["doc"] = str(path.relative_to(root))
            check["_path"] = path
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError) as exc:
                check.update(status="error", error=f"Could not read doc: {exc}")
                checks.append(check)
                continue
            front = _read_front_matter(text)
            check["_front"] = front
            check["_text"] = text
            check["asset"] = entry["asset"] or front.get(FRONT_MATTER_ASSET_KEY)
            if not check["asset"]:
                check.update(status="no_asset", error="No asset given and no `asset:` key in the doc's front matter.")
        checks.append(check)

    lookup = sorted({c["asset"] for c in checks if c.get("asset") and "status" not in c})
    timestamps: dict[str, dict] = {}
    server_time = None
    if lookup:
        response = _fetch_asset_timestamps(lookup)
        if not response.get("success"):
            return response
        server_time = response.get("server_time_utc")
        for item in response.get("assets", []):
            timestamps[item.get("input")] = item

    for check in checks:
        if "status" in check:
            continue
        info = timestamps.get(check["asset"], {})
        if not info.get("found"):
            check.update(status="asset_not_found", error=info.get("error", "Asset not found"))
            if info.get("candidates"):
                check["candidates"] = info["candidates"]
            continue

        asset_time = datetime.fromtimestamp(float(info["modified_epoch"]), tz=timezone.utc)
        check["asset_package"] = info.get("package")
        check["asset_modified_utc"] = _iso_utc(asset_time)
        check["asset_modified_local"] = _iso_local(asset_time)
        check["asset_dirty"] = bool(info.get("dirty"))

        stamp = _parse_timestamp(check["_front"].get(FRONT_MATTER_STAMP_KEY, ""))
        if stamp is not None:
            check["basis"] = "front_matter"
            doc_time = stamp
        else:
            check["basis"] = "file_mtime"
            doc_time = datetime.fromtimestamp(check["_path"].stat().st_mtime, tz=timezone.utc)
        check["doc_verified_utc"] = _iso_utc(doc_time)
        check["doc_verified_local"] = _iso_local(doc_time)

        diff = (asset_time - doc_time).total_seconds()  # > 0: asset newer than the doc
        check["asset_newer_by_seconds"] = round(diff, 3)

        reasons = []
        if diff > tolerance:
            reasons.append("asset was modified after the doc was written/verified")
        elif diff < -tolerance and check["basis"] == "front_matter":
            reasons.append("asset is older than the version the doc was verified against (restored/reverted?)")
        if check["asset_dirty"]:
            reasons.append("asset has unsaved changes in the editor")
        if reasons:
            check.update(status="stale", reasons=reasons)
        else:
            check["status"] = "fresh"

    if mark_synced:
        for check in checks:
            if check.get("status") not in ("fresh", "stale") or check.get("asset_dirty"):
                if "_path" in check:
                    check["marked_synced"] = False
                continue
            updates = {FRONT_MATTER_STAMP_KEY: check["asset_modified_utc"]}
            if not check["_front"].get(FRONT_MATTER_ASSET_KEY):
                updates = {FRONT_MATTER_ASSET_KEY: check["asset"], **updates}
            try:
                with open(check["_path"], "w", encoding="utf-8", newline="") as handle:
                    handle.write(_write_front_matter(check["_text"], updates))
                check["marked_synced"] = True
            except OSError as exc:
                check["marked_synced"] = False
                check["error"] = f"Could not write front matter: {exc}"

    counts: dict[str, int] = {}
    for check in checks:
        counts[check["status"]] = counts.get(check["status"], 0) + 1
        for private in [k for k in check if k.startswith("_")]:
            del check[private]

    return {
        "success": True,
        "docs_root": str(root),
        "server_time_utc": server_time,
        "tolerance_seconds": tolerance,
        "summary": counts,
        "docs": checks,
    }


async def handle_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    """Handle a docs tool call."""
    if name == "check_docs_freshness":
        result = await asyncio.to_thread(_check_docs_freshness_sync, arguments or {})
        return [TextContent(type="text", text=json.dumps(result, indent=2))]
    return [TextContent(type="text", text=f'{{"success": false, "error": "Unknown tool: {name}"}}')]
