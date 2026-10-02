#if WITH_EDITOR

#include "Private/Editor/Graph/GraphItems.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QPalette>
#include <QStyleOptionGraphicsItem>

#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphScene.h"

using namespace NodeGraph;
using EditorGraphs::ToQ;

namespace
{
    constexpr qreal HeaderHeight = 24.0;
    constexpr qreal RowHeight    = 20.0;
    constexpr qreal PinRadius    = 5.0;
    constexpr qreal PinHitRadius = 10.0;
    constexpr qreal Corner       = 6.0;
    constexpr int   LiteralChars = 14;

    bool IsDarkTheme()
    {
        return QApplication::palette().color(QPalette::Window).lightness() < 128;
    }

    /* The header colour for a node's palette category. */
    QColor CategoryColor(const std::string& InCategory, const ENodeRole InRole)
    {
        if(InRole == ENodeRole::Event)  { return QColor(0xA8, 0x32, 0x2A); }
        if(InRole == ENodeRole::Output) { return QColor(0x9C, 0x55, 0x1F); }

        const std::string Top = InCategory.substr(0, InCategory.find('/'));
        if(Top == "Flow")       { return QColor(0x5E, 0x5E, 0x66); }
        if(Top == "Variables")  { return QColor(0x2E, 0x7D, 0x32); }
        if(Top == "Math" || Top == "Logic") { return QColor(0x3A, 0x6E, 0xA5); }
        if(Top == "Vector" || Top == "Color") { return QColor(0x8A, 0x7A, 0x22); }
        if(Top == "Components") { return QColor(0x6F, 0x4A, 0x9E); }
        if(Top == "Inputs")     { return QColor(0x2A, 0x86, 0x86); }
        if(Top == "Values")     { return QColor(0x4A, 0x7A, 0x3A); }

        /* Anything else: a stable hue from the name. */
        const uint Hash = qHash(QString::fromStdString(Top));
        return QColor::fromHsv(int(Hash % 360), 110, 150);
    }

    QString Elide(const QString& InText, const int InChars)
    {
        return InText.size() <= InChars ? InText : InText.left(InChars - 1) + QChar(0x2026);
    }
}

// === NodeItem ===================================================================

