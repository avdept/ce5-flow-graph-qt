// Copyright 2026 ProdigyTech Inc. All rights reserved.

#include "StdAfx.h"
#include "FlowGraphQtDockable.h"

#include "FlowGraphQtViewModel.h"
#include "FlowGraphQtNodeProperties.h" // CFlowGraphView

#include "IEditorImpl.h"
#include "IUndoObject.h" // CUndo
#include "HyperGraph/FlowGraphManager.h"
#include "HyperGraph/FlowGraph.h"
#include "HyperGraph/FlowGraphNode.h"
#include "Objects/EntityObject.h" // CEntityObject (entity-graph labels)

#include <EditorFramework/InspectorLegacy.h>

#include <QCollapsibleFrame.h>
#include <QSearchBox.h>
#include <QtUtil.h>

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

// Opt-in pane under Tools: a DPI-correct Qt FlowGraph editor alongside the
// legacy MFC one.
REGISTER_VIEWPANE_FACTORY(CFlowGraphQtDockable, "FlowGraph Qt", "Tools", false)

namespace
{
// Mime type carrying a flownode class name when dragging from the palette.
const char* const kNodeClassMime = "application/x-cryengine-flowgraph-nodeclass";

// Category palette items store their plain name here so we can re-prefix a
// +/- expand indicator. Leaf (node) items instead use Qt::UserRole for the
// class name, so a valid value in this role marks an item as a category.
const int kCategoryNameRole = Qt::UserRole + 1;

void SetCategoryGlyph(QTreeWidgetItem* pItem, bool expanded)
{
	const QVariant name = pItem->data(0, kCategoryNameRole);
	if (name.isValid())
		pItem->setText(0, (expanded ? QStringLiteral("- ") : QStringLiteral("+ ")) + name.toString());
}

// Stylesheet ::item padding only takes effect on hover (the theme owns the
// normal state), so pad in a delegate instead: the style paints the full-width
// highlight, we draw the text inset and bump the row height.
class CPaddedItemDelegate : public QStyledItemDelegate
{
public:
	explicit CPaddedItemDelegate(QObject* pParent) : QStyledItemDelegate(pParent) {}

	virtual QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
	{
		QSize size = QStyledItemDelegate::sizeHint(option, index);
		size.setHeight(option.fontMetrics.height() + 8);
		return size;
	}

