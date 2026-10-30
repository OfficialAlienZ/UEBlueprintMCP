// Copyright (c) 2025 zolnoor. All rights reserved.

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
struct FMCPEditorContext;

/**
 * Automatic placement for Blueprint graph nodes created through MCP.
 *
 * Only nodes added by MCP are ever moved. A node is "auto-placed" when it was created without an explicit
 * node_position; those nodes are tracked in FMCPEditorContext::AutoPlacedNodes and may be repositioned when
 * they get wired. Nodes created with an explicit node_position, and every node that existed before, stay where
 * they are (an explicit position is only nudged away from an overlap).
 *
 * Layout rules:
 *  - Entry nodes (events, input actions, ...) start a new row below the existing graph content.
 *  - Other nodes continue the execution row to the right of the previous flow node.
 *  - Pure nodes (data only) are staged below that row until they are wired.
 *  - Wiring pulls nodes into a left-to-right flow: a target that sits left of its source is moved right of it,
 *    a data source is moved left of its consumer, both vertically aligned with the connected pins so wires run
 *    straight. Nodes chained behind a moved node move with it.
 *  - Nothing is placed on top of another node unless no free spot exists. Positions snap to the grid.
 *
 * Node sizes are estimated from titles and pins (the real Slate size is not available without an open editor).
 */
class UEBLUEPRINTMCP_API FMCPNodeLayout
{
public:
	/** Estimated on-screen size of a node (graph units). */
	static FVector2D EstimateNodeSize(const UEdGraphNode* Node);

	/**
	 * Place a node that was just added to its graph and remember it as the layout anchor.
	 * If bExplicitPosition is false the node is auto-placed (position chosen here); otherwise its current
	 * NodePosX/Y is kept (snapped) and only moved when it would overlap another node.
	 */
	static void PlaceNewNode(UEdGraphNode* Node, FMCPEditorContext& Context, bool bExplicitPosition);

	/** Re-flow auto-placed nodes after OutputPin was linked to InputPin (see class comment). */
	static void OnPinsConnected(UEdGraphPin* OutputPin, UEdGraphPin* InputPin, FMCPEditorContext& Context);

	/** Stop treating a node as auto-placed (it was positioned by hand) */
	static void ForgetNode(const UEdGraphNode* Node, FMCPEditorContext& Context);
};
