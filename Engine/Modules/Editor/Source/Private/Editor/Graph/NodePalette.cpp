#if WITH_EDITOR

#include "Private/Editor/Graph/NodePalette.h"

#include <algorithm>

#include <QKeyEvent>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphDocument.h"

using namespace NodeGraph;
using EditorGraphs::ToQ;

namespace
{
    constexpr int TypeRole = Qt::UserRole;
    constexpr int IndexRole = Qt::UserRole + 1;

    /* Whether a node with InDef (and, for a variable node, of InVariableType)
     * has a pin that a link from InFrom could join. */
    bool CanJoin(const Schema& InSchema, const NodeDefinition& InDef, const std::string& InVariableType,
                 const PinDefinition& InFrom, const std::string& InFromType)
    {
        for(const PinDefinition& Pin : InDef.Pins)
        {
            if(Pin.Direction == InFrom.Direction || Pin.Kind != InFrom.Kind) { continue; }
            if(Pin.IsExec()) { return true; }

            std::vector<std::string> Types;
            if(Pin.Type == DERIVED_TYPE)              { Types = InDef.WildcardTypes; }   // any it could come out as
            else if(Pin.Type != WILDCARD_TYPE)        { Types = { Pin.Type }; }
            else if(!InVariableType.empty())          { Types = { InVariableType }; }
            else                                      { Types = InDef.WildcardTypes; }

            for(const std::string& Type : Types)
            {
                const bool bOk = InFrom.IsOutput() ? InSchema.CanConvert(InFromType, Type) : InSchema.CanConvert(Type, InFromType);
                if(bOk) { return true; }
            }
        }
        return false;
    }
}

NodePalette::NodePalette(const GraphDocument& InDocument, const bool bInFromPin, const PinRef& InFromPin, QWidget* InParent)
    : QFrame(InParent, Qt::Popup)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    resize(300, 420);

    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);
    Layout->setSpacing(4);

    Search = new QLineEdit(this);
    Search->setPlaceholderText(QStringLiteral("Search nodes..."));
    Search->setClearButtonEnabled(true);
    Search->installEventFilter(this);
    Layout->addWidget(Search);

    Tree = new QTreeWidget(this);
    Tree->setHeaderHidden(true);
    Tree->setRootIsDecorated(true);
    Tree->setUniformRowHeights(true);
    Layout->addWidget(Tree, 1);

    // --- Entries -----------------------------------------------------------------
    const Schema& S = InDocument.GetSchema();
    const Graph&  G = InDocument.GetGraph();
    const PinDefinition* FromPin = bInFromPin ? InDocument.FindPin(InFromPin) : nullptr;
    const std::string FromType = bInFromPin ? InDocument.GetPinType(InFromPin) : std::string();

    std::vector<std::string> Present;
    for(const Node& Each : G.Nodes) { Present.push_back(Each.Type); }

    /* Dragged from an input: the new node feeds it, so it must be computable
     * in every stage that input leads to (a material's vertex stage). */
    const Node* FromNode = bInFromPin ? G.FindNode(InFromPin.Node) : nullptr;
    const uint32_t Required = FromNode && FromPin && FromPin->IsInput() ? S.GetRequiredStages(G, *FromNode, FromPin->Name) : 0u;
    const auto StageProblem = [&](const NodeDefinition& InDef) -> QString
    {
        const uint32_t Missing = Required & ~InDef.StageMask;
        if(Missing == 0) { return QString(); }
        uint32_t Bit = 1;
        while(!(Missing & Bit)) { Bit <<= 1; }
        return QStringLiteral("Not available in the %1 stage, which this input leads to").arg(ToQ(S.GetStageName(Bit)));
    };

    for(const NodeDefinition& Def : S.GetNodes())
    {
        if(Def.bUnique && std::find(Present.begin(), Present.end(), Def.Type) != Present.end()) { continue; }

        /* A variable node is offered once per variable, already pointing at it. */
        const PropertyDefinition* VariableProperty = nullptr;
        for(const PropertyDefinition& Property : Def.Properties)
        {
            if(Property.Kind == EPropertyKind::Variable) { VariableProperty = &Property; }
        }

        if(VariableProperty)
        {
            for(const Variable& Var : G.Variables)
            {
                if(FromPin && !CanJoin(S, Def, Var.Type, *FromPin, FromType)) { continue; }
                const bool bGet = Def.Role == ENodeRole::Pure;
                Entry Added;
                Added.Type       = Def.Type;
                Added.Properties = { { VariableProperty->Name, Var.Name } };
                Added.Label      = (bGet ? QStringLiteral("Get %1") : QStringLiteral("Set %1")).arg(ToQ(Var.Name));
                Added.Category   = ToQ(Def.Category);
                Added.Tooltip    = QStringLiteral("%1 (%2)").arg(ToQ(Var.Name), ToQ(Var.Type));
                Entries.push_back(Added);
            }
            continue;
        }

        /* So is a function node, per graph function: its pins are that function's. */
        const PropertyDefinition* FunctionProperty = nullptr;
        for(const PropertyDefinition& Property : Def.Properties)
        {
            if(Property.Kind == EPropertyKind::Function) { FunctionProperty = &Property; }
        }

        if(FunctionProperty)
        {
            for(const GraphFunction& Function : G.Functions)
            {
                Node Probe;
                Probe.Type = Def.Type;
                Probe.Properties[FunctionProperty->Name] = Function.Name;
                const NodeDefinition* Placed = S.FindDefinition(G, Probe);
                if(!Placed || (FromPin && !CanJoin(S, *Placed, std::string(), *FromPin, FromType))) { continue; }

                /* One Entry per function. */
                const bool bEntry = Placed->Role == ENodeRole::Event;
                const bool bHasEntry = std::any_of(G.Nodes.begin(), G.Nodes.end(), [&](const Node& Each)
                {
                    return Each.Type == Def.Type && Each.GetProperty(FunctionProperty->Name) == Function.Name;
                });
                if(bEntry && bHasEntry) { continue; }

                Entry Added;
                Added.Type       = Def.Type;
                Added.Properties = { { FunctionProperty->Name, Function.Name } };
                Added.Label      = bEntry ? QStringLiteral("%1 (Entry)").arg(ToQ(Function.Name)) : ToQ(Placed->DisplayName);
                Added.Category   = ToQ(Def.Category);
                Added.Tooltip    = ToQ(Def.Tooltip);
                Added.SearchText = (Added.Label + QLatin1Char(' ') + Added.Category + QLatin1Char(' ') + ToQ(Def.Keywords)).toLower();
                Entries.push_back(Added);
            }
            continue;
        }

        if(FromPin && !CanJoin(S, Def, std::string(), *FromPin, FromType)) { continue; }

        Entry Added;
        Added.Type        = Def.Type;
        Added.Label       = ToQ(Def.DisplayName);
        Added.Category    = ToQ(Def.Category);
        Added.Tooltip     = ToQ(Def.Tooltip);
        Added.Unavailable = StageProblem(Def);
        for(const PropertyDefinition& Property : Def.Properties)
        {
            if(!Property.Default.empty()) { Added.Properties[Property.Name] = Property.Default; }
        }
        Added.SearchText = (Added.Label + QLatin1Char(' ') + Added.Category + QLatin1Char(' ') + ToQ(Def.Keywords)).toLower();
        Entries.push_back(Added);
    }
    for(Entry& Each : Entries)
    {
        if(Each.SearchText.isEmpty()) { Each.SearchText = (Each.Label + QLatin1Char(' ') + Each.Category).toLower(); }
    }

    connect(Search, &QLineEdit::textChanged, this, &NodePalette::Refill);
    connect(Tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* InItem, int) { Choose(InItem); });
    connect(Tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* InItem, int)
    {
        if(InItem->data(0, IndexRole).isValid()) { Choose(InItem); }
    });

    Refill(QString());
}

