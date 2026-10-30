# UEBlueprintMCP - Project Context for Claude

## Overview

UEBlueprintMCP is a Model Context Protocol (MCP) server that enables AI-assisted manipulation of Unreal Engine 4.27 and 5.x Blueprints, Materials, Widgets, and Editor state through a persistent TCP connection.

## Repository Structure

```
UEBlueprintMCP/
├── Python/                          # MCP server (Python)
│   └── ue_blueprint_mcp/
│       ├── server.py                # Entry point, MCP server setup
│       ├── connection.py            # PersistentUnrealConnection (TCP)
│       └── tools/                   # Tool definitions by category
│           ├── blueprint.py         # Blueprint creation, compilation
│           ├── editor.py            # Actor manipulation, viewport, save
│           ├── nodes.py             # Graph nodes (events, functions, variables)
│           ├── project.py           # Enhanced Input assets
│           ├── umg.py               # Widget Blueprints
│           └── materials.py         # Materials, expressions, post-process
│
├── UnrealPlugin/UEBlueprintMCP/     # Editor plugin (C++)
│   └── Source/UEBlueprintMCP/
│       ├── Public/
│       │   ├── MCPBridge.h          # UEditorSubsystem, command router
│       │   ├── MCPServer.h          # TCP server
│       │   ├── MCPContext.h         # Stateful editor context
│       │   ├── MCPCommonUtils.h     # Shared utilities
│       │   ├── MCPInspectionUtils.h # Shared read-only introspection helpers (curves, properties, actors, widgets)
│       │   ├── MCPLogCapture.h      # Output Log ring buffer (FOutputDevice) for get_output_log
│       │   ├── MCPNodeLayout.h      # Auto-placement of newly added graph nodes (overlap avoidance, wire flow)
│       │   └── Actions/             # Action class headers
│       │       ├── EditorAction.h   # Base classes (FEditorAction hierarchy)
│       │       ├── BlueprintActions.h
│       │       ├── EditorActions.h
│       │       ├── InspectionActions.h  # Read-only introspection (get_blueprint_info)
│       │       ├── AIActions.h          # Behavior Tree / Blackboard introspection
│       │       ├── AssetInspectionActions.h # Enum / Curve / Data Table introspection
│       │       ├── ObjectInspectionActions.h # get_object_properties (full UPROPERTY export), get_project_config (ini)
│       │       ├── PIEActions.h         # PIE start/stop/status, Output Log, console commands
│       │       ├── GameplayActions.h    # Runtime gameplay driving in PIE (call_actor_function, inject_input)
│       │       ├── ObserveActions.h     # check_condition (one-shot PIE assertions), take_screenshot
│       │       ├── AutomationActions.h  # list/run/get results of Automation Framework tests (async runner on a core ticker)
│       │       ├── NodeActions.h
│       │       ├── ProjectActions.h
│       │       ├── UMGActions.h
│       │       └── MaterialActions.h
│       └── Private/
│           ├── MCPBridge.cpp        # Action registration & dispatch
│           ├── MCPServer.cpp        # TCP listener, JSON parsing
│           ├── MCPContext.cpp       # Context state, auto-save
│           ├── MCPCommonUtils.cpp   # Blueprint/asset resolution
│           ├── MCPInspectionUtils.cpp
│           ├── MCPLogCapture.cpp
│           ├── MCPNodeLayout.cpp
│           └── Actions/             # Action implementations
│               ├── EditorAction.cpp # Base class + execution pipeline
│               ├── BlueprintActions.cpp
│               ├── EditorActions.cpp
│               ├── InspectionActions.cpp
│               ├── AIActions.cpp
│               ├── AssetInspectionActions.cpp
│               ├── ObjectInspectionActions.cpp
│               ├── PIEActions.cpp
│               ├── GameplayActions.cpp
│               ├── ObserveActions.cpp
│               ├── AutomationActions.cpp
│               ├── NodeActions.cpp
│               ├── ProjectActions.cpp
│               ├── UMGActions.cpp
│               └── MaterialActions.cpp
│
└── docs/
    ├── SKILL.md                     # Claude Code skill file (reference doc)
    └── PLAYTEST.md                  # Skill: autonomous play-testing and bug reporting
```

