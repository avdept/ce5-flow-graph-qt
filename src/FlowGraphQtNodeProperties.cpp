// Copyright 2026 ProdigyTech Inc. All rights reserved.

#include "StdAfx.h"
#include "FlowGraphQtNodeProperties.h"

#include "FlowGraphQtViewModel.h"

#include "IEditorImpl.h"
#include "IUndoObject.h"
#include <IObjectManager.h>
#include "Objects/EntityObject.h"

#include "HyperGraph/HyperGraphNode.h"
#include "HyperGraph/HyperGraph.h"
#include "HyperGraph/FlowGraphNode.h"
#include "HyperGraph/FlowGraphManager.h"
#include "HyperGraph/IHyperGraph.h"
#include <Util/Variable.h>

#include <QAdvancedPropertyTreeLegacy.h>
#include <CrySerialization/IArchive.h>

#include <Controls/QPopupWidget.h>
#include <Controls/DictionaryWidget.h>

#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QVBoxLayout>
#include <QWidget>

#include <set>

namespace FlowGraphQt
{

void SFlowNodeInspector::Serialize(Serialization::IArchive& ar)
{
	if (pNode && pNode->CheckFlag(EHYPER_NODE_ENTITY))
	{
		string entity = static_cast<CFlowNode*>(pNode)->GetEntityTitle().GetString();
		ar(entity, "entity", "!Entity");
	}

	if (pPorts)
		pPorts->Serialize(ar);
}

CFlowGraphNodePropertiesWidget::CFlowGraphNodePropertiesWidget(CHyperNode& node)
	: m_node(node)
	, m_pTree(nullptr)
{
	m_inspector.pNode = &node;
	m_inspector.pPorts = new CVarBlock;
	const bool isEntityNode = node.CheckFlag(EHYPER_NODE_ENTITY);
	if (CHyperNode::Ports* pInputs = node.GetInputs())
	{
		for (size_t i = 0; i < pInputs->size(); ++i)
		{
			if (isEntityNode && i == 0)
				continue;

			IVariable* pVar = (*pInputs)[i].pVar;
			if (pVar && pVar->GetType() != IVariable::UNKNOWN)
				m_inspector.pPorts->AddVariable(pVar);
		}
	}

	QVBoxLayout* pLayout = new QVBoxLayout(this);
	pLayout->setContentsMargins(0, 0, 0, 0);

	m_pTree = new QAdvancedPropertyTreeLegacy("FlowGraph Node");
	m_pTree->setExpandLevels(4);
	m_pTree->setValueColumnWidth(0.6f);
	m_pTree->setAutoRevert(false);
	m_pTree->setSizeToContent(true);

	m_pTree->attach(Serialization::SStruct(m_inspector));

	connect(m_pTree, &QPropertyTreeLegacy::signalBeginUndo, this, &CFlowGraphNodePropertiesWidget::OnBeginUndo);
	connect(m_pTree, &QPropertyTreeLegacy::signalEndUndo, this, &CFlowGraphNodePropertiesWidget::OnEndUndo);
	connect(m_pTree, &QPropertyTreeLegacy::signalChanged, this, &CFlowGraphNodePropertiesWidget::OnChanged);

	pLayout->addWidget(m_pTree);
}

CFlowGraphNodePropertiesWidget::~CFlowGraphNodePropertiesWidget()
{
}

void CFlowGraphNodePropertiesWidget::OnBeginUndo()
{
	GetIEditorImpl()->GetIUndoManager()->Begin();
	m_node.RecordUndo();
}

void CFlowGraphNodePropertiesWidget::OnChanged()
{
	m_node.OnInputsChanged();
	m_node.Invalidate(true, true);
}

void CFlowGraphNodePropertiesWidget::OnEndUndo(bool undoAccepted)
{
	if (undoAccepted)
		GetIEditorImpl()->GetIUndoManager()->Accept("Change FlowGraph Node Input");
	else
		GetIEditorImpl()->GetIUndoManager()->Cancel();
}

bool CFlowGraphView::event(QEvent* pEvent)
{
	if (pEvent->type() == QEvent::KeyPress)
	{
		QKeyEvent* pKeyEvent = static_cast<QKeyEvent*>(pEvent);
		if (pKeyEvent->key() == Qt::Key_Tab && pKeyEvent->modifiers() == Qt::NoModifier)
		{
			const QPoint cursor = QCursor::pos();
			m_openAddNodeFromTab = true;
			ShowGraphContextMenu(QPointF(cursor.x(), cursor.y()));
			return true;
		}
	}

	return CryGraphEditor::CNodeGraphView::event(pEvent);
}

bool CFlowGraphView::eventFilter(QObject* pWatched, QEvent* pEvent)
{
	if (pEvent->type() == QEvent::KeyPress)
	{
		const QKeyEvent* pKeyEvent = static_cast<QKeyEvent*>(pEvent);
		if (pKeyEvent->key() == Qt::Key_Escape)
		{
			if (QPopupWidget* pPopup = GetContextMenu())
			{
				if (pPopup->isVisible())
				{
					pPopup->hide();
					return true;
				}
			}
		}
	}

	return CryGraphEditor::CNodeGraphView::eventFilter(pWatched, pEvent);
}

void CFlowGraphView::ShowGraphContextMenu(QPointF screenPos)
{
	const bool fromConnectionDrag = (GetAction() == eAction_ConnectionCreation);
	if (!m_openAddNodeFromTab && !fromConnectionDrag)
		return;
	m_openAddNodeFromTab = false;

	if (GetAction() == eAction_AddNodeMenu)
		AbortAction();

	CryGraphEditor::CNodeGraphView::ShowGraphContextMenu(screenPos);

	if (!m_contextMenuFilterInstalled)
	{
		if (CDictionaryWidget* pContent = GetContextMenuContent())
		{
			pContent->installEventFilter(this);
			for (QWidget* pChild : pContent->findChildren<QWidget*>())
				pChild->installEventFilter(this);

			m_contextMenuFilterInstalled = true;
		}
	}
}

QWidget* CFlowGraphView::CreatePropertiesWidget(CryGraphEditor::GraphItemSet& selectedItems)
{
	if (selectedItems.size() == 1)
	{
		CryGraphEditor::CAbstractNodeGraphViewModelItem* pItem = *selectedItems.begin();
		if (CryGraphEditor::CAbstractNodeItem* pNodeItem = pItem->Cast<CryGraphEditor::CAbstractNodeItem>())
		{
			CFlowGraphNodeItem* pFlowItem = static_cast<CFlowGraphNodeItem*>(pNodeItem);
			return new CFlowGraphNodePropertiesWidget(pFlowItem->GetHyperNode());
		}
	}

	return CryGraphEditor::CNodeGraphView::CreatePropertiesWidget(selectedItems);
}

// Entity-target context menu

std::vector<CFlowGraphNodeItem*> CFlowGraphView::CollectEntityNodes(CFlowGraphNodeItem& clicked) const
{
	std::set<CFlowGraphNodeItem*> seen;
	std::vector<CFlowGraphNodeItem*> result;

	auto add = [&](CFlowGraphNodeItem* pItem)
	{
		if (pItem && pItem->GetHyperNode().CheckFlag(EHYPER_NODE_ENTITY) && seen.insert(pItem).second)
			result.push_back(pItem);
	};

	add(&clicked);

	for (CryGraphEditor::CAbstractNodeGraphViewModelItem* pItem : GetSelectedItems())
	{
		if (CryGraphEditor::CAbstractNodeItem* pNodeItem = pItem->Cast<CryGraphEditor::CAbstractNodeItem>())
			add(static_cast<CFlowGraphNodeItem*>(pNodeItem));
	}

	return result;
}

void CFlowGraphView::AssignEntities(const std::vector<CFlowGraphNodeItem*>& nodes, EEntityAssign mode)
{
	if (nodes.empty())
		return;

	CHyperGraph* pGraph = static_cast<CHyperGraph*>(nodes.front()->GetHyperNode().GetGraph());

	CUndo undo("Assign FlowGraph Node Entity");
	if (pGraph)
		pGraph->RecordUndo();

	for (CFlowGraphNodeItem* pItem : nodes)
	{
		CFlowNode* pFlowNode = static_cast<CFlowNode*>(&pItem->GetHyperNode());

		switch (mode)
		{
		case EEntityAssign::Selected: pFlowNode->SetSelectedEntity(); break;
		case EEntityAssign::Graph:    pFlowNode->SetDefaultEntity();  break;
		case EEntityAssign::Unassign: pFlowNode->SetEntity(nullptr);  break;
		}

		pFlowNode->Invalidate(true, true);
		GetIEditorImpl()->GetFlowGraphManager()->SendNotifyEvent(EHG_NODE_UPDATE_ENTITY, 0, pFlowNode);
	}

	if (pGraph)
	{
		pGraph->SetModified();
		pGraph->SendNotifyEvent(nullptr, EHG_NODE_UPDATE_ENTITY);
	}
}

void CFlowGraphView::SelectAssignedEntities(const std::vector<CFlowGraphNodeItem*>& nodes)
{
	CUndo undo("Select Object(s)");
	GetIEditorImpl()->GetObjectManager()->ClearSelection();

	for (CFlowGraphNodeItem* pItem : nodes)
	{
		CFlowNode* pFlowNode = static_cast<CFlowNode*>(&pItem->GetHyperNode());
		if (CEntityObject* pEntity = pFlowNode->GetEntity())
			GetIEditorImpl()->GetObjectManager()->AddObjectToSelection(pEntity);
	}
}

bool CFlowGraphView::PopulateNodeContextMenu(CryGraphEditor::CAbstractNodeItem& node, QMenu& menu)
{
	CFlowGraphPinItem* const pContextPin = m_pContextPin;
	m_pContextPin = nullptr;

	if (pContextPin && !pContextPin->IsEntityPort())
		return false;

	CFlowGraphNodeItem& clicked = static_cast<CFlowGraphNodeItem&>(node);

	const std::vector<CFlowGraphNodeItem*> entityNodes = CollectEntityNodes(clicked);
	if (entityNodes.empty())
		return false;

	const struct { const char* label; EEntityAssign mode; } assignActions[] =
	{
		{ QT_TR_NOOP("Assign Selected Entity"), EEntityAssign::Selected },
		{ QT_TR_NOOP("Assign Graph Entity"),    EEntityAssign::Graph    },
		{ QT_TR_NOOP("Unassign Entity"),        EEntityAssign::Unassign },
	};
	for (const auto& action : assignActions)
	{
		const EEntityAssign mode = action.mode;
		QObject::connect(menu.addAction(QObject::tr(action.label)), &QAction::triggered, this,
		                 [this, entityNodes, mode]() { AssignEntities(entityNodes, mode); });
	}

	QObject::connect(menu.addAction(QObject::tr("Select Assigned Entity")), &QAction::triggered, this,
	                 [this, entityNodes]() { SelectAssignedEntities(entityNodes); });

	return true;
}

}