	virtual void paint(QPainter* pPainter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
	{
		const int leftPad = 10;
		const int rightPad = 4;

		QStyleOptionViewItem opt(option);
		initStyleOption(&opt, index);

		// Let the style draw the full-width background (selection/hover/etc.) with no text.
		const QString text = opt.text;
		opt.text.clear();
		const QWidget* pWidget = opt.widget;
		QStyle* pStyle = pWidget ? pWidget->style() : QApplication::style();
		pStyle->drawControl(QStyle::CE_ItemViewItem, &opt, pPainter, pWidget);

		// Draw the text inset, so the highlight stays full width but content is padded.
		QRect textRect = opt.rect.adjusted(leftPad, 0, -rightPad, 0);
		const QPalette::ColorGroup cg = (opt.state & QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled;
		const QPalette::ColorRole cr = (opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text;

		pPainter->save();
		pPainter->setPen(opt.palette.color(cg, cr));
		const QString elided = opt.fontMetrics.elidedText(text, Qt::ElideRight, textRect.width());
		pPainter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
		pPainter->restore();
	}
};
} // anonymous namespace

CFlowGraphQtDockable::CFlowGraphQtDockable()
	: CDockableEditor()
	, m_pView(nullptr)
	, m_pGraphTree(nullptr)
	, m_pNodePalette(nullptr)
	, m_pPaletteSearch(nullptr)
	, m_pInspector(nullptr)
	, m_pCurrentGraph(nullptr)
{
	// Center: toolbar + the node graph view.
	QWidget* pGraphSide = new QWidget();
	QVBoxLayout* pLayout = new QVBoxLayout(pGraphSide);
	pLayout->setContentsMargins(0, 0, 0, 0);

	QHBoxLayout* pTopBar = new QHBoxLayout();
	QPushButton* pRefresh = new QPushButton(tr("Refresh"));
	QPushButton* pSave = new QPushButton(tr("Save"));

	pTopBar->addStretch(1);
	pTopBar->addWidget(pRefresh);
	pTopBar->addWidget(pSave);
	pLayout->addLayout(pTopBar);

	m_pView = new FlowGraphQt::CFlowGraphView();
	m_pView->setAcceptDrops(true);
	m_pView->viewport()->setAcceptDrops(true);
	m_pView->viewport()->installEventFilter(this); // drop target for palette drags
	pLayout->addWidget(m_pView, 1);

	QObject::connect(pRefresh, &QPushButton::clicked, [this]() { RefreshGraphList(); });
	QObject::connect(pSave, &QPushButton::clicked, [this]() { SaveCurrentGraph(); });

	// Right side: properties inspector. The graph view broadcasts the selected
	// node's properties widget (CFlowGraphView::CreatePropertiesWidget) to it.
	m_pInspector = new CInspectorLegacy(this);

	// Left: nodes + flow-graph list | center: graph | right: properties inspector.
	QSplitter* pSplitter = new QSplitter(Qt::Horizontal);
	pSplitter->addWidget(CreateLeftSidebar());
	pSplitter->addWidget(pGraphSide);
	pSplitter->addWidget(m_pInspector);
	pSplitter->setStretchFactor(0, 0);
	pSplitter->setStretchFactor(1, 1);
	pSplitter->setStretchFactor(2, 0);
	pSplitter->setSizes({ 250, 700, 320 });

	SetContent(pSplitter);

	RegisterActions();
	InitMenu();

	RebuildNodePalette(QString());
	RefreshGraphList();
}

void CFlowGraphQtDockable::RegisterActions()
{
	RegisterAction("general.undo", &CFlowGraphQtDockable::OnUndo);
	RegisterAction("general.redo", &CFlowGraphQtDockable::OnRedo);
	RegisterAction("general.delete", &CFlowGraphQtDockable::OnDelete);
	RegisterAction("general.save", &CFlowGraphQtDockable::OnSave);
}

void CFlowGraphQtDockable::InitMenu()
{
	AddToMenu({ CEditor::MenuItems::FileMenu, CEditor::MenuItems::Save,
	            CEditor::MenuItems::EditMenu, CEditor::MenuItems::Undo,
	            CEditor::MenuItems::Redo, CEditor::MenuItems::Delete });
}

bool CFlowGraphQtDockable::OnUndo()
{
	GetIEditorImpl()->GetIUndoManager()->Undo();
	return true;
}

bool CFlowGraphQtDockable::OnRedo()
{
	GetIEditorImpl()->GetIUndoManager()->Redo();
	return true;
}

bool CFlowGraphQtDockable::OnDelete()
{
	if (m_pView)
		m_pView->OnDeleteEvent();
	return true;
}

bool CFlowGraphQtDockable::OnSave()
{
	SaveCurrentGraph();
	return true;
}

void CFlowGraphQtDockable::UpdateWindowTitle()
{
	if (!m_pCurrentGraph)
	{
		setWindowTitle(GetEditorName());
		return;
	}

	QString name = QtUtil::ToQString(m_pCurrentGraph->GetName());
	if (name.isEmpty())
		name = QtUtil::ToQString(GetEditorName());

	setWindowTitle(m_pCurrentGraph->IsModified() ? (name + QStringLiteral(" *")) : name);
}

QWidget* CFlowGraphQtDockable::CreateLeftSidebar()
{
	// Two stacked, resizable sections: the node palette on top, the flow-graph
	// list below.
	QSplitter* pSidebar = new QSplitter(Qt::Vertical);
	pSidebar->addWidget(CreateNodePalette());
	pSidebar->addWidget(CreateGraphList());
	pSidebar->setStretchFactor(0, 3);
	pSidebar->setStretchFactor(1, 2);
	pSidebar->setSizes({ 420, 260 });
	return pSidebar;
}

QWidget* CFlowGraphQtDockable::CreateNodePalette()
{
	QWidget* pContent = new QWidget();
	QVBoxLayout* pLayout = new QVBoxLayout(pContent);
	pLayout->setContentsMargins(0, 0, 0, 0);

	// QSearchBox is the framework's unified search field (don't use a raw QLineEdit).
	m_pPaletteSearch = new QSearchBox();
	m_pPaletteSearch->setPlaceholderText(tr("Filter nodes..."));
	m_pPaletteSearch->EnableContinuousSearch(true);
	m_pPaletteSearch->SetSearchFunction([this](const QString& text) { RebuildNodePalette(text); });
	pLayout->addWidget(m_pPaletteSearch);

	m_pNodePalette = new QTreeWidget();
	m_pNodePalette->setHeaderHidden(true);
	m_pNodePalette->setSortingEnabled(false);
	m_pNodePalette->setRootIsDecorated(false);       // we draw our own +/- indicator
	m_pNodePalette->setExpandsOnDoubleClick(false);  // avoid double-toggle fighting our click handler
	// Theme-proof per-row padding (full-width highlight, inset content).
	m_pNodePalette->setItemDelegate(new CPaddedItemDelegate(m_pNodePalette));

	// Drag source: we start the drag manually from the viewport (see eventFilter).
	m_pNodePalette->viewport()->installEventFilter(this);
	pLayout->addWidget(m_pNodePalette, 1);

	// +/- expand indicator on category rows, and toggle a group by clicking it.
	QObject::connect(m_pNodePalette, &QTreeWidget::itemExpanded, [](QTreeWidgetItem* pItem) { SetCategoryGlyph(pItem, true); });
	QObject::connect(m_pNodePalette, &QTreeWidget::itemCollapsed, [](QTreeWidgetItem* pItem) { SetCategoryGlyph(pItem, false); });
	QObject::connect(m_pNodePalette, &QTreeWidget::itemClicked, [](QTreeWidgetItem* pItem, int)
	{
		if (pItem->data(0, kCategoryNameRole).isValid())
			pItem->setExpanded(!pItem->isExpanded());
	});

	// Collapsible, titled section (the framework's grouping component).
	QCollapsibleFrame* pFrame = new QCollapsibleFrame(tr("Nodes"));
	pFrame->SetClosable(false);
	pFrame->SetWidget(pContent);
	return pFrame;
}

void CFlowGraphQtDockable::RebuildNodePalette(const QString& filter)
{
	if (!m_pNodePalette)
		return;

	// Split + removeAll(empty) instead of QString::SkipEmptyParts, which is
	// removed in Qt6 (and Qt::SkipEmptyParts doesn't exist until Qt 5.14).
	QStringList keywords = filter.toLower().split(QLatin1Char(' '));
	keywords.removeAll(QString());

	m_pNodePalette->clear();

	// Map an accumulated "A:B:" path to its tree item so categories are shared.
	std::map<QString, QTreeWidgetItem*> categories;

	for (const FlowGraphQt::SFlowNodeClass& cls : FlowGraphQt::EnumerateFlowNodeClasses())
	{
		if (!keywords.isEmpty())
		{
			const QString haystack = cls.uiName.toLower();
			bool matchesAll = true;
			for (const QString& word : keywords)
			{
				if (!haystack.contains(word))
				{
					matchesAll = false;
					break;
				}
			}
			if (!matchesAll)
				continue;
		}

		QTreeWidgetItem* pParent = nullptr;
		QString path;
		for (const QString& category : cls.categoryPath)
		{
			path += category;
			path += QLatin1Char(':');

			auto it = categories.find(path);
			if (it == categories.end())
			{
				QTreeWidgetItem* pCategoryItem = pParent
				                                 ? new QTreeWidgetItem(pParent)
				                                 : new QTreeWidgetItem(m_pNodePalette);
				pCategoryItem->setData(0, kCategoryNameRole, category);
				SetCategoryGlyph(pCategoryItem, false);
				categories[path] = pCategoryItem;
				pParent = pCategoryItem;
			}
			else
			{
				pParent = it->second;
			}
		}

		QTreeWidgetItem* pLeafItem = pParent
		                             ? new QTreeWidgetItem(pParent, QStringList(cls.leaf))
		                             : new QTreeWidgetItem(m_pNodePalette, QStringList(cls.leaf));
		pLeafItem->setData(0, Qt::UserRole, cls.className); // class name for node creation
		pLeafItem->setToolTip(0, cls.uiName);
	}

	// When filtering, expand so matches are visible; otherwise keep it collapsed.
	if (!keywords.isEmpty())
		m_pNodePalette->expandAll();
}

bool CFlowGraphQtDockable::eventFilter(QObject* pWatched, QEvent* pEvent)
{
	// --- Drag source: the palette viewport ---
	if (m_pNodePalette && pWatched == m_pNodePalette->viewport())
	{
		if (pEvent->type() == QEvent::MouseButtonPress)
		{
			QMouseEvent* pMouse = static_cast<QMouseEvent*>(pEvent);
			if (pMouse->button() == Qt::LeftButton)
			{
				m_dragStartPos = pMouse->pos(); // Qt6: pMouse->position().toPoint()
				QTreeWidgetItem* pItem = m_pNodePalette->itemAt(pMouse->pos());
				m_dragClassName = pItem ? pItem->data(0, Qt::UserRole).toString() : QString();
			}
		}
		else if (pEvent->type() == QEvent::MouseMove)
		{
			QMouseEvent* pMouse = static_cast<QMouseEvent*>(pEvent);
			if ((pMouse->buttons() & Qt::LeftButton) && !m_dragClassName.isEmpty()
			    && (pMouse->pos() - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance())
			{
				QMimeData* pMime = new QMimeData();
				pMime->setData(kNodeClassMime, m_dragClassName.toUtf8());

				QDrag* pDrag = new QDrag(this);
				pDrag->setMimeData(pMime);
				pDrag->exec(Qt::CopyAction);

				m_dragClassName.clear();
				return true; // consumed, so it doesn't become a click/selection
			}
		}
	}

	// --- Drop target: the graph view viewport ---
	if (m_pView && pWatched == m_pView->viewport())
	{
		// QDragEnterEvent derives from QDragMoveEvent.
		if (pEvent->type() == QEvent::DragEnter || pEvent->type() == QEvent::DragMove)
		{
			QDragMoveEvent* pDrag = static_cast<QDragMoveEvent*>(pEvent);
			if (pDrag->mimeData()->hasFormat(kNodeClassMime))
			{
				pDrag->acceptProposedAction();
				return true;
			}
		}
		else if (pEvent->type() == QEvent::Drop)
		{
			QDropEvent* pDrop = static_cast<QDropEvent*>(pEvent);
			if (m_pModel && pDrop->mimeData()->hasFormat(kNodeClassMime))
			{
				const QString className = QString::fromUtf8(pDrop->mimeData()->data(kNodeClassMime));
				const QPointF scenePos = m_pView->mapToScene(pDrop->pos()); // Qt6: pDrop->position().toPoint()
				// Drag-create isn't bracketed by the NodeGraph view (unlike connect/delete),
				// so open the undo step ourselves or RecordUndo() inside CreateNode is a no-op.
				{
					CUndo undo("Create FlowGraph Node");
					m_pModel->CreateNode(QVariant(className), scenePos);
				}
				pDrop->acceptProposedAction();
				return true;
			}
		}
	}

	return CDockableEditor::eventFilter(pWatched, pEvent);
}

CFlowGraphQtDockable::~CFlowGraphQtDockable()
{
	if (m_pCurrentGraph)
		m_pCurrentGraph->RemoveListener(this);

	if (m_pView)
		m_pView->SetModel(nullptr);
}

void CFlowGraphQtDockable::RefreshGraphList()
{
	if (!m_pGraphTree)
		return;

	CFlowGraphManager* pManager = GetIEditorImpl()->GetFlowGraphManager();
	if (!pManager)
		return;

	m_pGraphTree->clear();

	// Fixed top-level folders, always present even when empty - mirrors the legacy
	// CHyperGraphsTreeCtrl::FullReload (which creates every type folder up front).
	QTreeWidgetItem* pEntities = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("Entities")));
	QTreeWidgetItem* pPrefabs  = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("Prefabs")));
	QTreeWidgetItem* pModules  = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("FG Modules")));
	QTreeWidgetItem* pMatFx    = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("Material FX")));
	QTreeWidgetItem* pUIAction = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("UI Actions")));
	QTreeWidgetItem* pAIAction = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("AI Actions")));
	QTreeWidgetItem* pCustom   = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("Custom Actions")));
	QTreeWidgetItem* pFiles    = new QTreeWidgetItem(m_pGraphTree, QStringList(tr("Files")));

	std::map<CEntityObject*, QTreeWidgetItem*> entityFolders;
	QTreeWidgetItem* pFirstLeaf = nullptr;

	auto addLeaf = [&](QTreeWidgetItem* pParent, QString label, size_t graphIndex) -> QTreeWidgetItem*
	{
		if (label.isEmpty())
			label = tr("<unnamed>");
		QTreeWidgetItem* pLeaf = new QTreeWidgetItem(pParent, QStringList(label));
		pLeaf->setData(0, Qt::UserRole, static_cast<qulonglong>(graphIndex));
		if (!pFirstLeaf)
			pFirstLeaf = pLeaf;
		return pLeaf;
	};

	const size_t count = pManager->GetFlowGraphCount();
	for (size_t i = 0; i < count; ++i)
	{
		CHyperFlowGraph* pGraph = pManager->GetFlowGraph(i);
		if (!pGraph)
			continue;

		IFlowGraph* pRuntime = pGraph->GetIFlowGraph();
		const IFlowGraph::EFlowGraphType type = pRuntime ? pRuntime->GetType() : IFlowGraph::eFGT_Default;
		const QString graphName = QtUtil::ToQString(pGraph->GetName());

		switch (type)
		{
		case IFlowGraph::eFGT_AIAction:     addLeaf(pAIAction, graphName, i); break;
		case IFlowGraph::eFGT_UIAction:     addLeaf(pUIAction, graphName, i); break;
		case IFlowGraph::eFGT_CustomAction: addLeaf(pCustom, graphName, i);   break;
		case IFlowGraph::eFGT_MaterialFx:   addLeaf(pMatFx, graphName, i);    break;
		case IFlowGraph::eFGT_Module:       addLeaf(pModules, graphName, i);  break;
		case IFlowGraph::eFGT_Default:
		default:
			if (CEntityObject* pEntity = pGraph->GetEntity())
			{
				// An entity can own several flow graphs, distinguished by their group
				// name (GetName() just returns the entity name). So make a folder per
				// entity and label each leaf with the graph's own (group) name.
				QTreeWidgetItem* pParent = pEntity->GetPrefab() ? pPrefabs : pEntities;

				auto it = entityFolders.find(pEntity);
				QTreeWidgetItem* pEntityFolder;
				if (it != entityFolders.end())
				{
					pEntityFolder = it->second;
				}
				else
				{
					QString entityName = QtUtil::ToQString(pEntity->GetName());
					if (entityName.isEmpty())
						entityName = tr("<unnamed entity>");
					pEntityFolder = new QTreeWidgetItem(pParent, QStringList(entityName));
					entityFolders[pEntity] = pEntityFolder;
				}

				QString graphLabel = QtUtil::ToQString(pGraph->GetGroupName());
				if (graphLabel.isEmpty())
					graphLabel = tr("Default"); // the entity's single/unnamed graph
				addLeaf(pEntityFolder, graphLabel, i);
			}
			else
			{
				addLeaf(pFiles, graphName, i);
			}
			break;
		}
	}

	m_pGraphTree->expandAll();

	// Open the first graph so the editor isn't blank on load.
	if (pFirstLeaf)
	{
		m_pGraphTree->setCurrentItem(pFirstLeaf);
		OpenGraphByIndex(static_cast<size_t>(pFirstLeaf->data(0, Qt::UserRole).toULongLong()));
	}
}