## Architecture

### Execution Flow

```
MCP Client (Claude Code)
    ↓ MCP protocol
Python Server (server.py)
    ↓ TCP socket (port 55558, persistent)
C++ Plugin (MCPServer → MCPBridge)
    ↓ command dispatch
FEditorAction subclass
    ↓ Validate() → ExecuteInternal() → PostValidate()
Response + Auto-Save
```

### The FEditorAction Pipeline

ALL Unreal Editor operations MUST flow through the `FEditorAction` base class execution pipeline. This ensures:

1. **Validation** - Parameters checked before touching editor state
2. **Error Handling** - Graceful failures with descriptive messages
3. **Auto-Save** - Dirty packages saved after successful actions, debounced (`Context.RequestSave()`: ~2s after the last action, max 15s) so command bursts share one save; use `Context.SaveDirtyPackages()` / `FlushPendingSave()` when disk state is needed immediately
4. **Crash Protection** - SEH wrappers (Windows) prevent editor crashes

```cpp
// FEditorAction::Execute() pipeline:
1. Validate(Params, Context, OutError)     // Pre-check params & state
2. ExecuteInternal(Params, Context)        // Do the actual work
3. PostValidate(Context, OutError)         // Verify results (optional)
4. Context.SaveDirtyPackages()             // Auto-save on success
```

### Action Class Hierarchy

```
FEditorAction                    (base - validation, error handling, save)
├── FBlueprintAction             (Blueprint resolution, modification marking)
│   └── FBlueprintNodeAction     (Graph access, node registration)
├── FMaterialAction              (Material resolution, context tracking)
└── [Direct FEditorAction]       (Actor, viewport, project actions)
```

## Contribution Guidelines

### 1. ALL Editor Operations Flow Through FEditorAction

**CRITICAL:** Never directly manipulate editor state outside the action pipeline. Every command must:

- Inherit from the appropriate action base class
- Implement `Validate()` to check preconditions
- Implement `ExecuteInternal()` for the actual operation
- Return proper success/error responses via `CreateSuccessResponse()` or `CreateErrorResponse()`

The pipeline handles:
- Crash protection (SEH wrapper on Windows)
- Automatic dirty package tracking
- Auto-save after successful operations
- Consistent error formatting

```cpp
// CORRECT: Use the pipeline
class FMyAction : public FBlueprintNodeAction
{
    bool Validate(...) override { /* check params */ }
    TSharedPtr<FJsonObject> ExecuteInternal(...) override {
        // Do work here
        MarkBlueprintModified(Blueprint, Context);
        return CreateSuccessResponse(ResultData);
    }
};

// WRONG: Direct manipulation
void DoSomething() {
    UBlueprint* BP = FindBlueprint("MyBP");
    BP->Modify();  // NO! Not tracked, not validated, won't auto-save
}
```

### 2. Extend Existing Commands Before Creating New Ones

When adding functionality, **prefer adding parameters to existing commands** over creating new specific commands:

```cpp
// GOOD: Extended existing command with new parameter
// add_blueprint_function_node now supports `graph_name` parameter
// to work on function graphs, not just event graph

// BAD: Creating a separate command
// add_blueprint_function_node_to_function_graph  // DON'T DO THIS
```

This keeps the API surface manageable and reduces duplication.

### 3. Keep Files Under 1000 Lines

Large files become difficult to navigate and maintain. Current file sizes:

| File | Lines | Status |
|------|-------|--------|
| NodeActions.cpp | ~1800 | NEEDS SPLIT |
| MaterialActions.cpp | ~1560 | NEEDS SPLIT |
| BlueprintActions.cpp | ~1090 | NEEDS SPLIT |
| EditorActions.cpp | ~636 | OK |
| UMGActions.cpp | ~611 | OK |
| ProjectActions.cpp | ~388 | OK |
| EditorAction.cpp | ~378 | OK |

When a file exceeds 1000 lines, split it by creating a new action category file (e.g., `NodeActions.cpp` could split off `DispatcherActions.cpp` or `FunctionGraphActions.cpp`).

