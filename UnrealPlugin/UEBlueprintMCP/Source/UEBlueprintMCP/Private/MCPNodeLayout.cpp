// Copyright (c) 2025 zolnoor. All rights reserved.

#include "MCPNodeLayout.h"
#include "MCPContext.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"

namespace
{
	constexpr float GridSize = 16.f;
	constexpr float HGap = 48.f;        // horizontal space between a node and the next one in the flow
	constexpr float VGap = 16.f;        // vertical space between stacked nodes
	constexpr float RowGap = 64.f;      // vertical space between separate event chains
	constexpr float PinRowH = 26.f;
	constexpr float HeaderH = 34.f;
	constexpr int32 MaxSearchSteps = 64;

	struct FMetrics
	{
		FVector2D Size = FVector2D(200.f, 60.f);
		float Header = HeaderH;   // vertical offset of the first pin row from the top of the node
	};

	bool IsExecPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	bool IsPinVisible(const UEdGraphNode* Node, const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->bHidden)
		{
			return false;
		}
		return !(Pin->bAdvancedView && Node->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
	}

	bool HasExecPin(const UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (IsExecPin(Pin) && Pin->Direction == Direction && IsPinVisible(Node, Pin))
			{
				return true;
			}
		}
		return false;
	}

	/** Pure nodes are data only: no execution pins at all. */
	bool IsPureNode(const UEdGraphNode* Node)
	{
		return !HasExecPin(Node, EGPD_Input) && !HasExecPin(Node, EGPD_Output);
	}

	/** Entry nodes start an execution chain: they have an exec output but no exec input. */
	bool IsEntryNode(const UEdGraphNode* Node)
	{
		return HasExecPin(Node, EGPD_Output) && !HasExecPin(Node, EGPD_Input);
	}

	/** Extra width taken by the inline default-value editor of an unconnected input pin. */
	float DefaultValueWidgetWidth(const UEdGraphPin* Pin)
	{
		if (Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() > 0 || Pin->PinType.IsContainer() || IsExecPin(Pin))
		{
			return 0.f;
		}

		// Compared by name so this stays valid across engine versions (PC_Float vs PC_Real / PC_Double)
		const FString Category = Pin->PinType.PinCategory.ToString();
		if (Category == TEXT("bool"))
		{
			return 24.f;
		}
		if (Category == TEXT("string") || Category == TEXT("text"))
		{
			return 100.f;
		}
		if (Category == TEXT("int") || Category == TEXT("int64") || Category == TEXT("float") || Category == TEXT("real")
			|| Category == TEXT("double") || Category == TEXT("byte") || Category == TEXT("name"))
		{
			return 64.f;
		}
		if (Category == TEXT("struct") && Pin->PinType.PinSubCategoryObject.IsValid())
		{
			const FString StructName = Pin->PinType.PinSubCategoryObject->GetName();
			if (StructName == TEXT("Vector") || StructName == TEXT("Rotator"))
			{
				return 170.f;
			}
			if (StructName == TEXT("Vector2D"))
			{
				return 120.f;
			}
		}
		return 0.f;
	}

	FMetrics ComputeMetrics(const UEdGraphNode* Node)
	{
		FMetrics Metrics;

		if (Node->NodeWidth > 0 && Node->NodeHeight > 0)
		{
			// Resizable node (e.g. comment) with a known size
			Metrics.Size = FVector2D(Node->NodeWidth, Node->NodeHeight);
			return Metrics;
		}

		const UK2Node* K2Node = Cast<UK2Node>(Node);
		// Reroute knots draw as a small dot (UK2Node::ShouldDrawAsBead is gone in 5.8; this exists in every version)
		int32 KnotInputPin = INDEX_NONE, KnotOutputPin = INDEX_NONE;
		if (Node->ShouldDrawNodeAsControlPointOnly(KnotInputPin, KnotOutputPin))
		{
			Metrics.Size = FVector2D(24.f, 24.f);
			Metrics.Header = 0.f;
			return Metrics;
		}

		float LeftWidth = 0.f;
		float RightWidth = 0.f;
		int32 NumInputs = 0;
		int32 NumOutputs = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsPinVisible(Node, Pin))
			{
				continue;
			}
			const float Width = 22.f + 6.5f * Node->GetPinDisplayName(Pin).ToString().Len() + DefaultValueWidgetWidth(Pin);
			if (Pin->Direction == EGPD_Input)
			{
				++NumInputs;
				LeftWidth = FMath::Max(LeftWidth, Width);
			}
			else
			{
				++NumOutputs;
				RightWidth = FMath::Max(RightWidth, Width);
			}
		}
		const int32 NumRows = FMath::Max3(NumInputs, NumOutputs, 1);
		const float PinsWidth = LeftWidth + RightWidth + 30.f;

		if (K2Node && K2Node->DrawNodeAsVariable())
		{
			// Variable getter "pill": no header, just the pin(s)
			Metrics.Header = 0.f;
			Metrics.Size = FVector2D(PinsWidth, NumRows * PinRowH + 6.f);
			return Metrics;
		}

		if (K2Node && K2Node->ShouldDrawCompact())
		{
			// Operator nodes such as +, -, ==
			Metrics.Header = 8.f;
			Metrics.Size = FVector2D(FMath::Max(110.f, PinsWidth + 30.f), NumRows * PinRowH + 16.f);
			return Metrics;
		}

		const FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		TArray<FString> TitleLines;
		Title.ParseIntoArrayLines(TitleLines);
		int32 LongestTitleLine = 0;
		for (const FString& Line : TitleLines)
		{
			LongestTitleLine = FMath::Max(LongestTitleLine, Line.Len());
		}

		Metrics.Header = HeaderH + 14.f * FMath::Max(0, TitleLines.Num() - 1);
		Metrics.Size.X = FMath::Max3(120.f, PinsWidth, 7.5f * LongestTitleLine + 50.f);
		Metrics.Size.Y = Metrics.Header + NumRows * PinRowH + 10.f;
		return Metrics;
	}

	FVector2D NodePos(const UEdGraphNode* Node)
	{
		return FVector2D(Node->NodePosX, Node->NodePosY);
	}

	void SetNodePos(UEdGraphNode* Node, const FVector2D& Pos)
	{
		Node->NodePosX = FMath::RoundToInt(Pos.X);
		Node->NodePosY = FMath::RoundToInt(Pos.Y);
	}

	FVector2D SnapToGrid(const FVector2D& Pos)
	{
		return FVector2D(FMath::GridSnap((float)Pos.X, GridSize), FMath::GridSnap((float)Pos.Y, GridSize));
	}

	/** Y offset (from the node's top) of the centre of a pin row, used to run wires straight. */
	float PinOffsetY(const UEdGraphNode* Node, const UEdGraphPin* Pin)
	{
		int32 Row = 0;
		for (const UEdGraphPin* Other : Node->Pins)
		{
			if (Other == Pin)
			{
				break;
			}
			if (Other && Other->Direction == Pin->Direction && IsPinVisible(Node, Other))
			{
				++Row;
			}
		}
		return ComputeMetrics(Node).Header + Row * PinRowH + PinRowH * 0.5f;
	}

	bool RectsOverlap(const FVector2D& PosA, const FVector2D& SizeA, const FVector2D& PosB, const FVector2D& SizeB, float Margin)
	{
		return PosA.X < PosB.X + SizeB.X + Margin && PosB.X < PosA.X + SizeA.X + Margin
			&& PosA.Y < PosB.Y + SizeB.Y + Margin && PosB.Y < PosA.Y + SizeA.Y + Margin;
	}

	/**
	 * Lowest position at or below Desired where a node of the given size does not overlap another node
	 * (comment boxes are ignored: nodes are meant to sit inside them). Margin > 0 demands free space around
	 * the node, Margin < 0 tolerates a small overlap. Falls back to Desired when no free spot is found
	 * (overlap is then unavoidable).
	 */
	FVector2D FindFreeSpot(const UEdGraph* Graph, const FVector2D& Desired, const FVector2D& Size, const TSet<const UEdGraphNode*>& Ignore, float Margin)
	{
		struct FObstacle { FVector2D Pos; FVector2D Size; };
		TArray<FObstacle> Obstacles;
		for (const UEdGraphNode* Other : Graph->Nodes)
		{
			if (Other && !Ignore.Contains(Other) && !Other->IsA<UEdGraphNode_Comment>())
			{
				Obstacles.Add({ NodePos(Other), ComputeMetrics(Other).Size });
			}
		}

		FVector2D Pos = Desired;
		for (int32 Step = 0; Step < MaxSearchSteps; ++Step)
		{
			bool bHit = false;
			float PushTo = (float)Pos.Y;
			for (const FObstacle& Obstacle : Obstacles)
			{
				if (RectsOverlap(Pos, Size, Obstacle.Pos, Obstacle.Size, Margin))
				{
					bHit = true;
					PushTo = FMath::Max(PushTo, (float)(Obstacle.Pos.Y + Obstacle.Size.Y) + VGap);
				}
			}
			if (!bHit)
			{
				return Pos;
			}
			Pos.Y = FMath::CeilToFloat(PushTo / GridSize) * GridSize;
		}
		return Desired;
	}

	UEdGraphNode* FindNodeByGuid(const UEdGraph* Graph, const FGuid& Guid)
	{
		if (Guid.IsValid())
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == Guid)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	/** Left edge and bottom edge of everything in the graph except Ignore. Returns false for an empty graph. */
	bool GetGraphExtents(const UEdGraph* Graph, const UEdGraphNode* Ignore, float& OutMinX, float& OutMaxBottom)
	{
		bool bAny = false;
		OutMinX = 0.f;
		OutMaxBottom = 0.f;
		for (const UEdGraphNode* Other : Graph->Nodes)
		{
			if (!Other || Other == Ignore)
			{
				continue;
			}
			const FVector2D Pos = NodePos(Other);
			const float Bottom = (float)(Pos.Y + ComputeMetrics(Other).Size.Y);
			OutMinX = bAny ? FMath::Min(OutMinX, (float)Pos.X) : (float)Pos.X;
			OutMaxBottom = bAny ? FMath::Max(OutMaxBottom, Bottom) : Bottom;
			bAny = true;
		}
		return bAny;
	}

	/**
	 * Reroute nodes are only wire waypoints, so when a node ends up in front of one of them the reroute
	 * gives way: every pre-existing reroute node wired to Node's outputs that is not clearly to its right
	 * is moved right of it, in line with the connected pin.
	 */
	void PushRerouteNodesRight(const FMCPEditorContext& Context, const UEdGraphNode* Node)
	{
		const FVector2D Pos = NodePos(Node);
		const FVector2D Size = ComputeMetrics(Node).Size;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				UEdGraphNode* Knot = Linked ? Cast<UK2Node_Knot>(Linked->GetOwningNode()) : nullptr;
				if (!Knot || Context.AutoPlacedNodes.Contains(Knot->NodeGuid) || NodePos(Knot).X >= Pos.X + Size.X + HGap * 0.5f)
				{
					continue;
				}
				SetNodePos(Knot, SnapToGrid(FVector2D(Pos.X + Size.X + HGap, Pos.Y + PinOffsetY(Node, Pin) - PinOffsetY(Knot, Linked))));
			}
		}
	}

	/**
	 * Nodes that must move together with Mover: auto-placed pure nodes feeding it (transitively) and, for flow nodes,
	 * the auto-placed nodes chained behind its exec outputs together with their pure inputs. Nodes in Visited
	 * (e.g. the node Mover was just wired to) are never touched.
	 */
	void CollectCluster(const FMCPEditorContext& Context, UEdGraphNode* Mover, TSet<UEdGraphNode*>& Visited, TArray<UEdGraphNode*>& OutCluster)
	{
		TArray<UEdGraphNode*> Stack;
		Visited.Add(Mover);
		OutCluster.Add(Mover);
		Stack.Add(Mover);

		while (Stack.Num() > 0)
		{
			UEdGraphNode* Current = Stack.Pop();
			const bool bCurrentPure = IsPureNode(Current);
			for (const UEdGraphPin* Pin : Current->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Neighbor = Linked ? Linked->GetOwningNode() : nullptr;
					if (!Neighbor || Visited.Contains(Neighbor) || !Context.AutoPlacedNodes.Contains(Neighbor->NodeGuid))
					{
						continue;
					}
					if (Pin->Direction == EGPD_Input)
					{
						if (!IsPureNode(Neighbor))
						{
							continue;
						}
					}
					else if (bCurrentPure || !IsExecPin(Pin))
					{
						// Data consumers keep their own place
						continue;
					}
					Visited.Add(Neighbor);
					OutCluster.Add(Neighbor);
					Stack.Add(Neighbor);
				}
			}
		}
	}
}