void CFlowGraphQtDockable::OpenGraphByIndex(size_t graphIndex)
{
	CFlowGraphManager* pManager = GetIEditorImpl()->GetFlowGraphManager();
	if (!pManager || graphIndex >= pManager->GetFlowGraphCount())
		return;

	if (CHyperFlowGraph* pGraph = pManager->GetFlowGraph(graphIndex))
		SetCurrentGraph(pGraph, /*fitInView*/ true);
}

QWidget* CFlowGraphQtDockable::CreateGraphList()
{
	m_pGraphTree = new QTreeWidget();
	m_pGraphTree->setHeaderHidden(true);
	m_pGraphTree->setSortingEnabled(false);

	// Leaf click opens the graph; folder rows carry no data (toggle on click).
	QObject::connect(m_pGraphTree, &QTreeWidget::itemClicked, [this](QTreeWidgetItem* pItem, int)
	{
		const QVariant data = pItem->data(0, Qt::UserRole);
		if (data.isValid())
			OpenGraphByIndex(static_cast<size_t>(data.toULongLong()));
		else
			pItem->setExpanded(!pItem->isExpanded());
	});

	// Collapsible, titled section (the framework's grouping component).
	QCollapsibleFrame* pFrame = new QCollapsibleFrame(tr("Flow Graphs"));
	pFrame->SetClosable(false);
	pFrame->SetWidget(m_pGraphTree);
	return pFrame;
}

