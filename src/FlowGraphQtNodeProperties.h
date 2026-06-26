// Copyright 2026 ProdigyTech Inc. All rights reserved.
//
// Node properties: a QPropertyTreeLegacy bound to a node's input ports (its
// IVariable-backed CVarBlock), shown in the editor's inspector when a node is
// selected. CFlowGraphView overrides the framework hook that builds it.

#pragma once

#include <NodeGraph/NodeGraphView.h>

#include <CryCore/smartptr.h>
#include <CrySerialization/Forward.h>

#include <QWidget>

#include <vector>

class CHyperNode;
class CVarBlock;
class QPropertyTreeLegacy;
class QMenu;
class QEvent;

namespace FlowGraphQt
{

class CFlowGraphNodeItem;
class CFlowGraphPinItem;

// Serialized into the inspector: a read-only "Entity" row (entity nodes only)
// showing the assigned target entity, followed by the node's editable ports.
struct SFlowNodeInspector
{
	CHyperNode*           pNode = nullptr;
	_smart_ptr<CVarBlock> pPorts; // editable ports (entity target port excluded)

	void Serialize(Serialization::IArchive& ar);
};

class CFlowGraphNodePropertiesWidget : public QWidget
{
public:
	explicit CFlowGraphNodePropertiesWidget(CHyperNode& node);
	virtual ~CFlowGraphNodePropertiesWidget();

private:
	void OnBeginUndo();
	void OnEndUndo(bool undoAccepted);
	void OnChanged();

	CHyperNode&          m_node;
	SFlowNodeInspector   m_inspector; // owns the ports var block; attached to the tree
	QPropertyTreeLegacy* m_pTree;
};

// CNodeGraphView subclass that builds the per-node properties widget on selection.
class CFlowGraphView : public CryGraphEditor::CNodeGraphView
{
public:
	using CryGraphEditor::CNodeGraphView::CNodeGraphView;

	// Tab opens the add-node search popup at the cursor (Qt's focus system eats
	// Tab before keyPressEvent, so we intercept it here).
	virtual bool     event(QEvent* pEvent) override;

	// Closes the add-node popup on Esc. Its search box has focus while open, so
	// the view never sees the key; filter it on the popup's widgets instead.
	virtual bool     eventFilter(QObject* pWatched, QEvent* pEvent) override;

	// Works around a framework quirk where a dismissed add-node menu leaves the
	// view's action stuck, blocking the menu (right-click and Tab) from reopening.
	virtual void     ShowGraphContextMenu(QPointF screenPos) override;

	virtual QWidget* CreatePropertiesWidget(CryGraphEditor::GraphItemSet& selectedItems) override;

	// Adds the entity-target commands ("Assign Selected Entity", etc.) for nodes
	// that carry the EHYPER_NODE_ENTITY flag, mirroring the legacy MFC menu.
	virtual bool     PopulateNodeContextMenu(CryGraphEditor::CAbstractNodeItem& node, QMenu& menu) override;

	// The port under the cursor when the context menu was requested (null = the
	// node body/title). The content widget records it so the menu can be
	// port-specific: entity commands only on the entity port (or the body).
	void             SetContextPin(CFlowGraphPinItem* pPin) { m_pContextPin = pPin; }

private:
	// How to set the target entity on the affected nodes.
	enum class EEntityAssign
	{
		Selected, // the entity currently selected in the level viewport
		Graph,    // the graph's default (owner) entity
		Unassign, // clear the target entity
	};

	// Clicked node plus any selected nodes that carry the entity flag (deduped).
	std::vector<CFlowGraphNodeItem*> CollectEntityNodes(CFlowGraphNodeItem& clicked) const;

	void                             AssignEntities(const std::vector<CFlowGraphNodeItem*>& nodes, EEntityAssign mode);
	void                             SelectAssignedEntities(const std::vector<CFlowGraphNodeItem*>& nodes);

	CFlowGraphPinItem*               m_pContextPin = nullptr;
	bool                             m_contextMenuFilterInstalled = false;
	bool                             m_openAddNodeFromTab = false; // gate: only Tab opens the canvas search
};

} // namespace FlowGraphQt