FVector2D FMCPNodeLayout::EstimateNodeSize(const UEdGraphNode* Node)
{
	return Node ? ComputeMetrics(Node).Size : FVector2D::ZeroVector;
}

void FMCPNodeLayout::ForgetNode(const UEdGraphNode* Node, FMCPEditorContext& Context)
{
	if (Node)
	{
		Context.AutoPlacedNodes.Remove(Node->NodeGuid);
	}
}

void FMCPNodeLayout::PlaceNewNode(UEdGraphNode* Node, FMCPEditorContext& Context, bool bExplicitPosition)
{
	UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;
	if (!Graph)
	{
		return;
	}

	const FVector2D Size = ComputeMetrics(Node).Size;
	const bool bPure = IsPureNode(Node);
	FVector2D Desired = NodePos(Node);
	float Margin;

	if (bExplicitPosition)
	{
		// Respect the requested spot; only step aside when clearly overlapping something
		Margin = -8.f;
		Context.AutoPlacedNodes.Remove(Node->NodeGuid);
	}
	else
	{
		Margin = 8.f;
		Context.AutoPlacedNodes.Add(Node->NodeGuid, false);

		UEdGraphNode* Anchor = nullptr;
		if (!IsEntryNode(Node))
		{
			Anchor = FindNodeByGuid(Graph, Context.LastLayoutAnchorId);
			if (!Anchor)
			{
				for (UEdGraphNode* Candidate : Graph->Nodes)
				{
					if (Cast<UK2Node_FunctionEntry>(Candidate))
					{
						Anchor = Candidate;
						break;
					}
				}
			}
		}

		if (Anchor)
		{
			const FVector2D AnchorPos = NodePos(Anchor);
			const FVector2D AnchorSize = ComputeMetrics(Anchor).Size;
			// Flow nodes continue the row; data nodes wait below it until they are wired
			Desired = bPure
				? FVector2D(AnchorPos.X, AnchorPos.Y + AnchorSize.Y + 2.f * VGap)
				: FVector2D(AnchorPos.X + AnchorSize.X + HGap, AnchorPos.Y);
		}
		else
		{
			// Entry node (or nothing to continue): start a new row below the existing content
			float MinX, MaxBottom;
			Desired = GetGraphExtents(Graph, Node, MinX, MaxBottom) ? FVector2D(MinX, MaxBottom + RowGap) : FVector2D::ZeroVector;
		}
	}

	TSet<const UEdGraphNode*> Ignore;
	Ignore.Add(Node);
	SetNodePos(Node, FindFreeSpot(Graph, SnapToGrid(Desired), Size, Ignore, Margin));

	if (!bPure)
	{
		Context.LastLayoutAnchorId = Node->NodeGuid;
	}
}