void CFlowGraphQtDockable::SetCurrentGraph(CHyperFlowGraph* pGraph, bool fitInView)
{
	if (m_pCurrentGraph != pGraph)
	{
		if (m_pCurrentGraph)
			m_pCurrentGraph->RemoveListener(this);

		m_pCurrentGraph = pGraph;

		if (m_pCurrentGraph)
			m_pCurrentGraph->AddListener(this);
	}

	RebuildModel(fitInView);
}

void CFlowGraphQtDockable::RebuildModel(bool fitInView)
{
	// Detach + delete the old model before building a new one (its items point at
	// CHyperNodes that an undo/redo may have just recreated).
	m_pView->SetModel(nullptr);

	if (m_pCurrentGraph)
	{
		m_pModel.reset(new FlowGraphQt::CFlowGraphViewModel(*m_pCurrentGraph));
		m_pView->SetModel(m_pModel.get());
		if (fitInView)
		{
			// FitSceneInView fits the WHOLE graph (down to 10%), so spread-out
			// graphs open uselessly zoomed out. Use it only to center on the
			// content (it excludes the grid background, which we can't reach from
			// here), then open at 100%.
			m_pView->FitSceneInView();
			const QPoint contentCenter = m_pView->GetPosition();
			m_pView->SetZoom(100);
			m_pView->SetPosition(contentCenter);
		}
	}
	else
	{
		m_pModel.reset();
	}

	UpdateWindowTitle();
}