NodeItem::NodeItem(GraphScene& InScene, const Node& InNode, const NodeDefinition* InDef)
    : Scene(InScene)
    , Id(InNode.Id)
{
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setPos(InNode.X, InNode.Y);
    setZValue(0.0);
    setAcceptHoverEvents(true);

    const GraphDocument& Doc = Scene.GetDocument();
    const Graph& G = Doc.GetGraph();

    if(!InDef)
    {
        /* A placeholder: its pins are whatever its links name. */
        bPlaceholder = true;
        Title       = ToQ(InNode.Type);
        Subtitle    = QStringLiteral("unknown node type");
        HeaderColor = QColor(0x80, 0x20, 0x20);
        for(const Link& Each : G.Links)
        {
            const bool bIn = Each.To.Node == Id;
            if(!bIn && Each.From.Node != Id) { continue; }
            const std::string& Name = bIn ? Each.To.Pin : Each.From.Pin;
            const bool bExists = std::any_of(Pins.begin(), Pins.end(), [&](const PinSlot& P) { return P.Name == Name && P.bOutput == !bIn; });
            if(bExists) { continue; }
            PinSlot Slot;
            Slot.Name    = Name;
            Slot.bOutput = !bIn;
            Slot.Label   = ToQ(Name);
            Slot.Color   = QColor(0x9A, 0x9A, 0x9A);
            Slot.bLinked = true;
            Pins.push_back(Slot);
        }
    }
    else
    {
        Title       = ToQ(InDef->DisplayName);
        HeaderColor = CategoryColor(InDef->Category, InDef->Role);

        /* The first non-bool setting names it ("Get Speed", "Parameter: Tint"). */
        if(!InDef->Properties.empty() && InDef->Properties.front().Kind != EPropertyKind::Bool)
        {
            const PropertyDefinition& First = InDef->Properties.front();
            QString Value = ToQ(InNode.GetProperty(First.Name, First.Default));
            if(First.Kind == EPropertyKind::Texture)
            {
                Value = Value.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);   // Content/Textures/Brick.nmtexture: Brick
            }
            /* A function node already says which function, once it has one. */
            const bool bNamed = First.Kind == EPropertyKind::Function && Title.contains(Value);
            if(!Value.isEmpty() && !bNamed)
            {
                Title = First.Kind == EPropertyKind::Variable ? QString(Title).replace(QStringLiteral("Variable"), Value)
                                                              : Title + QStringLiteral(": ") + Value;
            }
        }

        /* The schema's own words for its setup (".xz -> vec2"), else the
         * resolved wildcard type. */
        Subtitle = ToQ(Doc.GetSchema().DescribeNode(G, InNode));
        if(Subtitle.isEmpty() && InDef->HasWildcard() && !InDef->bWildcardFromSchema)
        {
            Subtitle = ToQ(Doc.GetSchema().GetWildcardType(G, InNode));
        }

        /* A node limited to some of the schema's stages says which: "Pixel". */
        QStringList Stages;
        bool bLimited = false;
        for(uint32_t Bit = 1; Bit != 0; Bit <<= 1)
        {
            const std::string Stage = Doc.GetSchema().GetStageName(Bit);
            if(Stage.empty()) { continue; }
            if(InDef->StageMask & Bit) { Stages << ToQ(Stage); }
            else                       { bLimited = true; }
        }
        if(bLimited && !Stages.isEmpty())
        {
            QString Badge = Stages.join(QStringLiteral(", "));
            Badge[0] = Badge[0].toUpper();
            Subtitle = Subtitle.isEmpty() ? Badge : Subtitle + QStringLiteral(" \u00B7 ") + Badge;
        }

        for(const PinDefinition& Pin : InDef->Pins)
        {
            PinSlot Slot;
            Slot.Name     = Pin.Name;
            Slot.bOutput  = Pin.IsOutput();
            Slot.bExec    = Pin.IsExec();
            Slot.TypeName = Pin.IsExec() ? std::string() : Doc.GetSchema().GetPinType(G, InNode, Pin);
            Slot.Color    = Slot.bExec ? (IsDarkTheme() ? QColor(0xE8, 0xE8, 0xE8) : QColor(0x40, 0x40, 0x40))
                                       : Scene.TypeColor(Slot.TypeName);
            Slot.bLinked  = Doc.IsLinked(PinRef{ Id, Pin.Name });
            Slot.Label    = Pin.IsExec() && (Pin.Name == "Exec" || Pin.Name == "Then") ? QString() : ToQ(Pin.Name);

            /* An unlinked input shows the value it will get. */
            if(Pin.IsInput() && !Pin.IsExec() && !Slot.bLinked)
            {
                const PinTypeInfo* Info = Doc.GetSchema().FindType(Slot.TypeName);
                if(!Pin.Implicit.empty() && !InNode.FindConstant(Pin.Name))
                {
                    Slot.Label += QStringLiteral(" = ") + Elide(ToQ(Pin.Implicit), LiteralChars);
                }
                else if(Info && Info->bHasLiteral)
                {
                    const std::string* Given = InNode.FindConstant(Pin.Name);
                    const std::string Value = Given ? *Given : (!Pin.Default.empty() ? Pin.Default : Info->DefaultLiteral);
                    Slot.Label += QStringLiteral(" = ") + Elide(Info->Name == "string" ? QStringLiteral("\"%1\"").arg(ToQ(Value)) : ToQ(Value), LiteralChars);
                }
                else if(Slot.TypeName == "Entity")
                {
                    Slot.Label += QStringLiteral(" = Self");
                }
            }
            Pins.push_back(Slot);
        }

        if(!InDef->Tooltip.empty()) { setToolTip(ToQ(InDef->Tooltip)); }
    }

    /* Layout: execution pins first on each side, then values. */
    std::stable_sort(Pins.begin(), Pins.end(), [](const PinSlot& L, const PinSlot& R) { return L.bExec && !R.bExec; });

    const QFontMetricsF Metrics(QApplication::font());
    QFont Bold = QApplication::font();
    Bold.setBold(true);
    const QFontMetricsF BoldMetrics(Bold);

    qreal InWidth = 0.0, OutWidth = 0.0;
    int InRows = 0, OutRows = 0;
    for(const PinSlot& Slot : Pins)
    {
        const qreal LabelWidth = Metrics.horizontalAdvance(Slot.Label);
        if(Slot.bOutput) { OutWidth = std::max(OutWidth, LabelWidth); ++OutRows; }
        else             { InWidth  = std::max(InWidth, LabelWidth);  ++InRows; }
    }

    QFont Small = QApplication::font();
    Small.setPointSizeF(Small.pointSizeF() * 0.8);
    const qreal SubtitleWidth = Subtitle.isEmpty() ? 0.0 : QFontMetricsF(Small).horizontalAdvance(Subtitle) + 12.0;

    Width  = std::max({ 120.0, BoldMetrics.horizontalAdvance(Title) + SubtitleWidth + 28.0, InWidth + OutWidth + 44.0 });
    Width  = std::ceil(Width / 10.0) * 10.0;
    Height = HeaderHeight + 8.0 + std::max(1, std::max(InRows, OutRows)) * RowHeight;

    int InIndex = 0, OutIndex = 0;
    for(PinSlot& Slot : Pins)
    {
        const int Row = Slot.bOutput ? OutIndex++ : InIndex++;
        Slot.Local = QPointF(Slot.bOutput ? Width : 0.0, HeaderHeight + 4.0 + Row * RowHeight + RowHeight * 0.5);
    }
}