### 4. Error Messages Must Be Descriptive

Return clear error messages that help diagnose issues:

```cpp
// GOOD: Specific, actionable error
OutError = FString::Printf(TEXT("Blueprint '%s' not found. Check spelling or create it first."), *BlueprintName);

// BAD: Vague error
OutError = TEXT("Operation failed");
```

Error types should indicate the category:
- `validation_failed` - Parameter or precondition issue
- `not_found` - Asset/node/pin doesn't exist
- `execution_failed` - Operation threw exception
- `crash_prevented` - SEH caught access violation
- `pie_active` - Asset-modifying action rejected while Play-In-Editor runs (`BlocksDuringPIE()`, defaults to `RequiresSave()`)

### 5. Python Tool Docstrings

Every Python tool must have a docstring that will be shown to MCP clients:

```python
@mcp.tool()
async def my_command(ctx: Context, blueprint_name: str, param: str) -> str:
    """
    Brief description of what this command does.

    Args:
        blueprint_name: Target Blueprint
        param: Description of this parameter

    Returns:
        JSON with node_id on success
    """
    return await send_command_async("my_command", {...})
```

### 6. Register Actions in MCPBridge.cpp

After creating a new action class, register it in `MCPBridge::RegisterActions()`:

```cpp
void UMCPBridge::RegisterActions()
{
    // ... existing registrations ...

    // Add your new action:
    ActionHandlers.Add(TEXT("my_new_command"), MakeShared<FMyNewAction>());
}
```

## Common Patterns

### Creating Blueprint Nodes

Use `FEdGraphSchemaAction_K2NewNode::SpawnNode<T>()` - it handles all the boilerplate:

```cpp
#include "EdGraphSchema_K2_Actions.h"

// Simple node:
UK2Node_IfThenElse* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_IfThenElse>(
    Graph, Position, EK2NewNodeFlags::None
);

// Node with initialization (lambda runs BEFORE pin allocation):
UK2Node_DynamicCast* CastNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_DynamicCast>(
    Graph, Position, EK2NewNodeFlags::None,
    [TargetClass](UK2Node_DynamicCast* Node) {
        Node->TargetType = TargetClass;
    }
);
```

### String-Based Class Resolution

Accept user-friendly strings and resolve them:

```cpp
// In FMCPCommonUtils or your action:
UBlueprint* BP = FMCPCommonUtils::FindBlueprint(BlueprintName);  // "BP_Enemy"
UClass* Class = LoadClass<UObject>(nullptr, TEXT("/Script/Engine.Actor"));  // Engine class
```

### Using Helper Methods

```cpp
// Parameter extraction
FString Name;
if (!GetRequiredString(Params, TEXT("name"), Name, OutError)) return false;
FString Optional = GetOptionalString(Params, TEXT("graph_name"));
FVector2D Position = GetNodePosition(Params);

// Blueprint/Graph access (after validation)
UBlueprint* BP = GetTargetBlueprint(Params, Context);
UEdGraph* Graph = GetTargetGraph(Params, Context);

// State management
MarkBlueprintModified(BP, Context);
RegisterCreatedNode(Node, Context);
```

## Testing Changes

After modifying the C++ plugin:

1. Close Unreal Editor
2. Build: `UnrealBuildTool.exe GravityGokuEditor Win64 Development -project="path/to/project.uproject"`
3. Launch Unreal Editor
4. Reconnect MCP (`/mcp` in Claude Code)
5. Test the modified command

## UE 4.27 Compatibility Notes

All engine-version differences live in `Public/MCPCompat.h` (`MCP_UE5` macro, `MCPCompat::` helpers). Keep `#if` blocks out of action code. Verify with:
`RunUAT.sh BuildPlugin -Plugin=<path>/UEBlueprintMCP.uplugin -Package=/tmp/out -TargetPlatforms=Mac -Rocket`

