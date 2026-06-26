// Copyright 2026 ProdigyTech Inc. All rights reserved.

#include "StdAfx.h"
#include "FlowGraphQtViewModel.h"

#include "IEditorImpl.h"                  // GetIEditorImpl
#include "HyperGraph/HyperGraph.h"
#include "HyperGraph/HyperGraphNode.h"
#include "HyperGraph/FlowGraphNode.h"     // CFlowNode::GetEntityTitle
#include "HyperGraph/FlowGraphManager.h"  // GetPrototypesEx (node dictionary)

#include "FlowGraphQtNodeProperties.h" // CFlowGraphView::SetContextPin

#include <NodeGraph/NodeWidget.h>
#include <NodeGraph/PinWidget.h>
#include <NodeGraph/AbstractNodeContentWidget.h>
#include <NodeGraph/ConnectionWidget.h>
#include <NodeGraph/NodeEditorData.h>

#include <NodeGraph/NodeGraphViewStyle.h>
#include <NodeGraph/NodeWidgetStyle.h>
#include <NodeGraph/TextWidgetStyle.h>
#include <NodeGraph/HeaderWidgetStyle.h>
#include <NodeGraph/ConnectionWidgetStyle.h>
#include <NodeGraph/NodePinWidgetStyle.h>

#include "HyperGraph/FlowGraphPreferences.h" // gFlowGraphColorPreferences (legacy node colors)

#include <CryIcon.h>
#include <QtUtil.h>

#include <QFont>
#include <QFontMetrics>
#include <QGraphicsGridLayout>
#include <QGraphicsLinearLayout>
#include <QIcon>
#include <QPainter>
#include <QSizePolicy>
#include <QStringList>

#include <algorithm>