void FMCPNodeLayout::OnPinsConnected(UEdGraphPin* OutputPin, UEdGraphPin* InputPin, FMCPEditorContext& Context)
{
	UEdGraphNode* Source = OutputPin ? OutputPin->GetOwningNode() : nullptr;
	UEdGraphNode* Target = InputPin ? InputPin->GetOwningNode() : nullptr;
	if (!Source || !Target || Source == Target)
	{
		return;
	}
	UEdGraph* Graph = Source->GetGraph();
	if (!Graph || Graph != Target->GetGraph())
	{
		return;
	}

	const bool* SourceState = Context.AutoPlacedNodes.Find(Source->NodeGuid);
	const bool* TargetState = Context.AutoPlacedNodes.Find(Target->NodeGuid);
	const bool bSourceAuto = SourceState != nullptr;
	const bool bTargetAuto = TargetState != nullptr;
	const bool bSourceSettled = bSourceAuto && *SourceState;
	if (!bSourceAuto && !bTargetAuto)
	{
		return;
	}

	// A node that is already in place keeps its spot; a reroute node it is wired to moves out of its way
	if (bSourceSettled && !bTargetAuto && Cast<UK2Node_Knot>(Target))
	{
		PushRerouteNodesRight(Context, Source);
		return;
	}

	const bool bSourcePure = IsPureNode(Source);
	const FVector2D SourcePos = NodePos(Source);
	const FVector2D SourceSize = ComputeMetrics(Source).Size;
	const FVector2D TargetPos = NodePos(Target);

	// The wire should run left to right with some room in between
	const bool bBackwards = TargetPos.X < SourcePos.X + SourceSize.X + HGap * 0.5f;

	UEdGraphNode* Mover = nullptr;
	UEdGraphNode* Fixed = nullptr;
	FVector2D Desired;
	if (bTargetAuto && !(bSourceAuto && bSourcePure))
	{
		// Flow: put the target right of its source
		if (!bBackwards)
		{
			return;
		}
		Mover = Target;
		Fixed = Source;
		Desired.X = SourcePos.X + SourceSize.X + HGap;
		Desired.Y = SourcePos.Y + PinOffsetY(Source, OutputPin) - PinOffsetY(Target, InputPin);
	}
	else if (bSourceAuto)
	{
		// Data (or flow into a node that is not ours): put the source left of its consumer.
		// A pure source that is still waiting where it was staged is always pulled next to its first consumer.
		if (!bBackwards && (!bSourcePure || bSourceSettled))
		{
			return;
		}
		Mover = Source;
		Fixed = Target;
		Desired.X = TargetPos.X - HGap - SourceSize.X;
		Desired.Y = TargetPos.Y + PinOffsetY(Target, InputPin) - PinOffsetY(Source, OutputPin);
	}
	else
	{
		return;
	}

	TSet<UEdGraphNode*> Visited;
	Visited.Add(Fixed);
	TArray<UEdGraphNode*> Cluster;
	CollectCluster(Context, Mover, Visited, Cluster);

	TSet<const UEdGraphNode*> Ignore;
	for (const UEdGraphNode* Member : Cluster)
	{
		Ignore.Add(Member);
	}

	const FVector2D MoverPos = NodePos(Mover);
	const FVector2D Final = FindFreeSpot(Graph, SnapToGrid(Desired), ComputeMetrics(Mover).Size, Ignore, 8.f);
	const FVector2D Delta = Final - MoverPos;
	for (UEdGraphNode* Member : Cluster)
	{
		SetNodePos(Member, NodePos(Member) + Delta);
	}
	for (const UEdGraphNode* Member : Cluster)
	{
		PushRerouteNodesRight(Context, Member);
	}

	Context.AutoPlacedNodes.Add(Mover->NodeGuid, true);
}
