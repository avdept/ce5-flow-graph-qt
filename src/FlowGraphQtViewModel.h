// Copyright 2026 ProdigyTech Inc. All rights reserved.
//
// Projects a legacy CHyperGraph (FlowGraph) onto the CryGraphEditor NodeGraph
// framework so it renders at the right DPI. Edits - node/connection add+remove,
// port values, undo/redo, save - all go back to the engine graph. Follows the
// Schematyc adapter in EditorSchematyc/GraphViewModel.*.

#pragma once

#include <NodeGraph/AbstractNodeGraphViewModel.h>
#include <NodeGraph/AbstractNodeItem.h>
#include <NodeGraph/AbstractPinItem.h>
#include <NodeGraph/AbstractConnectionItem.h>
#include <NodeGraph/ICryGraphEditor.h>
#include <Controls/DictionaryWidget.h>

#include "HyperGraph/IHyperGraph.h"

#include <CryString/CryString.h>

#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <vector>
#include <map>

class CHyperGraph;
class CHyperNode;
class CHyperEdge;
class CHyperNodePort;

namespace CryGraphEditor
{
class CNodeGraphViewStyle;
class CNodeWidget;
class CPinWidget;
class CConnectionWidget;
class CNodeEditorData;
}

namespace FlowGraphQt
{

class CFlowGraphViewModel;
class CFlowGraphNodeItem;

// A UI-visible flownode class, classified into its category folder path.
// Shared by the canvas add-node dictionary and the sidebar node palette.
struct SFlowNodeClass
{
	QStringList categoryPath;
	QString     leaf;
	QString     className;
	QString     uiName;
};

// Enumerates all UI-visible flownode classes (APPROVED|ADVANCED|DEBUG), each
// split into a category folder path + leaf by GetUIClassName().
std::vector<SFlowNodeClass> EnumerateFlowNodeClasses();

// Pin: wraps a single CHyperNodePort (input or output).
class CFlowGraphPinItem : public CryGraphEditor::CAbstractPinItem
{
public:
	CFlowGraphPinItem(CFlowGraphNodeItem& nodeItem, CHyperNodePort& port, bool bInput, uint32 index, CryGraphEditor::CNodeGraphViewModel& model);
	virtual ~CFlowGraphPinItem();

	virtual CryGraphEditor::CPinWidget*        CreateWidget(CryGraphEditor::CNodeWidget& nodeWidget, CryGraphEditor::CNodeGraphView& view) override;
	virtual const char*                        GetStyleId() const override { return m_styleId.c_str(); }
	virtual CryGraphEditor::CAbstractNodeItem& GetNodeItem() const override;

	virtual QString  GetName() const override;
	virtual QString  GetDescription() const override { return m_description; }
	virtual QString  GetTypeName() const override    { return m_typeName; }
	virtual QString  GetToolTipText() const override;

	virtual QVariant GetId() const override;
	virtual bool     HasId(QVariant id) const override;

	virtual bool     IsInputPin() const override     { return m_bInput; }
	virtual bool     IsOutputPin() const override    { return !m_bInput; }

	virtual bool     CanConnect(const CryGraphEditor::CAbstractPinItem* pOtherPin) const override;

	const QString&   GetInternalName() const    { return m_internalName; }
	CHyperNodePort&  GetPort() const             { return m_port; }

	bool             IsEntityPort() const;

private:
	CFlowGraphNodeItem& m_nodeItem;
	CHyperNodePort&     m_port;
	QString             m_displayName;
	QString             m_internalName;
	QString             m_typeName;
	QString             m_description;
	string              m_styleId;
	uint32              m_index;
	uint32              m_typeIndex;
	bool                m_bInput;
};

// Node: wraps a CHyperNode and exposes its input/output ports as pins.
class CFlowGraphNodeItem : public CryGraphEditor::CAbstractNodeItem
{
public:
	CFlowGraphNodeItem(CHyperNode& node, CryGraphEditor::CNodeGraphViewModel& model);
	virtual ~CFlowGraphNodeItem();

