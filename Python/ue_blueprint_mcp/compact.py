"""
Token-saving formatting of command results.

Every tool result goes through dumps(): numbers are rounded, Unreal export text loses trailing zeros and
default-only noise, values that only echo a parameter are dropped, node GUIDs become short aliases (#N, accepted
back in any parameter), batch results keep only their scalar fields (full=true for everything), and fields=[...]
keeps only the listed (dotted) paths. Pass raw=true to a tool to skip it.
"""

import json
import math
import re

# Default fields Unreal writes into every Niagara distribution / binding; leaving them out changes nothing
_NOISE = [
    re.compile(r"ParameterBinding=\(TypeDefHandle=\(RegisteredTypeIndex=\d+\)\),?"),
    re.compile(r"ParameterExpression=None,?"),
    re.compile(r"MaxLutSampleCount=128,?"),
    re.compile(r"ValuesTimeRange=\(X=0\.0+,Y=1\.0+\),?"),
]
_TRAILING_ZEROS = re.compile(r"(\d\.\d*?)0+(?!\d)")
_DANGLING_DOT = re.compile(r"(\d)\.(?![\d\w])")
_EMPTY_VALUES = ("", "()", "None", "False", "0", "0.0")
_GUID = re.compile(r"(?<![0-9A-Fa-f])[0-9A-F]{32}(?![0-9A-Fa-f])")
_ALIAS = re.compile(r"#(\d+)")
_READ_PREFIXES = ("get_", "find_", "list_", "check_")  # batch keeps these results whole
_LOCAL_OPTIONS = ("raw", "include_empty", "include_time", "full", "fields")

# Node GUIDs <-> short aliases, kept for the server's lifetime (GUIDs stay valid across calls)
_guid_to_alias: dict[str, str] = {}
_alias_to_guid: dict[str, str] = {}


def _alias(match: re.Match) -> str:
    guid = match.group(0)
    alias = _guid_to_alias.get(guid)
    if alias is None:
        alias = f"#{len(_guid_to_alias) + 1}"
        _guid_to_alias[guid] = alias
        _alias_to_guid[alias] = guid
    return alias


def _unalias(value):
    if isinstance(value, str):
        return _alias_to_guid.get(value, value) if _ALIAS.fullmatch(value) else value
    if isinstance(value, list):
        return [_unalias(item) for item in value]
    if isinstance(value, dict):
        return {key: _unalias(item) for key, item in value.items()}
    return value


def _text(value: str) -> str:
    if len(value) >= 32:
        value = _GUID.sub(_alias, value)
    if len(value) < 6 or not any(c.isdigit() for c in value) and "=" not in value:
        return value
    for pattern in _NOISE:
        value = pattern.sub("", value)
    value = _TRAILING_ZEROS.sub(r"\1", value)
    value = _DANGLING_DOT.sub(r"\1", value)
    return value.replace(",)", ")")


def _number(value: float) -> float | int:
    if not math.isfinite(value):
        return value
    if value == int(value) and abs(value) < 1e15:
        return int(value)
    magnitude = abs(value)
    if magnitude >= 100:
        return round(value, 1)
    if magnitude >= 1:
        return round(value, 2)
    return float(f"{value:.4g}")


def _walk(value, nested: bool = False):
    if isinstance(value, float):
        return _number(value)
    if isinstance(value, str):
        return _text(value)
    if isinstance(value, list):
        return [_walk(item, True) for item in value]
    if isinstance(value, dict):
        # Inside lists/objects, empty lists/objects and success=true (e.g. pin linked_to:[], batch results) say nothing
        return {
            key: _walk(item, True) for key, item in value.items()
            if item is not None and not (nested and (item == [] or item == {} or (key == "success" and item is True)))
        }
    return value


def _options(params: dict | None) -> dict:
    """Local options as real values: some clients send lists and booleans as JSON text ("[\"a\"]", "false")."""
    params = dict(params or {})
    for key in _LOCAL_OPTIONS:
        value = params.get(key)
        if not isinstance(value, str):
            continue
        text = value.strip()
        if key == "fields":
            try:
                parsed = json.loads(text) if text.startswith("[") else [part.strip() for part in text.split(",")]
            except ValueError:
                parsed = [text]
            params[key] = [str(part) for part in parsed if str(part)]
        else:
            params[key] = text.lower() in ("true", "1", "yes")
    return params