QRectF NodeItem::boundingRect() const
{
    return QRectF(-PinRadius - 3.0, -3.0, Width + 2.0 * PinRadius + 6.0, Height + 6.0);
}

void NodeItem::paint(QPainter* InPainter, const QStyleOptionGraphicsItem* /*InOption*/, QWidget* /*InWidget*/)
{
    const bool bDark = IsDarkTheme();
    const QPalette Palette = QApplication::palette();
    InPainter->setRenderHint(QPainter::Antialiasing);

    const QRectF Body(0.0, 0.0, Width, Height);

    // --- Body and header -----------------------------------------------------
    QPainterPath BodyPath;
    BodyPath.addRoundedRect(Body, Corner, Corner);
    InPainter->fillPath(BodyPath, bDark ? QColor(0x2B, 0x2D, 0x31, 235) : QColor(0xF4, 0xF4, 0xF6, 240));

    QPainterPath HeaderPath;
    HeaderPath.addRoundedRect(QRectF(0.0, 0.0, Width, HeaderHeight + Corner), Corner, Corner);
    InPainter->save();
    InPainter->setClipRect(QRectF(0.0, 0.0, Width, HeaderHeight));
    InPainter->fillPath(HeaderPath, HeaderColor);
    InPainter->restore();

    QFont Bold = QApplication::font();
    Bold.setBold(true);
    InPainter->setFont(Bold);
    InPainter->setPen(Qt::white);
    InPainter->drawText(QRectF(10.0, 0.0, Width - 20.0, HeaderHeight), Qt::AlignVCenter | Qt::AlignLeft, Title);

    if(!Subtitle.isEmpty())
    {
        QFont Small = QApplication::font();
        Small.setPointSizeF(Small.pointSizeF() * 0.8);
        InPainter->setFont(Small);
        InPainter->setPen(QColor(255, 255, 255, 190));
        InPainter->drawText(QRectF(10.0, 0.0, Width - 18.0, HeaderHeight), Qt::AlignVCenter | Qt::AlignRight, Subtitle);
    }

    // --- Outline: selection, then any message --------------------------------
    QPen Outline(bDark ? QColor(0x18, 0x18, 0x1A) : QColor(0xA0, 0xA0, 0xA8), 1.0);
    if(bHasMessage)
    {
        Outline = QPen(bMessageIsError ? QColor(0xE0, 0x40, 0x40) : QColor(0xE0, 0xA0, 0x30), 2.0);
    }
    if(isSelected())
    {
        Outline = QPen(Palette.color(QPalette::Highlight).lighter(bDark ? 130 : 100), 2.0);
    }
    if(bPlaceholder && !isSelected())
    {
        Outline = QPen(QColor(0xE0, 0x40, 0x40), 2.0, Qt::DashLine);
    }
    InPainter->setPen(Outline);
    InPainter->setBrush(Qt::NoBrush);
    InPainter->drawRoundedRect(Body, Corner, Corner);

    // --- Pins ------------------------------------------------------------------
    InPainter->setFont(QApplication::font());
    const QColor TextColor = Palette.color(QPalette::WindowText);
    for(const PinSlot& Slot : Pins)
    {
        const bool bCandidate = std::find(Candidates.begin(), Candidates.end(), Slot.Name) != Candidates.end();
        if(bCandidate)
        {
            InPainter->setPen(QPen(Palette.color(QPalette::Highlight), 2.0));
            InPainter->setBrush(Qt::NoBrush);
            InPainter->drawEllipse(Slot.Local, PinRadius + 4.0, PinRadius + 4.0);
        }

        InPainter->setPen(QPen(Slot.Color, 1.5));
        InPainter->setBrush(Slot.bLinked ? QBrush(Slot.Color) : QBrush(Qt::NoBrush));
        if(Slot.bExec)
        {
            const QPointF C = Slot.Local;
            const QPolygonF Arrow({ C + QPointF(-PinRadius, -PinRadius), C + QPointF(PinRadius, 0.0), C + QPointF(-PinRadius, PinRadius) });
            InPainter->drawPolygon(Arrow);
        }
        else
        {
            InPainter->drawEllipse(Slot.Local, PinRadius, PinRadius);
        }

        if(!Slot.Label.isEmpty())
        {
            InPainter->setPen(TextColor);
            const QRectF Row = Slot.bOutput
                             ? QRectF(Slot.Local.x() - Width + 10.0, Slot.Local.y() - RowHeight * 0.5, Width - 22.0, RowHeight)
                             : QRectF(Slot.Local.x() + 12.0, Slot.Local.y() - RowHeight * 0.5, Width - 22.0, RowHeight);
            InPainter->drawText(Row, Qt::AlignVCenter | (Slot.bOutput ? Qt::AlignRight : Qt::AlignLeft), Slot.Label);
        }
    }
}