void NodePalette::Popup(const QPoint& InGlobalPos)
{
    move(InGlobalPos);
    show();
    Search->setFocus();
}

void NodePalette::Refill(const QString& InFilter)
{
    Tree->clear();
    const QStringList Words = InFilter.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);

    std::map<QString, QTreeWidgetItem*> Folders;
    const auto FolderFor = [&](const QString& InCategory) -> QTreeWidgetItem*
    {
        QTreeWidgetItem* Parent = nullptr;
        QString Path;
        for(const QString& Part : InCategory.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        {
            Path += QLatin1Char('/') + Part;
            QTreeWidgetItem*& Folder = Folders[Path];
            if(!Folder)
            {
                Folder = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(Tree);
                Folder->setText(0, Part);
                Folder->setFlags(Qt::ItemIsEnabled);
                QFont Bold = Folder->font(0);
                Bold.setBold(true);
                Folder->setFont(0, Bold);
            }
            Parent = Folder;
        }
        return Parent;
    };

    QTreeWidgetItem* First = nullptr;
    for(size_t Index = 0; Index < Entries.size(); ++Index)
    {
        const Entry& Each = Entries[Index];
        const bool bMatch = std::all_of(Words.begin(), Words.end(), [&](const QString& W) { return Each.SearchText.contains(W); });
        if(!bMatch) { continue; }

        QTreeWidgetItem* Folder = FolderFor(Each.Category);
        auto* Item = Folder ? new QTreeWidgetItem(Folder) : new QTreeWidgetItem(Tree);
        Item->setText(0, Each.Label);
        Item->setToolTip(0, Each.Unavailable.isEmpty() ? Each.Tooltip : Each.Unavailable);
        Item->setData(0, IndexRole, int(Index));
        if(!Each.Unavailable.isEmpty())
        {
            /* Shown, so it is clear why it cannot be used here, but not choosable. */
            Item->setFlags(Item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
            continue;
        }
        if(!First) { First = Item; }
    }

    /* Searching, or a short list, shows every match; browsing them all starts folded. */
    if(Words.isEmpty() && Entries.size() > 30) { Tree->collapseAll(); }
    else                                       { Tree->expandAll(); }
    if(First && !Words.isEmpty()) { Tree->setCurrentItem(First); }
}

void NodePalette::ChooseCurrent()
{
    QTreeWidgetItem* Item = Tree->currentItem();
    if(Item && Item->data(0, IndexRole).isValid()) { Choose(Item); }
}

void NodePalette::Choose(QTreeWidgetItem* InItem)
{
    if(!InItem || !InItem->data(0, IndexRole).isValid()) { return; }
    const Entry& Picked = Entries[size_t(InItem->data(0, IndexRole).toInt())];
    if(!Picked.Unavailable.isEmpty()) { return; }
    emit Chosen(Picked.Type, Picked.Properties);
    close();
}

bool NodePalette::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* The search box keeps focus; the arrow keys and Enter drive the list. */
    if(InWatched == Search && InEvent->type() == QEvent::KeyPress)
    {
        auto* Key = static_cast<QKeyEvent*>(InEvent);
        switch(Key->key())
        {
            case Qt::Key_Down:
            case Qt::Key_Up:
            case Qt::Key_PageDown:
            case Qt::Key_PageUp:
                QCoreApplication::sendEvent(Tree, InEvent);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                ChooseCurrent();
                return true;
            case Qt::Key_Escape:
                close();
                return true;
            default:
                break;
        }
    }
    return QFrame::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR
