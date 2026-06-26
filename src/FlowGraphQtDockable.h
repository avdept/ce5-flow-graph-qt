// Copyright 2026 ProdigyTech Inc. All rights reserved.
//
// FlowGraph host pane: pick a graph from the left "Flow Graphs" list and it is
// rendered (and edited) through the Qt NodeGraph framework (DPI-correct).
// Registered under Tools as "FlowGraph Qt".

#pragma once

#include "EditorFramework/Editor.h"

#include "HyperGraph/IHyperGraph.h" // IHyperGraphListener, EHyperGraphEvent

#include <QPoint>
#include <QString>

#include <memory>

class QSearchBox;
class QTreeWidget;
class QWidget;

class CHyperFlowGraph;
class CInspectorLegacy;

namespace FlowGraphQt
{
class CFlowGraphViewModel;
class CFlowGraphView;
}

// IHyperGraphListener: so undo/redo (which restores the graph and fires
// EHG_GRAPH_UNDO_REDO) rebuilds the Qt projection.
class CFlowGraphQtDockable : public CDockableEditor, public IHyperGraphListener
{
public:
	CFlowGraphQtDockable();
	virtual ~CFlowGraphQtDockable();

	virtual const char* GetEditorName() const override { return "FlowGraph Qt"; }

	// Drives drag-from-palette (source) and drop-on-canvas (target) to create nodes.
	virtual bool eventFilter(QObject* pWatched, QEvent* pEvent) override;

	// IHyperGraphListener
	virtual void OnHyperGraphEvent(IHyperNode* pNode, EHyperGraphEvent event) override;
	// ~IHyperGraphListener

private:
	void     RefreshGraphList();
	void     OpenGraphByIndex(size_t graphIndex);
	void     SetCurrentGraph(CHyperFlowGraph* pGraph, bool fitInView);
	void     RebuildModel(bool fitInView);
	void     SaveCurrentGraph();
	void     UpdateWindowTitle();

	// Menu + shortcut wiring (File/Edit menus, Ctrl+Z/Y, Del, Ctrl+S).
	void     RegisterActions();
	void     InitMenu();
	bool     OnUndo();
	bool     OnRedo();
	bool     OnDelete();
	bool     OnSave();

	QWidget* CreateLeftSidebar();
	QWidget* CreateNodePalette();
	void     RebuildNodePalette(const QString& filter);
	QWidget* CreateGraphList();

	FlowGraphQt::CFlowGraphView*                      m_pView;
	QTreeWidget*                                  m_pGraphTree;
	QTreeWidget*                                  m_pNodePalette;
	QSearchBox*                                   m_pPaletteSearch;
	CInspectorLegacy*                             m_pInspector;
	std::unique_ptr<FlowGraphQt::CFlowGraphViewModel> m_pModel;
	CHyperFlowGraph*                              m_pCurrentGraph;

	// Palette drag state.
	QPoint                                        m_dragStartPos;
	QString                                       m_dragClassName;
};