const NodeItem::PinSlot* NodeItem::PinAt(const QPointF& InScenePos) const
{
    const QPointF Local = mapFromScene(InScenePos);
    const PinSlot* Best = nullptr;
    qreal BestDistance = PinHitRadius;
    for(const PinSlot& Slot : Pins)
    {
        const qreal Distance = std::hypot(Local.x() - Slot.Local.x(), Local.y() - Slot.Local.y());
        if(Distance <= BestDistance)
        {
            Best = &Slot;
            BestDistance = Distance;
        }
    }
    return Best;
}

QPointF NodeItem::PinScenePos(const std::string& InPin, const bool bInOutput) const
{
    for(const PinSlot& Slot : Pins)
    {
        if(Slot.Name == InPin && Slot.bOutput == bInOutput) { return mapToScene(Slot.Local); }
    }
    return mapToScene(QPointF(bInOutput ? Width : 0.0, HeaderHeight * 0.5));
}

void NodeItem::SetMessage(const bool bInError, const QString& InText)
{
    bHasMessage = !InText.isEmpty();
    bMessageIsError = bInError;
    if(bHasMessage) { setToolTip(InText); }
    update();
}

void NodeItem::SetCandidatePins(const std::vector<std::string>& InPins)
{
    Candidates = InPins;
    update();
}