	virtual CryGraphEditor::CNodeWidget*        CreateWidget(CryGraphEditor::CNodeGraphView& view) override;
	virtual const char*                         GetStyleId() const override { return "Node::FlowGraph"; }

	virtual QVariant                            GetId() const override;
	virtual bool                                HasId(QVariant id) const override;
	virtual QVariant                            GetTypeId() const override;

	virtual QPointF                             GetPosition() const override;
	virtual void                                SetPosition(QPointF position) override;

	virtual const CryGraphEditor::PinItemArray& GetPinItems() const override { return m_pins; }
	virtual QString                             GetToolTipText() const override { return GetName(); }

	CHyperNode&        GetHyperNode() const { return m_node; }
	HyperNodeID        GetHyperNodeId() const;

	CFlowGraphPinItem* FindPinByInternalName(const QString& internalName, bool bInput) const;

private:
	void LoadPins();

	CHyperNode&                      m_node;
	CryGraphEditor::PinItemArray     m_pins;
	CryGraphEditor::CNodeEditorData* m_pData;
};

// Connection: wraps a CHyperEdge (output pin -> input pin).
class CFlowGraphConnectionItem : public CryGraphEditor::CAbstractConnectionItem
{
public:
	CFlowGraphConnectionItem(CHyperEdge& edge, CFlowGraphPinItem& sourcePin, CFlowGraphPinItem& targetPin, CryGraphEditor::CNodeGraphViewModel& model);
	virtual ~CFlowGraphConnectionItem();

	virtual CryGraphEditor::CConnectionWidget* CreateWidget(CryGraphEditor::CNodeGraphView& view) override;
	virtual const char*                        GetStyleId() const override { return "Connection::FlowGraph"; }

	virtual CryGraphEditor::CAbstractPinItem&  GetSourcePinItem() const override { return m_sourcePin; }
	virtual CryGraphEditor::CAbstractPinItem&  GetTargetPinItem() const override { return m_targetPin; }

	virtual QVariant                           GetId() const override;
	virtual bool                               HasId(QVariant id) const override;

	CHyperEdge& GetEdge() const { return m_edge; }

private:
	CHyperEdge&        m_edge;
	CFlowGraphPinItem& m_sourcePin;
	CFlowGraphPinItem& m_targetPin;
};

// One node-dictionary entry: either a category folder (with children) or a
// leaf that creates a flownode. Leaves carry the engine class name as the
// identifier the view model's CreateNode() expects.
class CFlowGraphDictionaryEntry : public CAbstractDictionaryEntry
{
public:
	CFlowGraphDictionaryEntry(const QString& name, uint32 type, CFlowGraphDictionaryEntry* pParent);

	virtual uint32                          GetType() const override                        { return m_type; }
	virtual QVariant                        GetColumnValue(int32 columnIndex) const override { return m_name; }
	virtual int32                           GetNumChildEntries() const override             { return static_cast<int32>(m_children.size()); }
	virtual const CAbstractDictionaryEntry* GetChildEntry(int32 index) const override;
	virtual const CAbstractDictionaryEntry* GetParentEntry() const override                 { return m_pParent; }
	virtual QVariant                        GetIdentifier() const override                  { return m_className; }

	CFlowGraphDictionaryEntry* AddFolder(const QString& name);
	void                       AddNode(const QString& name, const QString& className);

private:
	QString                                                 m_name;
	QString                                                 m_className;
	uint32                                                  m_type;
	CFlowGraphDictionaryEntry*                              m_pParent;
	std::vector<std::unique_ptr<CFlowGraphDictionaryEntry>> m_children;
};

// Node-creation dictionary: the flownode classes (grouped by GetUIClassName,
// masked to APPROVED|ADVANCED|DEBUG) shown in the canvas right-click search.
class CFlowGraphNodesDictionary : public CAbstractDictionary
{
public:
	CFlowGraphNodesDictionary();