def _fields(shaped, params: dict):
    fields = params.get("fields")
    if isinstance(fields, list) and fields and isinstance(shaped, dict) and shaped.get("success") is not False:
        return _select(shaped, [str(field).split(".") for field in fields] + [["success"]])
    return shaped


def _drop_echo(result: dict, params: dict) -> dict:
    return {key: value for key, value in result.items() if key not in params or params[key] != value}


def _brief(result) -> object:
    """A batch sub-result without its lists/objects: ids, names and counts, or the error."""
    if not isinstance(result, dict):
        return result
    if result.get("success") is False:
        return {key: value for key, value in result.items() if key != "success"}
    kept = {key: value for key, value in result.items() if key != "success" and not isinstance(value, (dict, list))}
    return kept or "ok"


def _select(value, paths: list[list[str]]):
    if not paths or any(not path for path in paths):
        return value
    if isinstance(value, list):
        return [_select(item, paths) for item in value]
    if not isinstance(value, dict):
        return value
    selected = {}
    for key, item in value.items():
        rest = [path[1:] for path in paths if path[0] == key]
        if rest:
            selected[key] = _select(item, rest)
    return selected


def _shape_batch(params: dict, result: dict) -> dict:
    holder = result if isinstance(result.get("results"), list) else result.get("details")
    if not isinstance(holder, dict) or not isinstance(holder.get("results"), list):
        return result
    commands = params.get("commands") or []
    shaped = []
    for index, sub in enumerate(holder["results"]):
        command = commands[index] if index < len(commands) and isinstance(commands[index], dict) else {}
        sub_type = str(command.get("type", ""))
        sub_params = _options(command.get("params"))
        reads = sub_type.startswith(_READ_PREFIXES)
        if isinstance(sub, dict):
            # Read results get the same shaping as a direct call (empties dropped, their own fields=[...])
            sub = _fields(_shape(sub_type, sub_params, sub), sub_params) if reads else _drop_echo(sub, _unalias(sub_params))
        shaped.append(sub if params.get("full") or reads else _brief(sub))
    holder["results"] = shaped
    if not params.get("full"):
        holder.pop("executed", None)
        if not holder.get("failed"):
            holder.pop("failed", None)
    return result


def _shape(command: str, params: dict, result: dict) -> dict:
    result = _drop_echo(result, _unalias(params))
    if command == "batch":
        return _shape_batch(params, result)
    if command in ("get_actors_in_level", "find_actors_by_name"):
        actors = result.get("actors")
        if isinstance(actors, list) and all(isinstance(a, dict) and set(a) <= {"name", "class"} for a in actors):
            result["actors"] = [f"{a.get('name')} ({a.get('class')})" for a in actors]
    if command == "get_object_properties" and not params.get("property_names") and not params.get("include_empty"):
        properties = result.get("properties")
        if isinstance(properties, dict):
            kept = {key: value for key, value in properties.items() if value not in _EMPTY_VALUES}
            result["properties"] = kept
            result["omitted_empty"] = len(properties) - len(kept)
            result.pop("property_count", None)
        result.pop("class_path", None)
    elif command == "get_blueprint_info":
        # Placeholder events (disabled, editor boilerplate comment) and node positions say nothing useful
        for event in result.get("events") or []:
            if isinstance(event, dict):
                for key in ("node_x", "node_y", "node_class"):
                    event.pop(key, None)
                if event.get("is_disabled"):
                    event.pop("node_comment", None)
        result.pop("defaults_from_compiled_class", None)
    elif command == "get_output_log" and not params.get("include_time"):
        entries = result.get("entries")
        if isinstance(entries, list):
            result["entries"] = [
                f"{entry.get('id')} {str(entry.get('verbosity', ''))[:1]} {entry.get('category', '')}: {entry.get('message', '')}"
                for entry in entries if isinstance(entry, dict)
            ]
            result.pop("count", None)
    return result


def dumps(result: dict, command: str = "", params: dict | None = None) -> str:
    params = _options(params)
    if params.get("raw"):
        return json.dumps(result, separators=(",", ":"))
    shaped = _fields(_walk(_shape(command, params, result)), params)
    if shaped.get("success") is True and len(shaped) > 1:
        shaped.pop("success")  # no "error" key already means it worked
    return json.dumps(shaped, separators=(",", ":"))


def strip_local(params: dict | None) -> dict | None:
    """Remove the options handled here before the command goes to Unreal."""
    if not params:
        return params
    return _unalias({key: value for key, value in params.items() if key not in _LOCAL_OPTIONS}) or None