UE4 differences handled:
- `MarkAsGarbage` → `MarkPendingKill`; `UPackage::SavePackage(Pkg, Asset, Flags, Filename)`
- `FEditorFileUtils::GetDirtyPackages` → `GetDirtyWorldPackages` + `GetDirtyContentPackages`
- Asset registry: `ClassNames` / `GetFName()` instead of `ClassPaths` / `GetClassPathName()`
- Materials: `Material->Expressions` and pins directly on `UMaterial` (no `GetExpressionCollection` / `GetEditorOnlyData`)
- Pins: `PC_Float` instead of `PC_Real` + `PC_Double`
- `SetOverlayMaterial` does not exist (ignored with a warning)
- Classes not exported from their modules (linker errors on `StaticClass()`): `UMaterialExpressionSceneTexture/SceneDepth/ScreenPosition/DDX/DDY`, `UK2Node_GetSubsystemFromPC`. Resolve by name with `FindObject<UClass>` and use reflection / the exported base class.
- Enhanced Input is optional (`MCP_WITH_ENHANCED_INPUT`, set in `UEBlueprintMCP.Build.cs`). It is OFF by default on 4.27 and on only if the project's `.uproject` enables the `EnhancedInput` plugin; ON by default on UE 5.1+. Override with env var `UEBLUEPRINTMCP_ENHANCED_INPUT=0|1`. Enhanced Input commands (`create_input_action`, `create_input_mapping_context`, `add_key_mapping_to_context`, `add_enhanced_input_action_node`) are only registered when enabled; legacy input commands always work. The plugin is not declared in the `.uplugin`.
- `.uplugin` uses no platform allow list (`WhitelistPlatforms` in 4.27 vs `PlatformAllowList` in 5.x)
- `APlayerController::InputKey` takes `FInputKeyParams` from UE 5.1 (`MCP_INPUT_KEY_PARAMS`); use `MCPCompat::InjectKeyEvent` / `InjectKeyAxis`. The UE5 branch is not compiled on a 4.27-only machine.

### PIE world targeting

Actor / inspection actions take an optional `world` param (`editor` default, `pie`, `auto`), resolved by `FEditorAction::ResolveWorld()`. Actions that also work on the PIE world override `BlocksDuringPIE()` to false and call `ValidateEditableWorldParam()` so the editor level stays protected. Actor lookup goes through `FMCPCommonUtils::FindActor(World, Name)` (name, label, then `@player_pawn`-style aliases in game worlds). Runtime-only actions (`call_actor_function`, `inject_input`) require PIE and never touch assets.

### Observing and automated tests

- C++ actions never block the game thread. `check_condition` is one-shot; `wait_for_condition` is a Python polling loop (`tools/editor.py`) over it. `take_screenshot` writes a PNG (ImageWrapper module) and the Python tool attaches it as `ImageContent`.
- Automation tests: `FMCPAutomationRunner` (in `AutomationActions.cpp`) drives `FAutomationTestFramework` from a core ticker, one test at a time (StartTestByName, then ExecuteLatentCommands per tick, StopTest). Results stay in memory until the next batch; `run_automation_tests` is rejected while PIE is active and functional tests manage their own PIE.

### UMG single-child containers (UE4 editor bug)

In the UE4 Designer, pasting into a single-child container (Button, Border, SizeBox, ScaleBox...) that already has a child asks to replace it, but answering "No" still deletes the existing child. Current MCP commands are not affected (they only add to new, empty widgets or the multi-child root Canvas Panel). In any new UMG action, check `CanAddMoreChildren()` / `GetChildrenCount()` before `AddChild` and return an error instead of replacing content.

## UE 5.7 Compatibility Notes

Changes made for UE 5.7 compatibility:
- `ANY_PACKAGE` → `nullptr` (removed in 5.7)
- `FImageUtils::CompressImageArray` → `PNGCompressImageArray` with `TArray64<uint8>`
- `.uplugin` `WhitelistPlatforms` → `PlatformAllowList`
- Global `BufferSize` renamed to `MCPRecvBufferSize` (name conflict)

## Mac Compatibility Notes

- **TCP_NODELAY required**: Python socket must set `TCP_NODELAY` to avoid 200-500ms latency from Nagle + macOS Delayed ACK interaction.
- **HasPendingData before Recv**: C++ must check `HasPendingData()` before peek recv; Mac returns errors on blocking recv with no data.