	virtual int32                           GetNumEntries() const override        { return static_cast<int32>(m_roots.size()); }
	virtual const CAbstractDictionaryEntry* GetEntry(int32 index) const override;

	virtual int32                           GetNumColumns() const override        { return 1; }
	virtual QString                         GetColumnName(int32 index) const override { return QStringLiteral("Name"); }
	virtual int32                           GetDefaultFilterColumn() const override { return 0; }
	virtual int32                           GetDefaultSortColumn() const override   { return 0; }

	virtual void                            ClearEntries() override               { m_roots.clear(); }
	virtual void                            ResetEntries() override;

private:
	void Build();

	std::vector<std::unique_ptr<CFlowGraphDictionaryEntry>> m_roots;
};

// Runtime context: owns the view style and the node dictionary.
class CFlowGraphRuntimeContext : public CryGraphEditor::INodeGraphRuntimeContext
{
public:
	CFlowGraphRuntimeContext();
	virtual ~CFlowGraphRuntimeContext();

	virtual const char*                                GetTypeName() const override { return "FlowGraphQt"; }
	virtual CAbstractDictionary*                       GetAvailableNodesDictionary() override { return &m_dictionary; }
	virtual const CryGraphEditor::CNodeGraphViewStyle* GetStyle() const override { return m_pStyle; }

private:
	CFlowGraphNodesDictionary            m_dictionary;
	CryGraphEditor::CNodeGraphViewStyle* m_pStyle;
};

// View model: projects a CHyperGraph, eagerly building all node/connection items.
class CFlowGraphViewModel : public CryGraphEditor::CNodeGraphViewModel
{
public:
	explicit CFlowGraphViewModel(CHyperGraph& graph);
	virtual ~CFlowGraphViewModel();

	virtual CryGraphEditor::INodeGraphRuntimeContext& GetRuntimeContext() override { return m_runtimeContext; }
	virtual QString                                   GetGraphName() override;

	virtual uint32                                    GetNodeItemCount() const override { return m_nodesByIndex.size(); }
	virtual CryGraphEditor::CAbstractNodeItem*        GetNodeItemByIndex(uint32 index) const override;
	virtual CryGraphEditor::CAbstractNodeItem*        GetNodeItemById(QVariant id) const override;

	virtual CryGraphEditor::CAbstractNodeItem*        CreateNode(QVariant typeId, const QPointF& position = QPointF()) override;
	virtual bool                                      RemoveNode(CryGraphEditor::CAbstractNodeItem& node) override;

	virtual uint32                                    GetConnectionItemCount() const override { return m_connectionsByIndex.size(); }
	virtual CryGraphEditor::CAbstractConnectionItem*  GetConnectionItemByIndex(uint32 index) const override;
	virtual CryGraphEditor::CAbstractConnectionItem*  GetConnectionItemById(QVariant id) const override;

	virtual CryGraphEditor::CAbstractConnectionItem*  CreateConnection(CryGraphEditor::CAbstractPinItem& sourcePin, CryGraphEditor::CAbstractPinItem& targetPin) override;
	virtual bool                                      RemoveConnection(CryGraphEditor::CAbstractConnectionItem& connection) override;

	CHyperGraph& GetHyperGraph() const { return m_graph; }

private:
	void BuildNodes();
	void BuildConnections();

	void MaybeRecordUndo();

	CHyperGraph&             m_graph;
	CFlowGraphRuntimeContext m_runtimeContext;

	std::vector<CFlowGraphNodeItem*>       m_nodesByIndex;
	std::map<HyperNodeID, CFlowGraphNodeItem*> m_nodesById;
	std::vector<CFlowGraphConnectionItem*> m_connectionsByIndex;

	bool m_suppressUndo = false;
};

}
