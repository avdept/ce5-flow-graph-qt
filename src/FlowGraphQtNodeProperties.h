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
	_smart_ptr<CVarBlock> pPorts;

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
	SFlowNodeInspector   m_inspector;
	QPropertyTreeLegacy* m_pTree;
};

// CNodeGraphView subclass that builds the per-node properties widget on selection.
class CFlowGraphView : public CryGraphEditor::CNodeGraphView
{
public:
	using CryGraphEditor::CNodeGraphView::CNodeGraphView;

	virtual bool     event(QEvent* pEvent) override;

	virtual bool     eventFilter(QObject* pWatched, QEvent* pEvent) override;

	virtual void     ShowGraphContextMenu(QPointF screenPos) override;

	virtual QWidget* CreatePropertiesWidget(CryGraphEditor::GraphItemSet& selectedItems) override;

	virtual bool     PopulateNodeContextMenu(CryGraphEditor::CAbstractNodeItem& node, QMenu& menu) override;

	void             SetContextPin(CFlowGraphPinItem* pPin) { m_pContextPin = pPin; }

private:
	enum class EEntityAssign
	{
		Selected,
		Graph,
		Unassign,
	};

	std::vector<CFlowGraphNodeItem*> CollectEntityNodes(CFlowGraphNodeItem& clicked) const;

	void                             AssignEntities(const std::vector<CFlowGraphNodeItem*>& nodes, EEntityAssign mode);
	void                             SelectAssignedEntities(const std::vector<CFlowGraphNodeItem*>& nodes);

	CFlowGraphPinItem*               m_pContextPin = nullptr;
	bool                             m_contextMenuFilterInstalled = false;
	bool                             m_openAddNodeFromTab = false;
};

}