QVariant NodeItem::itemChange(const GraphicsItemChange InChange, const QVariant& InValue)
{
    if(InChange == ItemPositionHasChanged)
    {
        for(LinkItem* Each : Links) { Each->UpdatePath(); }
    }
    return QGraphicsItem::itemChange(InChange, InValue);
}

// === LinkItem ===================================================================

LinkItem::LinkItem(const NodeGraph::Link& InLink, NodeItem* InFrom, NodeItem* InTo, const QColor& InColor, const bool bInExec)
    : Link(InLink)
    , From(InFrom)
    , To(InTo)
{
    setPen(QPen(InColor, bInExec ? 2.5 : 2.0, Qt::SolidLine, Qt::RoundCap));
    setZValue(-1.0);
    setFlag(ItemIsSelectable, false);
    UpdatePath();
}

LinkItem::LinkItem(NodeItem* InAnchor, const std::string& InPin, const bool bInFromOutput, const QColor& InColor, const bool bInExec)
    : From(bInFromOutput ? InAnchor : nullptr)
    , To(bInFromOutput ? nullptr : InAnchor)
    , AnchorPin(InPin)
    , bAnchorIsOutput(bInFromOutput)
{
    setPen(QPen(InColor, bInExec ? 2.5 : 2.0, Qt::DashLine, Qt::RoundCap));
    setZValue(10.0);
    FreeEnd = InAnchor->PinScenePos(InPin, bInFromOutput);
    UpdatePath();
}

void LinkItem::SetFreeEnd(const QPointF& InScenePos)
{
    FreeEnd = InScenePos;
    UpdatePath();
}

void LinkItem::UpdatePath()
{
    QPointF Start, End;
    if(!AnchorPin.empty())
    {
        const QPointF Anchor = (bAnchorIsOutput ? From : To)->PinScenePos(AnchorPin, bAnchorIsOutput);
        Start = bAnchorIsOutput ? Anchor : FreeEnd;
        End   = bAnchorIsOutput ? FreeEnd : Anchor;
    }
    else
    {
        Start = From->PinScenePos(Link.From.Pin, true);
        End   = To->PinScenePos(Link.To.Pin, false);
    }

    const qreal Reach = std::max(40.0, std::abs(End.x() - Start.x()) * 0.5);
    QPainterPath Path(Start);
    Path.cubicTo(Start + QPointF(Reach, 0.0), End - QPointF(Reach, 0.0), End);
    setPath(Path);
}

// === CommentItem ================================================================

CommentItem::CommentItem(const int InIndex, const Comment& InComment)
    : QGraphicsRectItem(0.0, 0.0, InComment.Width, InComment.Height)
    , Index(InIndex)
    , Text(ToQ(InComment.Text))
{
    setPos(InComment.X, InComment.Y);
    setZValue(-10.0);
    setFlags(ItemIsMovable | ItemIsSelectable);
    setToolTip(QStringLiteral("Double-click to edit the comment"));
}

void CommentItem::paint(QPainter* InPainter, const QStyleOptionGraphicsItem* /*InOption*/, QWidget* /*InWidget*/)
{
    const bool bDark = IsDarkTheme();
    InPainter->setRenderHint(QPainter::Antialiasing);

    const QRectF Box = rect();
    InPainter->setPen(isSelected() ? QPen(QApplication::palette().color(QPalette::Highlight), 2.0)
                                   : QPen(bDark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 40), 1.0));
    InPainter->setBrush(bDark ? QColor(255, 255, 255, 14) : QColor(0, 0, 0, 12));
    InPainter->drawRoundedRect(Box, 8.0, 8.0);

    QFont Heading = QApplication::font();
    Heading.setBold(true);
    Heading.setPointSizeF(Heading.pointSizeF() * 1.2);
    InPainter->setFont(Heading);
    InPainter->setPen(QApplication::palette().color(QPalette::WindowText));
    InPainter->drawText(Box.adjusted(10.0, 6.0, -10.0, 0.0), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, Text);
}

#endif // WITH_EDITOR