namespace FlowGraphQt
{

namespace
{

// Classic FlowGraph port colors, indexed by IVariable::EType, mirroring
// PortTypeToColor[] in HyperNodePainter_Default.cpp. Values are 0x00RRGGBB
// (the legacy code ORs alpha into the top byte, so the literals are RGB).
const uint32 kPortColorTable[] =
{
	0x0000FF00, // UNKNOWN -> green
	0x00FF0000, // INT     -> red
	0x000050FF, // BOOL    -> blue
	0x00FFFFFF, // FLOAT   -> white
	0x00FF00FF, // VECTOR2 -> magenta
	0x00FFFF00, // VECTOR  -> yellow
	0x0000FFFF, // VECTOR4 -> cyan
	0x007F00FF, // QUAT    -> purple
	0x0000FFFF, // STRING  -> cyan
	0x00FF7F00, // ARRAY   -> orange
	0x0000FF7F,
	0x007F7F7F,
	0x00000000,
};
const uint32 kPortColorCount = sizeof(kPortColorTable) / sizeof(kPortColorTable[0]);

uint32 ClampPortType(int type)
{
	return (type >= 0 && static_cast<uint32>(type) < kPortColorCount) ? static_cast<uint32>(type) : 0u;
}

QColor PortColor(uint32 index)
{
	const uint32 v = kPortColorTable[ClampPortType(index)];
	return QColor((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

string MakePinStyleId(uint32 typeIndex)
{
	string s;
	s.Format("Pin::FlowGraph::%u", typeIndex);
	return s;
}

const char* PortTypeName(uint32 index)
{
	// Parallel to IVariable::EType.
	static const char* names[] = { "Any", "Int", "Bool", "Float", "Vec2", "Vec3", "Vec4", "Quat", "String", "Array" };
	return (index < (sizeof(names) / sizeof(names[0]))) ? names[index] : "";
}

// COLORREF (packed abgr; R is the low byte) -> QColor, matching the legacy
// painter's GET_GDI_COLOR (GetRValue/GetGValue/GetBValue).
QColor LegacyColor(COLORREF c)
{
	return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF));
}

// The teal title-bar color the legacy painter fills the entity port with
// (brushBackgroundSelected == colorNodeSelected).
QColor EntityPortColor()
{
	return LegacyColor(gFlowGraphColorPreferences.colorNodeSelected);
}

//////////////////////////////////////////////////////////////////////////
// Entity-target pin: drawn with the title-bar background (like the legacy
// full-width entity bar) so it reads as a special port, not a normal input.
class CFlowGraphEntityPinWidget : public CryGraphEditor::CPinWidget
{
public:
	CFlowGraphEntityPinWidget(CryGraphEditor::CAbstractPinItem& item, CryGraphEditor::CNodeWidget& nodeWidget, CryGraphEditor::CNodeGraphView& view)
		: CryGraphEditor::CPinWidget(item, nodeWidget, view, true)
		, m_fillColor(EntityPortColor())
	{
		// These don't change for the pin's lifetime, so resolve them once rather
		// than per paint.
		const CryGraphEditor::CNodePinWidgetStyle& style =
		  static_cast<const CryGraphEditor::CNodePinWidgetStyle&>(GetStyle());
		m_textColor = style.GetHeaderTextStyle().GetTextColor();
		m_textFont = style.GetTextFont();
		m_textFont.setBold(true);
	}

	virtual void paint(QPainter* pPainter, const QStyleOptionGraphicsItem* pOption, QWidget* pWidget) override
	{
		// Full-width teal fill. The +1 (like CHeaderWidget::paint) covers the 1px
		// body-colored seam on the right/bottom that otherwise reads as a border.
		pPainter->save();
		pPainter->setPen(Qt::NoPen);
		pPainter->setBrush(m_fillColor);
		pPainter->drawRect(boundingRect().adjusted(0, 0, 1, 1));
		pPainter->restore();

		// Draw the label ourselves, inset 6px on each side. We can't defer to the
		// base paint: it draws at m_pName->rect() (pin-local 0,0), so layout
		// margins don't move the text. The arrow icon is a separate child item
		// and still renders. Skip the label at low zoom, mirroring the base.
		if (GetView().GetZoom() >= 40)
		{
			pPainter->save();
			pPainter->setFont(m_textFont);
			pPainter->setPen(m_textColor);
			pPainter->drawText(boundingRect().adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, GetItem().GetName());
			pPainter->restore();
		}
	}

private:
	QColor m_fillColor;
	QColor m_textColor;
	QFont  m_textFont;
};

//////////////////////////////////////////////////////////////////////////
// Node body. Reimplements the pin grid rather than subclassing
// CPinGridNodeContentWidget, whose layout counters are private and which can't
// give the entity port its own row. Differences from the stock grid:
//   - the entity port (first input of an EHYPER_NODE_ENTITY node) gets its own
//     full-width row flush to the title, so the body is two layers: a margin-0
//     outer column (entity bar + grid) wrapping the padded grid of normal ports.
//   - a right-button release isn't consumed, so it reaches the node and the
//     context menu works over ports; the hit port is recorded on the view so
//     the menu can be entity- vs plain-port specific.
// Hover/connection routing is otherwise copied from the stock grid.
class CFlowGraphContentWidget : public CryGraphEditor::CAbstractNodeContentWidget
{
public:
	CFlowGraphContentWidget(CryGraphEditor::CNodeWidget& node, CryGraphEditor::CNodeGraphView& view)
		: CryGraphEditor::CAbstractNodeContentWidget(node, view)
		, m_pOuterLayout(nullptr)
		, m_pGridLayout(nullptr)
		, m_pEntityWidget(nullptr)
		, m_numInputRows(0)
		, m_numOutputRows(0)
		, m_pLastEnteredPin(nullptr)
	{
		node.SetContentWidget(this);

		// Outer layer: no margins, so the entity bar can sit flush to the title
		// and span the full node width.
		m_pOuterLayout = new QGraphicsLinearLayout(Qt::Vertical);
		m_pOuterLayout->setContentsMargins(0.f, 0.f, 0.f, 0.f);
		m_pOuterLayout->setSpacing(0.f);

		// Inner layer: the regular ports keep the usual padding.
		m_pGridLayout = new QGraphicsGridLayout();
		m_pGridLayout->setColumnAlignment(0, Qt::AlignLeft);
		m_pGridLayout->setColumnAlignment(1, Qt::AlignRight);
		m_pGridLayout->setVerticalSpacing(2.f);
		m_pGridLayout->setHorizontalSpacing(35.f);
		m_pGridLayout->setContentsMargins(5.0f, 5.0f, 5.0f, 5.0f);
		m_pOuterLayout->addItem(m_pGridLayout); // entity bar (if any) is inserted before this

		CryGraphEditor::CAbstractNodeItem& nodeItem = node.GetItem();
		nodeItem.SignalPinAdded.Connect(this, &CFlowGraphContentWidget::OnPinAdded);
		nodeItem.SignalPinRemoved.Connect(this, &CFlowGraphContentWidget::OnPinRemoved);
		nodeItem.SignalInvalidated.Connect(this, &CFlowGraphContentWidget::OnItemInvalidated);

		for (CryGraphEditor::CAbstractPinItem* pPinItem : nodeItem.GetPinItems())
		{
			CryGraphEditor::CPinWidget* pPinWidget = pPinItem->CreateWidget(node, GetView());
			AddPin(*pPinWidget);
		}

		setLayout(m_pOuterLayout);
	}

	virtual void DeleteLater() override
	{
		CryGraphEditor::CAbstractNodeItem& nodeItem = GetNode().GetItem();
		nodeItem.SignalPinAdded.DisconnectObject(this);
		nodeItem.SignalPinRemoved.DisconnectObject(this);
		nodeItem.SignalInvalidated.DisconnectObject(this);

		for (CryGraphEditor::CPinWidget* pPinWidget : m_pins)
			pPinWidget->DeleteLater();

		CryGraphEditor::CAbstractNodeContentWidget::DeleteLater();
	}

	virtual void OnLayoutChanged() override
	{
		UpdateLayout(m_pOuterLayout);
	}

	virtual void OnItemInvalidated() override
	{
		CryGraphEditor::PinWidgetArray pinWidgets;
		pinWidgets.swap(m_pins);
		for (CryGraphEditor::CPinWidget* pPinWidget : pinWidgets)
			DetachPin(*pPinWidget);

		m_pEntityWidget = nullptr;
		m_numInputRows = m_numOutputRows = 0;
		for (CryGraphEditor::CAbstractPinItem* pPinItem : GetNode().GetItem().GetPinItems())
		{
			const QVariant pinId = pPinItem->GetId();
			auto predicate = [pinId](CryGraphEditor::CPinWidget* pPinWidget) -> bool
			{
				return (pPinWidget && pPinWidget->GetItem().HasId(pinId));
			};

			auto result = std::find_if(pinWidgets.begin(), pinWidgets.end(), predicate);
			if (result == pinWidgets.end())
			{
				AddPin(*pPinItem->CreateWidget(GetNode(), GetView()));
			}
			else
			{
				AddPin(**result);
				*result = nullptr;
			}
		}

		for (CryGraphEditor::CPinWidget* pPinWidget : pinWidgets)
		{
			if (pPinWidget != nullptr)
			{
				RemovePin(*pPinWidget);
				pPinWidget->GetItem().SignalInvalidated.DisconnectObject(this);
				pPinWidget->DeleteLater();
			}
		}

		UpdateLayout(m_pOuterLayout);
	}

	virtual void OnInputEvent(CryGraphEditor::CNodeWidget* pSender, CryGraphEditor::SMouseInputEventArgs& args) override
	{
		using CryGraphEditor::EMouseEventReason;
		using CryGraphEditor::SMouseInputEventArgs;
		using CryGraphEditor::SPinMouseEventArgs;

		const EMouseEventReason reason = args.GetReason();

		// Which pin (if any) is under the cursor?
		CryGraphEditor::CPinWidget* pHitPinWidget = nullptr;
		if (reason != EMouseEventReason::HoverLeave)
		{
			for (CryGraphEditor::CPinWidget* pPinWidget : m_pins)
			{
				const QPointF localPoint = pPinWidget->mapFromScene(args.GetScenePos());
				if (pPinWidget->GetRect().contains(localPoint))
				{
					pHitPinWidget = pPinWidget;
					break;
				}
			}
		}

		// Right-click: record the port for the context menu and let the event
		// fall through to the node (so the node menu shows over ports too).
		if (reason == EMouseEventReason::ButtonRelease && args.GetButton() == Qt::MouseButton::RightButton)
		{
			CFlowGraphPinItem* pPin = pHitPinWidget ? static_cast<CFlowGraphPinItem*>(&pHitPinWidget->GetItem()) : nullptr;
			static_cast<CFlowGraphView&>(GetView()).SetContextPin(pPin);
			args.SetAccepted(false);
			return;
		}

		// --- the remainder mirrors CPinGridNodeContentWidget::OnInputEvent ---
		if (m_pLastEnteredPin != nullptr && (m_pLastEnteredPin != pHitPinWidget || pHitPinWidget == nullptr))
		{
			if (reason == EMouseEventReason::HoverLeave || reason == EMouseEventReason::HoverMove)
			{
				SMouseInputEventArgs mouseArgs(
				  EMouseEventReason::HoverLeave,
				  Qt::MouseButton::NoButton, Qt::MouseButton::NoButton, args.GetModifiers(),
				  args.GetLocalPos(), args.GetScenePos(), args.GetScreenPos(),
				  args.GetLastLocalPos(), args.GetLastScenePos(), args.GetLastScreenPos());

				GetView().OnPinMouseEvent(m_pLastEnteredPin, SPinMouseEventArgs(mouseArgs));
				m_pLastEnteredPin = nullptr;

				args.SetAccepted(true);
				return;
			}
		}

		if (pHitPinWidget)
		{
			EMouseEventReason effectiveReason = reason;
			if (m_pLastEnteredPin == nullptr)
			{
				m_pLastEnteredPin = pHitPinWidget;
				if (effectiveReason == EMouseEventReason::HoverMove)
					effectiveReason = EMouseEventReason::HoverEnter;
			}

			SMouseInputEventArgs mouseArgs(
			  effectiveReason,
			  args.GetButton(), args.GetButtons(), args.GetModifiers(),
			  args.GetLocalPos(), args.GetScenePos(), args.GetScreenPos(),
			  args.GetLastLocalPos(), args.GetLastScenePos(), args.GetLastScreenPos());

			GetView().OnPinMouseEvent(pHitPinWidget, SPinMouseEventArgs(mouseArgs));

			args.SetAccepted(true);
			return;
		}

		args.SetAccepted(false);
	}

private:
	void AddPin(CryGraphEditor::CPinWidget& pinWidget)
	{
		CFlowGraphPinItem& item = static_cast<CFlowGraphPinItem&>(pinWidget.GetItem());
		if (item.IsEntityPort())
		{
			// Flush, full-width bar above the padded grid.
			pinWidget.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			m_pOuterLayout->insertItem(0, &pinWidget);
			m_pEntityWidget = &pinWidget;
		}
		else if (item.IsInputPin())
			m_pGridLayout->addItem(&pinWidget, m_numInputRows++, 0);
		else
			m_pGridLayout->addItem(&pinWidget, m_numOutputRows++, 1);

		m_pins.push_back(&pinWidget);
		UpdateLayout(m_pOuterLayout);
	}

	// Remove a pin from whichever layer holds it, without touching m_pins.
	void DetachPin(CryGraphEditor::CPinWidget& pinWidget)
	{
		if (&pinWidget == m_pEntityWidget)
			m_pOuterLayout->removeItem(&pinWidget);
		else
			m_pGridLayout->removeItem(&pinWidget);
	}

	void RemovePin(CryGraphEditor::CPinWidget& pinWidget)
	{
		CFlowGraphPinItem& item = static_cast<CFlowGraphPinItem&>(pinWidget.GetItem());
		if (item.IsEntityPort())
			m_pEntityWidget = nullptr;
		else if (item.IsInputPin())
			--m_numInputRows;
		else
			--m_numOutputRows;

		DetachPin(pinWidget);

		auto result = std::find(m_pins.begin(), m_pins.end(), &pinWidget);
		if (result != m_pins.end())
			m_pins.erase(result);
	}

	void OnPinAdded(CryGraphEditor::CAbstractPinItem& item)
	{
		AddPin(*item.CreateWidget(GetNode(), GetView())); // AddPin already calls UpdateLayout
	}

	void OnPinRemoved(CryGraphEditor::CAbstractPinItem& item)
	{
		const QVariant pinId = item.GetId();
		auto condition = [pinId](CryGraphEditor::CPinWidget* pPinWidget) -> bool
		{
			return (pPinWidget && pPinWidget->GetItem().HasId(pinId));
		};

		const auto result = std::find_if(m_pins.begin(), m_pins.end(), condition);
		if (result != m_pins.end())
		{
			CryGraphEditor::CPinWidget* pPinWidget = *result;
			RemovePin(*pPinWidget);
			pPinWidget->DeleteLater();
		}

		UpdateLayout(m_pOuterLayout);
	}

	QGraphicsLinearLayout*      m_pOuterLayout; // entity bar (flush) + grid
	QGraphicsGridLayout*        m_pGridLayout;  // padded regular ports
	CryGraphEditor::CPinWidget* m_pEntityWidget;
	uint8                       m_numInputRows;
	uint8                       m_numOutputRows;
	CryGraphEditor::CPinWidget* m_pLastEnteredPin;
};

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// CFlowGraphPinItem
//////////////////////////////////////////////////////////////////////////
CFlowGraphPinItem::CFlowGraphPinItem(CFlowGraphNodeItem& nodeItem, CHyperNodePort& port, bool bInput, uint32 index, CryGraphEditor::CNodeGraphViewModel& model)
	: CryGraphEditor::CAbstractPinItem(model)
	, m_nodeItem(nodeItem)
	, m_port(port)
	, m_index(index)
	, m_bInput(bInput)
{
	m_internalName = QtUtil::ToQString(port.GetName());
	const char* szHuman = port.GetHumanName();
	m_displayName = (szHuman && szHuman[0]) ? QtUtil::ToQString(szHuman) : m_internalName;

	m_typeIndex = ClampPortType(port.pVar ? static_cast<int>(port.pVar->GetType()) : IVariable::UNKNOWN);
	m_typeName = QtUtil::ToQString(PortTypeName(m_typeIndex));
	m_styleId = MakePinStyleId(m_typeIndex);

	if (port.pVar)
		m_description = QtUtil::ToQString(port.pVar->GetDescription());
}

QString CFlowGraphPinItem::GetName() const
{
	// The target-entity port shows the assigned entity exactly like the legacy
	// painter ("Choose Entity", the entity name, "<Graph Entity>", ...).
	if (IsEntityPort())
		return QtUtil::ToQString(static_cast<CFlowNode&>(m_nodeItem.GetHyperNode()).GetEntityTitle());

	return m_displayName;
}

QString CFlowGraphPinItem::GetToolTipText() const
{
	// Mirror the legacy hover tooltip: the port's description (with name/type for context).
	QString tip = m_displayName;
	if (!m_typeName.isEmpty())
		tip += QStringLiteral(" (") + m_typeName + QStringLiteral(")");
	if (!m_description.isEmpty())
		tip += QStringLiteral("\n") + m_description;
	return tip;
}

CFlowGraphPinItem::~CFlowGraphPinItem()
{
}

CryGraphEditor::CPinWidget* CFlowGraphPinItem::CreateWidget(CryGraphEditor::CNodeWidget& nodeWidget, CryGraphEditor::CNodeGraphView& view)
{
	if (IsEntityPort())
		return new CFlowGraphEntityPinWidget(*this, nodeWidget, view);

	return new CryGraphEditor::CPinWidget(*this, nodeWidget, view, true);
}

bool CFlowGraphPinItem::IsEntityPort() const
{
	return m_bInput && m_index == 0 && m_nodeItem.GetHyperNode().CheckFlag(EHYPER_NODE_ENTITY);
}

CryGraphEditor::CAbstractNodeItem& CFlowGraphPinItem::GetNodeItem() const
{
	return static_cast<CryGraphEditor::CAbstractNodeItem&>(m_nodeItem);
}

QVariant CFlowGraphPinItem::GetId() const
{
	// (index, direction) is unique within the owning node.
	return QVariant(static_cast<uint>((m_index << 1) | (m_bInput ? 1u : 0u)));
}

bool CFlowGraphPinItem::HasId(QVariant id) const
{
	return id.toUInt() == ((m_index << 1) | (m_bInput ? 1u : 0u));
}

bool CFlowGraphPinItem::CanConnect(const CryGraphEditor::CAbstractPinItem* pOtherPin) const
{
	if (!pOtherPin)
		return true; // can a drag start from this pin at all

	if (&pOtherPin->GetNodeItem() == &GetNodeItem())
		return false;
	if (IsInputPin() == pOtherPin->IsInputPin())
		return false;

	// Defer to the engine's real connection rules (type compatibility,
	// duplicate-edge, etc.). CanConnectPorts is direction-agnostic.
	const CFlowGraphPinItem& other = static_cast<const CFlowGraphPinItem&>(*pOtherPin);
	CHyperGraph& graph = static_cast<CFlowGraphViewModel&>(GetViewModel()).GetHyperGraph();
	CHyperNode& myNode = static_cast<CFlowGraphNodeItem&>(GetNodeItem()).GetHyperNode();
	CHyperNode& otherNode = static_cast<CFlowGraphNodeItem&>(other.GetNodeItem()).GetHyperNode();

	return graph.CanConnectPorts(&myNode, &m_port, &otherNode, &other.GetPort());
}

//////////////////////////////////////////////////////////////////////////
// CFlowGraphNodeItem
//////////////////////////////////////////////////////////////////////////
CFlowGraphNodeItem::CFlowGraphNodeItem(CHyperNode& node, CryGraphEditor::CNodeGraphViewModel& model)
	: CryGraphEditor::CAbstractNodeItem(*(m_pData = new CryGraphEditor::CNodeEditorData()), model)
	, m_node(node)
{
	m_name = QtUtil::ToQString(node.GetTitle());

	SetAcceptsDeletion(!node.CheckFlag(EHYPER_NODE_UNREMOVEABLE));
	SetAcceptsRenaming(false); // inline rename is a later milestone

	const Gdiplus::PointF pos = node.GetPos();
	CryGraphEditor::CAbstractNodeItem::SetPosition(QPointF(pos.X, pos.Y));

	LoadPins();
}

CFlowGraphNodeItem::~CFlowGraphNodeItem()
{
	for (CryGraphEditor::CAbstractPinItem* pPin : m_pins)
		delete pPin;

	delete m_pData;
}

void CFlowGraphNodeItem::LoadPins()
{
	CHyperNode::Ports* pInputs = m_node.GetInputs();
	if (pInputs)
	{
		for (uint32 i = 0; i < pInputs->size(); ++i)
		{
			m_pins.push_back(new CFlowGraphPinItem(*this, (*pInputs)[i], /*bInput*/ true, i, GetViewModel()));
		}
	}

	CHyperNode::Ports* pOutputs = m_node.GetOutputs();
	if (pOutputs)
	{
		for (uint32 i = 0; i < pOutputs->size(); ++i)
		{
			m_pins.push_back(new CFlowGraphPinItem(*this, (*pOutputs)[i], /*bInput*/ false, i, GetViewModel()));
		}
	}
}

CryGraphEditor::CNodeWidget* CFlowGraphNodeItem::CreateWidget(CryGraphEditor::CNodeGraphView& view)
{
	CryGraphEditor::CNodeWidget* pNodeWidget = new CryGraphEditor::CNodeWidget(*this, view);
	new CFlowGraphContentWidget(*pNodeWidget, view);
	// cppcheck-suppress memleak
	return pNodeWidget;
}

QVariant CFlowGraphNodeItem::GetId() const
{
	return QVariant(static_cast<uint>(m_node.GetId()));
}

bool CFlowGraphNodeItem::HasId(QVariant id) const
{
	return id.toUInt() == static_cast<uint>(m_node.GetId());
}

QVariant CFlowGraphNodeItem::GetTypeId() const
{
	return QVariant(QtUtil::ToQString(m_node.GetClassName()));
}

QPointF CFlowGraphNodeItem::GetPosition() const
{
	const Gdiplus::PointF pos = m_node.GetPos();
	return QPointF(pos.X, pos.Y);
}

void CFlowGraphNodeItem::SetPosition(QPointF position)
{
	m_node.SetPos(Gdiplus::PointF(static_cast<float>(position.x()), static_cast<float>(position.y())));
	CryGraphEditor::CAbstractNodeItem::SetPosition(position);
}

HyperNodeID CFlowGraphNodeItem::GetHyperNodeId() const
{
	return m_node.GetId();
}

CFlowGraphPinItem* CFlowGraphNodeItem::FindPinByInternalName(const QString& internalName, bool bInput) const
{
	for (CryGraphEditor::CAbstractPinItem* pAbstract : m_pins)
	{
		CFlowGraphPinItem* pPin = static_cast<CFlowGraphPinItem*>(pAbstract);
		if (pPin->IsInputPin() == bInput && pPin->GetInternalName() == internalName)
			return pPin;
	}
	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// CFlowGraphConnectionItem
//////////////////////////////////////////////////////////////////////////
CFlowGraphConnectionItem::CFlowGraphConnectionItem(CHyperEdge& edge, CFlowGraphPinItem& sourcePin, CFlowGraphPinItem& targetPin, CryGraphEditor::CNodeGraphViewModel& model)
	: CryGraphEditor::CAbstractConnectionItem(model)
	, m_edge(edge)
	, m_sourcePin(sourcePin)
	, m_targetPin(targetPin)
{
	m_sourcePin.AddConnection(*this);
	m_targetPin.AddConnection(*this);
}

CFlowGraphConnectionItem::~CFlowGraphConnectionItem()
{
	m_sourcePin.RemoveConnection(*this);
	m_targetPin.RemoveConnection(*this);
}

CryGraphEditor::CConnectionWidget* CFlowGraphConnectionItem::CreateWidget(CryGraphEditor::CNodeGraphView& view)
{
	return new CryGraphEditor::CConnectionWidget(this, view);
}

QVariant CFlowGraphConnectionItem::GetId() const
{
	return QVariant::fromValue(reinterpret_cast<quintptr>(&m_edge));
}

bool CFlowGraphConnectionItem::HasId(QVariant id) const
{
	return reinterpret_cast<quintptr>(&m_edge) == id.value<quintptr>();
}

//////////////////////////////////////////////////////////////////////////
// Style
//////////////////////////////////////////////////////////////////////////
namespace
{

// The legacy painter uses Tahoma 10.13pt for title + ports (HyperNodePainter_Default).
// As a point size it now scales correctly with DPI (the whole reason for the port).
QFont LegacyNodeFont()
{
	QFont font(QStringLiteral("Tahoma"));
	font.setPointSizeF(10.13);
	return font;
}

// The CNodeGraphViewStyle ctor pre-registers default "Node"/"Connection"/"Pin"
// ids; re-registering them asserts, and the icon-less default "Pin" crashes the
// painter. So we register suffixed ids and return those from GetStyleId().
void AddNodeStyle(CryGraphEditor::CNodeGraphViewStyle& viewStyle, const char* szStyleId)
{
	const SFlowGraphColorPreferences& prefs = gFlowGraphColorPreferences;
	const QColor bodyColor = LegacyColor(prefs.colorNodeBkg);
	const QColor outlineColor = LegacyColor(prefs.colorNodeOutline);
	const QColor titleTextColor = LegacyColor(prefs.colorTitleText);
	// The legacy painter fills the title bar with brushBackgroundSelected
	// (colorNodeSelected) regardless of selection state -> the teal title bar.
	const QColor titleBarColor = LegacyColor(prefs.colorNodeSelected);

	CryGraphEditor::CNodeWidgetStyle* pStyle = new CryGraphEditor::CNodeWidgetStyle(szStyleId, viewStyle);
	pStyle->SetBackgroundColor(bodyColor);
	pStyle->SetBorderColor(outlineColor);
	// No body inset: the node wraps the content in a margin (default 6,3,6,6),
	// which would pad the full-width entity bar away from the title and edges.
	// The regular-port padding is supplied by the content's own inner grid.
	pStyle->SetMargins(QMargins(0, 0, 0, 0));

	const QFont nodeFont = LegacyNodeFont();

	CryGraphEditor::CTextWidgetStyle& textStyle = pStyle->GetHeaderTextStyle();
	textStyle.SetTextColor(titleTextColor);
	textStyle.SetTextFont(nodeFont);

	CryGraphEditor::CHeaderWidgetStyle& headerStyle = pStyle->GetHeaderWidgetStyle();
	headerStyle.SetLeftColor(titleBarColor);
	headerStyle.SetRightColor(titleBarColor);
	// Legacy title bar height == the title text height (no extra padding).
	headerStyle.SetHeight(QFontMetrics(nodeFont).height() + 4);

	// Remove the title image: a null icon makes CHeaderWidget skip the icon slot
	// entirely (so the title sits flush-left).
	headerStyle.SetNodeIcon(QIcon());
	// cppcheck-suppress memleak
}

void AddPinStyle(CryGraphEditor::CNodeGraphViewStyle& viewStyle, const char* szStyleId, const char* szIcon, QColor color)
{
	CryIcon icon(szIcon, { { QIcon::Mode::Normal, color } });

	CryGraphEditor::CNodePinWidgetStyle* pStyle = new CryGraphEditor::CNodePinWidgetStyle(szStyleId, viewStyle);
	pStyle->SetIcon(icon);
	pStyle->SetColor(color);                 // pin connector dot keeps its per-type color
	pStyle->SetTextFont(LegacyNodeFont());   // pin name font (PinWidget reads this directly)
	// Pin name color: PinWidget now reads it from the pin's header text style.
	pStyle->GetHeaderTextStyle().SetTextColor(LegacyColor(gFlowGraphColorPreferences.colorText));
	// cppcheck-suppress memleak
}

void AddConnectionStyle(CryGraphEditor::CNodeGraphViewStyle& viewStyle, const char* szStyleId, float width)
{
	CryGraphEditor::CConnectionWidgetStyle* pStyle = new CryGraphEditor::CConnectionWidgetStyle(szStyleId, viewStyle);
	pStyle->SetWidth(width);
	pStyle->SetUsePinColors(false);    // legacy wires are a single color, not a pin gradient
	pStyle->SetColor(QColor(0, 0, 0)); // black, like the legacy edges
	// cppcheck-suppress memleak
}

CryGraphEditor::CNodeGraphViewStyle* CreateStyle()
{
	CryGraphEditor::CNodeGraphViewStyle* pViewStyle = new CryGraphEditor::CNodeGraphViewStyle("FlowGraphQt");

	// Canvas grid colors copied from the legacy FlowGraph preferences.
	pViewStyle->SetGridBackgroundColor(LegacyColor(gFlowGraphColorPreferences.colorBackground));
	pViewStyle->SetGridSegmentLineColor(LegacyColor(gFlowGraphColorPreferences.colorGrid));
	pViewStyle->SetGridSubSegmentLineColor(LegacyColor(gFlowGraphColorPreferences.colorGrid));

	AddNodeStyle(*pViewStyle, "Node::FlowGraph");

	// One pin style per IVariable::EType, matching the legacy FlowGraph palette.
	// Ports are drawn as colored arrows (like the legacy nodes), not dots.
	for (uint32 i = 0; i < kPortColorCount; ++i)
	{
		AddPinStyle(*pViewStyle, MakePinStyleId(i).c_str(), "icons:Graph/Node_connection_arrow_R.ico", PortColor(i));
	}

	AddConnectionStyle(*pViewStyle, "Connection::FlowGraph", 2.0f);

	return pViewStyle;
}

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// CFlowGraphDictionaryEntry
//////////////////////////////////////////////////////////////////////////
CFlowGraphDictionaryEntry::CFlowGraphDictionaryEntry(const QString& name, uint32 type, CFlowGraphDictionaryEntry* pParent)
	: m_name(name)
	, m_type(type)
	, m_pParent(pParent)
{
}

const CAbstractDictionaryEntry* CFlowGraphDictionaryEntry::GetChildEntry(int32 index) const
{
	return (index >= 0 && index < static_cast<int32>(m_children.size())) ? m_children[index].get() : nullptr;
}

CFlowGraphDictionaryEntry* CFlowGraphDictionaryEntry::AddFolder(const QString& name)
{
	m_children.emplace_back(new CFlowGraphDictionaryEntry(name, Type_Folder, this));
	return m_children.back().get();
}

void CFlowGraphDictionaryEntry::AddNode(const QString& name, const QString& className)
{
	std::unique_ptr<CFlowGraphDictionaryEntry> pEntry(new CFlowGraphDictionaryEntry(name, Type_Entry, this));
	pEntry->m_className = className;
	m_children.push_back(std::move(pEntry));
}

//////////////////////////////////////////////////////////////////////////
// CFlowGraphNodesDictionary
//////////////////////////////////////////////////////////////////////////
CFlowGraphNodesDictionary::CFlowGraphNodesDictionary()
{
	Build();
}

const CAbstractDictionaryEntry* CFlowGraphNodesDictionary::GetEntry(int32 index) const
{
	return (index >= 0 && index < static_cast<int32>(m_roots.size())) ? m_roots[index].get() : nullptr;
}

void CFlowGraphNodesDictionary::ResetEntries()
{
	m_roots.clear();
	Build();
}

std::vector<SFlowNodeClass> EnumerateFlowNodeClasses()
{
	std::vector<SFlowNodeClass> classes;

	CFlowGraphManager* pManager = GetIEditorImpl()->GetFlowGraphManager();
	if (!pManager)
		return classes;

	const uint32 categoryMask = EFLN_APPROVED | EFLN_ADVANCED | EFLN_DEBUG;

	std::vector<THyperNodePtr> prototypes;
	pManager->GetPrototypesEx(prototypes, /*bForUI*/ true);

	for (const THyperNodePtr& pProto : prototypes)
	{
		CHyperNode* pNode = pProto;
		if (!pNode || pNode->IsEditorSpecialNode() || !pNode->IsFlowNode())
			continue;

		CFlowNode* pFlowNode = static_cast<CFlowNode*>(pNode);
		if ((pFlowNode->GetCategory() & categoryMask) == 0)
			continue;

		const QString uiName = QtUtil::ToQString(pFlowNode->GetUIClassName());
		if (uiName.isEmpty())
			continue;

		// Split "Group:Sub:Leaf"; nodes without a group fall under "Misc".
		QStringList parts = uiName.split(QLatin1Char(':'));
		parts.removeAll(QString());
		if (parts.size() < 2)
			parts.prepend(QStringLiteral("Misc"));

		SFlowNodeClass cls;
		cls.leaf = parts.takeLast();
		cls.categoryPath = parts;
		cls.className = QtUtil::ToQString(pFlowNode->GetClassName());
		cls.uiName = uiName;
		classes.push_back(std::move(cls));
	}

	return classes;
}

void CFlowGraphNodesDictionary::Build()
{
	std::map<QString, CFlowGraphDictionaryEntry*> folders; // accumulated "A:B:" path -> folder

	for (const SFlowNodeClass& cls : EnumerateFlowNodeClasses())
	{
		CFlowGraphDictionaryEntry* pParent = nullptr;
		QString path;
		for (const QString& category : cls.categoryPath)
		{
			path += category;
			path += QLatin1Char(':');

			auto it = folders.find(path);
			if (it != folders.end())
			{
				pParent = it->second;
				continue;
			}

			CFlowGraphDictionaryEntry* pFolder;
			if (pParent)
			{
				pFolder = pParent->AddFolder(category);
			}
			else
			{
				m_roots.emplace_back(new CFlowGraphDictionaryEntry(category, CAbstractDictionaryEntry::Type_Folder, nullptr));
				pFolder = m_roots.back().get();
			}
			folders[path] = pFolder;
			pParent = pFolder;
		}

		if (pParent)
			pParent->AddNode(cls.leaf, cls.className);
	}
}

//////////////////////////////////////////////////////////////////////////
// CFlowGraphRuntimeContext
//////////////////////////////////////////////////////////////////////////
CFlowGraphRuntimeContext::CFlowGraphRuntimeContext()
	: m_pStyle(CreateStyle())
{
}

CFlowGraphRuntimeContext::~CFlowGraphRuntimeContext()
{
	if (m_pStyle)
		m_pStyle->deleteLater();
}

//////////////////////////////////////////////////////////////////////////
// CFlowGraphViewModel
//////////////////////////////////////////////////////////////////////////
CFlowGraphViewModel::CFlowGraphViewModel(CHyperGraph& graph)
	: m_graph(graph)
{
	BuildNodes();
	BuildConnections();
}

CFlowGraphViewModel::~CFlowGraphViewModel()
{
	for (CFlowGraphConnectionItem* pConnection : m_connectionsByIndex)
		delete pConnection;

	for (CFlowGraphNodeItem* pNode : m_nodesByIndex)
		delete pNode;
}

void CFlowGraphViewModel::BuildNodes()
{
	IHyperGraphEnumerator* pEnum = m_graph.GetNodesEnumerator();
	if (!pEnum)
		return;

	for (IHyperNode* pINode = pEnum->GetFirst(); pINode; pINode = pEnum->GetNext())
	{
		CHyperNode* pNode = static_cast<CHyperNode*>(pINode);
		CFlowGraphNodeItem* pItem = new CFlowGraphNodeItem(*pNode, *this);
		m_nodesByIndex.push_back(pItem);
		m_nodesById[pNode->GetId()] = pItem;
	}
	pEnum->Release();
}

void CFlowGraphViewModel::BuildConnections()
{
	std::vector<CHyperEdge*> edges;
	if (!m_graph.GetAllEdges(edges))
		return;

	for (CHyperEdge* pEdge : edges)
	{
		if (!pEdge)
			continue;

		auto srcIt = m_nodesById.find(pEdge->nodeOut);
		auto dstIt = m_nodesById.find(pEdge->nodeIn);
		if (srcIt == m_nodesById.end() || dstIt == m_nodesById.end())
			continue;

		// nodeOut/portOut is the OUTPUT side (source); nodeIn/portIn the INPUT side (target).
		CFlowGraphPinItem* pSrcPin = srcIt->second->FindPinByInternalName(QtUtil::ToQString(pEdge->portOut), /*bInput*/ false);
		CFlowGraphPinItem* pDstPin = dstIt->second->FindPinByInternalName(QtUtil::ToQString(pEdge->portIn), /*bInput*/ true);
		if (!pSrcPin || !pDstPin)
			continue;

		CFlowGraphConnectionItem* pConnection = new CFlowGraphConnectionItem(*pEdge, *pSrcPin, *pDstPin, *this);
		m_connectionsByIndex.push_back(pConnection);
	}
}

QString CFlowGraphViewModel::GetGraphName()
{
	return QtUtil::ToQString(m_graph.GetName());
}

CryGraphEditor::CAbstractNodeItem* CFlowGraphViewModel::GetNodeItemByIndex(uint32 index) const
{
	return (index < m_nodesByIndex.size()) ? m_nodesByIndex[index] : nullptr;
}

CryGraphEditor::CAbstractNodeItem* CFlowGraphViewModel::GetNodeItemById(QVariant id) const
{
	auto it = m_nodesById.find(static_cast<HyperNodeID>(id.toUInt()));
	return (it != m_nodesById.end()) ? it->second : nullptr;
}

CryGraphEditor::CAbstractConnectionItem* CFlowGraphViewModel::GetConnectionItemByIndex(uint32 index) const
{
	return (index < m_connectionsByIndex.size()) ? m_connectionsByIndex[index] : nullptr;
}

CryGraphEditor::CAbstractConnectionItem* CFlowGraphViewModel::GetConnectionItemById(QVariant id) const
{
	for (CFlowGraphConnectionItem* pConnection : m_connectionsByIndex)
	{
		if (pConnection->HasId(id))
			return pConnection;
	}
	return nullptr;
}

void CFlowGraphViewModel::MaybeRecordUndo()
{
	if (!m_suppressUndo)
		m_graph.RecordUndo(); // self-guards on CUndo::IsRecording()
}

CryGraphEditor::CAbstractNodeItem* CFlowGraphViewModel::CreateNode(QVariant typeId, const QPointF& position)
{
	const QString className = typeId.toString();
	if (className.isEmpty())
		return nullptr;

	MaybeRecordUndo();

	Gdiplus::PointF pos(static_cast<float>(position.x()), static_cast<float>(position.y()));
	IHyperNode* pINode = m_graph.CreateNode(QtUtil::ToString(className).c_str(), pos);
	if (!pINode)
		return nullptr;

	CHyperNode* pNode = static_cast<CHyperNode*>(pINode);
	CFlowGraphNodeItem* pItem = new CFlowGraphNodeItem(*pNode, *this);
	m_nodesByIndex.push_back(pItem);
	m_nodesById[pNode->GetId()] = pItem;
	m_graph.SetModified();

	SignalCreateNode(*pItem);
	return pItem;
}

bool CFlowGraphViewModel::RemoveNode(CryGraphEditor::CAbstractNodeItem& node)
{
	CFlowGraphNodeItem* pItem = static_cast<CFlowGraphNodeItem*>(&node);

	auto indexIt = std::find(m_nodesByIndex.begin(), m_nodesByIndex.end(), pItem);
	if (indexIt == m_nodesByIndex.end())
		return false;

	// One undo snapshot for the whole deletion; the cascade below must not add more.
	MaybeRecordUndo();
	const bool wasSuppressed = m_suppressUndo;
	m_suppressUndo = true;

	// Tear down our connection items first (removes the edges + widgets, keeps
	// pins valid until the node item itself is destroyed).
	const CryGraphEditor::PinItemArray pins = pItem->GetPinItems();
	for (CryGraphEditor::CAbstractPinItem* pPin : pins)
	{
		const CryGraphEditor::ConnectionItemSet conns = pPin->GetConnectionItems();
		for (CryGraphEditor::CAbstractConnectionItem* pConn : conns)
			RemoveConnection(*pConn);
	}

	m_suppressUndo = wasSuppressed;

	CHyperNode& hyperNode = pItem->GetHyperNode();
	const HyperNodeID id = hyperNode.GetId();

	SignalRemoveNode(node);

	m_nodesByIndex.erase(indexIt);
	m_nodesById.erase(id);

	m_graph.RemoveNode(&hyperNode); // engine-side removal (also clears any stray edges)
	m_graph.SetModified();

	delete pItem;
	return true;
}

CryGraphEditor::CAbstractConnectionItem* CFlowGraphViewModel::CreateConnection(CryGraphEditor::CAbstractPinItem& sourcePin, CryGraphEditor::CAbstractPinItem& targetPin)
{
	CFlowGraphPinItem& src = static_cast<CFlowGraphPinItem&>(sourcePin);
	CFlowGraphPinItem& tgt = static_cast<CFlowGraphPinItem&>(targetPin);

	if (!src.CanConnect(&tgt))
		return nullptr;

	// Normalize to output(source) -> input(target); the framework may hand us
	// the pins in either drag order.
	CFlowGraphPinItem& outPin = src.IsOutputPin() ? src : tgt;
	CFlowGraphPinItem& inPin = src.IsOutputPin() ? tgt : src;

	CHyperNode& outNode = static_cast<CFlowGraphNodeItem&>(outPin.GetNodeItem()).GetHyperNode();
	CHyperNode& inNode = static_cast<CFlowGraphNodeItem&>(inPin.GetNodeItem()).GetHyperNode();

	// One undo snapshot for the whole connect (including any conflict removal below).
	MaybeRecordUndo();

	// ConnectPorts silently drops conflicting edges on non-multi ports. Mirror
	// that here first so we don't leave stale connection items/widgets behind.
	auto removeConflicting = [this](CFlowGraphPinItem& pin)
	{
		if (pin.GetPort().bAllowMulti)
			return;
		const CryGraphEditor::ConnectionItemSet conns = pin.GetConnectionItems();
		for (CryGraphEditor::CAbstractConnectionItem* pConn : conns)
			RemoveConnection(*pConn);
	};
	const bool wasSuppressed = m_suppressUndo;
	m_suppressUndo = true;
	removeConflicting(inPin);
	removeConflicting(outPin);
	m_suppressUndo = wasSuppressed;

	if (!m_graph.ConnectPorts(&outNode, &outPin.GetPort(), &inNode, &inPin.GetPort()))
		return nullptr;

	// Retrieve the edge ConnectPorts just made (keyed by the input port).
	CHyperEdge* pEdge = m_graph.FindEdge(&inNode, &inPin.GetPort());
	if (!pEdge)
		return nullptr;

	CFlowGraphConnectionItem* pConnection = new CFlowGraphConnectionItem(*pEdge, outPin, inPin, *this);
	m_connectionsByIndex.push_back(pConnection);
	m_graph.SetModified();

	SignalCreateConnection(*pConnection);
	return pConnection;
}

bool CFlowGraphViewModel::RemoveConnection(CryGraphEditor::CAbstractConnectionItem& connection)
{
	CFlowGraphConnectionItem* pConnection = static_cast<CFlowGraphConnectionItem*>(&connection);

	auto it = std::find(m_connectionsByIndex.begin(), m_connectionsByIndex.end(), pConnection);
	if (it == m_connectionsByIndex.end())
		return false;

	MaybeRecordUndo();

	m_graph.RemoveEdge(&pConnection->GetEdge());
	m_graph.SetModified();

	SignalRemoveConnection(connection);

	m_connectionsByIndex.erase(it);
	delete pConnection;
	return true;
}

} // namespace FlowGraphQt