void CFlowGraphQtDockable::OnHyperGraphEvent(IHyperNode* /*pNode*/, EHyperGraphEvent event)
{
	// Undo/redo restores the graph in place (recreating its nodes), so our item
	// pointers are stale, so rebuild. Only react to UNDO_REDO (our own edits fire
	// NODE_ADD / GRAPH_INVALIDATE and would re-enter) and to NODE_UPDATE_ENTITY,
	// which changes the entity-port label + title (fired by AssignEntities).
	if (event == EHG_GRAPH_UNDO_REDO || event == EHG_NODE_UPDATE_ENTITY)
		RebuildModel(/*fitInView*/ false);

	// Edits / undo / redo may have changed the modified state -> refresh the asterisk.
	UpdateWindowTitle();
}

void CFlowGraphQtDockable::SaveCurrentGraph()
{
	if (!m_pCurrentGraph)
		return;

	const CString filename = m_pCurrentGraph->GetFilename();
	bool saved = false;
	if (!filename.IsEmpty())
		saved = m_pCurrentGraph->Save(filename.GetString()); // file-backed (module / matfx / ...)
	else
		saved = GetIEditorImpl()->SaveDocument();             // entity graph persists with the level

	if (saved)
		m_pCurrentGraph->SetModified(false); // clear the asterisk (Save() leaves the flag as-is)

	UpdateWindowTitle();
}
