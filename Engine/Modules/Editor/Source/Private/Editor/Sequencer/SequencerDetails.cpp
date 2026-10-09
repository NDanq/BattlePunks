#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerDetails.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Public/RenderCore/PostProcessStack.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"

using Sequencer::ESequenceTrackType;
using Sequencer::SequenceAsset;
using Sequencer::SequenceTrack;

SequencerDetails::SequencerDetails(SequencerDocument& InDocument, QWidget* InParent)
    : QWidget(InParent), Document(InDocument)
{
    setObjectName(QStringLiteral("SequencerDetails"));
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    auto* Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    Layout->addWidget(Scroll);
    Body = new QWidget(Scroll);
    Scroll->setWidget(Body);
    Form = new QFormLayout(Body);
    Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    /* Not while a field is being typed in, nor mid-drag: it rebuilds when that ends. */
    const auto Refresh = [this]()
    {
        if(Document.IsDragging()) { return; }
        QWidget* Focus = QApplication::focusWidget();
        if(Focus && Body->isAncestorOf(Focus)) { return; }
        Rebuild();
    };
    connect(&Document, &SequencerDocument::SelectionChanged, this, [this]() { Rebuild(); });
    connect(&Document, &SequencerDocument::Changed, this, Refresh);
    Rebuild();
}

void SequencerDetails::Rebuild()
{
    while(Form->rowCount() > 0) { Form->removeRow(0); }

    const std::vector<SequencerItem>& Selection = Document.GetSelection();
    if(Selection.size() > 1)
    {
        AddNote(QStringLiteral("%1 items selected. Drag them on the timeline to move them together; Delete removes them.").arg(Selection.size()));
        return;
    }
    if(Selection.size() == 1)
    {
        const SequencerItem& Item = Selection.front();
        switch(Item.Kind)
        {
            case SequencerItem::EKind::Key:     BuildKey(Item); return;
            case SequencerItem::EKind::Section: BuildSection(Item); return;
            case SequencerItem::EKind::Event:   BuildEvent(Item); return;
            case SequencerItem::EKind::Marker:  BuildMarker(Item); return;
        }
    }
    if(Document.GetCurrentTrack().IsValid() && Document.FindTrack(Document.GetCurrentTrack()))
    {
        BuildTrack(Document.GetCurrentTrack());
        return;
    }
    if(Document.GetCurrentBinding() >= 0 && size_t(Document.GetCurrentBinding()) < Document.GetAsset().Bindings.size())
    {
        BuildBinding(Document.GetCurrentBinding());
        return;
    }
    BuildSequence();
}

// === Rows ========================================================================

void SequencerDetails::AddInt(const QString& InLabel, const int InValue, const int InMin, const int InMax,
                              const std::function<void(SequenceAsset&, int)>& InSet)
{
    auto* Box = new QSpinBox(Body);
    Box->setRange(InMin, InMax);
    Box->setValue(InValue);
    Box->setKeyboardTracking(false);
    Box->setObjectName(InLabel);
    connect(Box, &QSpinBox::editingFinished, this, [this, Box, InLabel, InSet]()
    {
        const int Value = Box->value();
        Document.Edit(QStringLiteral("Set %1").arg(InLabel), [&](SequenceAsset& InOut) { InSet(InOut, Value); });
    });
    Form->addRow(InLabel, Box);
}

void SequencerDetails::AddFloat(const QString& InLabel, const double InValue, const double InMin, const double InMax, const int InDecimals,
                                const std::function<void(SequenceAsset&, double)>& InSet)
{
    auto* Box = new QDoubleSpinBox(Body);
    Box->setRange(InMin, InMax);
    Box->setDecimals(InDecimals);
    Box->setValue(InValue);
    Box->setKeyboardTracking(false);
    Box->setObjectName(InLabel);
    connect(Box, &QDoubleSpinBox::editingFinished, this, [this, Box, InLabel, InSet]()
    {
        const double Value = Box->value();
        Document.Edit(QStringLiteral("Set %1").arg(InLabel), [&](SequenceAsset& InOut) { InSet(InOut, Value); });
    });
    Form->addRow(InLabel, Box);
}

void SequencerDetails::AddText(const QString& InLabel, const QString& InValue, const std::function<void(SequenceAsset&, const QString&)>& InSet,
                               const QString& InBrowseFilter)
{
    auto* Edit = new QLineEdit(InValue, Body);
    Edit->setObjectName(InLabel);
    const auto Commit = [this, InLabel, InSet](const QString& InText)
    {
        Document.Edit(QStringLiteral("Set %1").arg(InLabel), [&](SequenceAsset& InOut) { InSet(InOut, InText); });
    };
    connect(Edit, &QLineEdit::editingFinished, this, [Edit, Commit]() { Commit(Edit->text().trimmed()); });
    if(InBrowseFilter.isEmpty())
    {
        Form->addRow(InLabel, Edit);
        return;
    }
    /* A file picker beside it, giving the runtime path. */
    auto* Row    = new QWidget(Body);
    auto* Layout = new QHBoxLayout(Row);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->addWidget(Edit, 1);
    auto* Browse = new QPushButton(QStringLiteral("..."), Row);
    Browse->setFixedWidth(28);
    Layout->addWidget(Browse);
    connect(Browse, &QPushButton::clicked, this, [this, Edit, Commit, InLabel, InBrowseFilter]()
    {
        const QString File = QFileDialog::getOpenFileName(this, InLabel, EditorGraphs::GetProjectRoot(), InBrowseFilter);
        if(File.isEmpty()) { return; }
        const QString Runtime = EditorContent::ToRuntimePath(File);
        if(Runtime.isEmpty()) { return; }
        Edit->setText(Runtime);
        Commit(Runtime);
    });
    Form->addRow(InLabel, Row);
}

void SequencerDetails::AddCheck(const QString& InLabel, const bool bInValue, const std::function<void(SequenceAsset&, bool)>& InSet)
{
    auto* Box = new QCheckBox(Body);
    Box->setChecked(bInValue);
    Box->setObjectName(InLabel);
    connect(Box, &QCheckBox::toggled, this, [this, InLabel, InSet](const bool bInOn)
    {
        Document.Edit(QStringLiteral("Set %1").arg(InLabel), [&](SequenceAsset& InOut) { InSet(InOut, bInOn); });
    });
    Form->addRow(InLabel, Box);
}

void SequencerDetails::AddChoice(const QString& InLabel, const QStringList& InChoices, const int InCurrent,
                                 const std::function<void(SequenceAsset&, int)>& InSet)
{
    auto* Box = new QComboBox(Body);
    Box->addItems(InChoices);
    Box->setCurrentIndex(InCurrent);
    Box->setObjectName(InLabel);
    connect(Box, qOverload<int>(&QComboBox::activated), this, [this, InLabel, InSet](const int InIndex)
    {
        Document.Edit(QStringLiteral("Set %1").arg(InLabel), [&](SequenceAsset& InOut) { InSet(InOut, InIndex); });
    });
    Form->addRow(InLabel, Box);
}

void SequencerDetails::AddNote(const QString& InText)
{
    auto* Label = new QLabel(InText, Body);
    Label->setWordWrap(true);
    Label->setForegroundRole(QPalette::PlaceholderText);
    Form->addRow(Label);
}

// === Forms =======================================================================

void SequencerDetails::BuildSequence()
{
    const SequenceAsset& Asset = Document.GetAsset();
    AddNote(QStringLiteral("The sequence"));
    AddInt(QStringLiteral("Frame rate"), int(Asset.FrameRate), 1, 1000, [](SequenceAsset& InOut, const int InValue)
    {
        /* Keys keep their time, not their frame. */
        const double Scale = double(InValue) / double(std::max<uint32_t>(InOut.FrameRate, 1));
        const auto Rescale = [Scale](int32_t& InOutFrame) { InOutFrame = int32_t(std::lround(InOutFrame * Scale)); };
        const auto Track = [&](Sequencer::SequenceTrack& InOutTrack)
        {
            for(auto& Channel : InOutTrack.Channels) { for(auto& Key : Channel.Keys) { Rescale(Key.Frame); } }
            for(auto& Event : InOutTrack.Events) { Rescale(Event.Frame); }
            for(auto& Section : InOutTrack.Sections)
            {
                Rescale(Section.Start); Rescale(Section.End); Rescale(Section.Offset); Rescale(Section.BlendIn); Rescale(Section.BlendOut);
            }
        };
        for(auto& Binding : InOut.Bindings) { for(auto& Each : Binding.Tracks) { Track(Each); } }
        for(auto& Each : InOut.Tracks) { Track(Each); }
        for(auto& Marker : InOut.Markers) { Rescale(Marker.Frame); }
        Rescale(InOut.Length);
        InOut.FrameRate = uint32_t(InValue);
    });
    AddInt(QStringLiteral("Length (frames)"), Asset.Length, 1, 1000000, [](SequenceAsset& InOut, const int InValue) { InOut.Length = InValue; });
    AddNote(QStringLiteral("%1 s long, %2 bindings, %3 markers.").arg(Asset.GetDurationSeconds(), 0, 'f', 2)
                .arg(Asset.Bindings.size()).arg(Asset.Markers.size()));
    const std::vector<std::string> Problems = Asset.Validate();
    for(const std::string& Problem : Problems) { AddNote(QStringLiteral("⚠ %1").arg(QString::fromStdString(Problem))); }
}

void SequencerDetails::BuildBinding(const int InBinding)
{
    const Sequencer::SequenceBinding& Binding = Document.GetAsset().Bindings[size_t(InBinding)];
    const bool bSpawn = Binding.Kind == Sequencer::ESequenceBindingKind::Spawn;
    AddNote(bSpawn ? QStringLiteral("A spawned binding: made from its prefab while the sequence plays")
                   : QStringLiteral("A binding to an entity of the level"));
    AddText(QStringLiteral("Id"), QString::fromStdString(Binding.Id), [InBinding](SequenceAsset& InOut, const QString& InText)
    {
        if(InText.isEmpty() || InOut.FindBinding(InText.toStdString()) >= 0) { return; }
        const std::string Old = InOut.Bindings[size_t(InBinding)].Id;
        const std::string New = InText.toStdString();
        InOut.Bindings[size_t(InBinding)].Id = New;
        /* What names it by id follows: cameras, @id arguments. */
        const auto Rename = [&](Sequencer::SequenceTrack& InOutTrack)
        {
            for(auto& Section : InOutTrack.Sections) { if(InOutTrack.Type == ESequenceTrackType::CameraCut && Section.Asset == Old) { Section.Asset = New; } }
            for(auto& Event : InOutTrack.Events)
            {
                for(auto& Arg : Event.Args)
                {
                    if(Arg.Type == Sequencer::SequenceEventArg::EType::Binding && Arg.Text == Old) { Arg.Text = New; }
                }
            }
        };
        for(auto& Each : InOut.Bindings) { for(auto& Track : Each.Tracks) { Rename(Track); } }
        for(auto& Track : InOut.Tracks) { Rename(Track); }
    });
    AddText(QStringLiteral("Name"), QString::fromStdString(Binding.Name), [InBinding](SequenceAsset& InOut, const QString& InText)
    {
        InOut.Bindings[size_t(InBinding)].Name = InText.toStdString();
    });
    if(bSpawn)
    {
        AddText(QStringLiteral("Prefab"), QString::fromStdString(Binding.Prefab), [InBinding](SequenceAsset& InOut, const QString& InText)
        {
            InOut.Bindings[size_t(InBinding)].Prefab = InText.toStdString();
        }, QStringLiteral("Prefabs (*.nmprefab)"));
    }
    else
    {
        char Guid[GameCore::EntityGuid::STRING_BUFFER] = {};
        if(Binding.Guid.IsSet()) { Binding.Guid.ToString(Guid); }
        auto* Label = new QLabel(Binding.Guid.IsSet() ? QString::fromLatin1(Guid) : QStringLiteral("(by name)"), Body);
        Label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        Form->addRow(QStringLiteral("Guid"), Label);
        auto* Rebind = new QPushButton(QStringLiteral("Bind to Selected Entity"), Body);
        Rebind->setObjectName(QStringLiteral("BindToSelected"));
        connect(Rebind, &QPushButton::clicked, this, [this, InBinding]() { emit BindToSelectedRequested(InBinding); });
        Form->addRow(Rebind);
    }
}

void SequencerDetails::BuildTrack(const SequencerTrackRef& InTrack)
{
    const SequenceTrack* Track = Document.FindTrack(InTrack);
    AddNote(QStringLiteral("%1 track").arg(QString::fromLatin1(Sequencer::TrackTypeName(Track->Type))));
    AddText(QStringLiteral("Name"), QString::fromStdString(Track->Name), [this, InTrack](SequenceAsset&, const QString& InText)
    {
        if(SequenceTrack* Live = Document.FindLiveTrack(InTrack)) { Live->Name = InText.toStdString(); }
    });
    AddCheck(QStringLiteral("Muted"), Track->bMuted, [this, InTrack](SequenceAsset&, const bool bInOn)
    {
        if(SequenceTrack* Live = Document.FindLiveTrack(InTrack)) { Live->bMuted = bInOn; }
    });
    if(Track->Type == ESequenceTrackType::Float || Track->Type == ESequenceTrackType::Bool)
    {
        AddText(QStringLiteral("Component"), QString::fromStdString(Track->Component), [this, InTrack](SequenceAsset&, const QString& InText)
        {
            if(SequenceTrack* Live = Document.FindLiveTrack(InTrack)) { Live->Component = InText.toStdString(); }
        });
        AddText(QStringLiteral("Field"), QString::fromStdString(Track->Field), [this, InTrack](SequenceAsset&, const QString& InText)
        {
            if(SequenceTrack* Live = Document.FindLiveTrack(InTrack)) { Live->Field = InText.toStdString(); }
        });
    }
    if(Track->Type == ESequenceTrackType::PostProcessSetting)
    {
        QStringList Settings;
        int Current = -1;
        for(size_t Index = 0; Index < size_t(RenderCore::EPostProcessParam::Count); ++Index)
        {
            Settings << QString::fromLatin1(RenderCore::PostProcessParamName(RenderCore::EPostProcessParam(Index)));
            if(Track->Field == RenderCore::PostProcessParamName(RenderCore::EPostProcessParam(Index))) { Current = int(Index); }
        }
        AddChoice(QStringLiteral("Setting"), Settings, Current, [this, InTrack](SequenceAsset&, const int InIndex)
        {
            if(SequenceTrack* Live = Document.FindLiveTrack(InTrack))
            {
                Live->Field = RenderCore::PostProcessParamName(RenderCore::EPostProcessParam(InIndex));
            }
        });
        AddNote(QStringLiteral("Keyed over whatever the level's volumes give, and turns its effect on."));
    }
    AddNote(Sequencer::IsSectionTrack(Track->Type) ? QStringLiteral("Double-click its lane to add a section.")
          : Track->Type == ESequenceTrackType::Event ? QStringLiteral("Double-click its lane to add an event.")
          : QStringLiteral("Double-click its lane (a Transform track's channels) to add a key; K keys the binding's pose."));
}

void SequencerDetails::BuildKey(const SequencerItem& InItem)
{
    const SequenceTrack* Track = Document.FindTrack(InItem.Track);
    if(!Track) { return; }
    const Sequencer::SequenceKey& Key = Track->Channels[size_t(InItem.Channel)].Keys[size_t(InItem.Index)];
    AddNote(Track->Type == ESequenceTrackType::Transform ? QStringLiteral("%1 key").arg(QString::fromLatin1(Sequencer::TransformChannelName(size_t(InItem.Channel))))
                                                          : QStringLiteral("Key"));
    const SequencerItem Item = InItem;
    const auto Live = [this, Item]() -> Sequencer::SequenceKey*
    {
        SequenceTrack* T = Document.FindLiveTrack(Item.Track);
        return T ? &T->Channels[size_t(Item.Channel)].Keys[size_t(Item.Index)] : nullptr;
    };
    AddInt(QStringLiteral("Frame"), Key.Frame, 0, 1000000, [Live](SequenceAsset&, const int InValue) { if(auto* K = Live()) { K->Frame = InValue; } });
    if(Track->Type == ESequenceTrackType::Bool)
    {
        AddCheck(QStringLiteral("On"), Key.Value != 0.0f, [Live](SequenceAsset&, const bool bInOn) { if(auto* K = Live()) { K->Value = bInOn ? 1.0f : 0.0f; } });
        return;
    }
    AddFloat(QStringLiteral("Value"), Key.Value, -1e9, 1e9, 4, [Live](SequenceAsset&, const double InValue) { if(auto* K = Live()) { K->Value = float(InValue); } });
    QStringList Interps;
    for(size_t Index = 0; Index < size_t(Sequencer::ESequenceInterp::Count); ++Index)
    {
        Interps << QString::fromLatin1(Sequencer::InterpName(Sequencer::ESequenceInterp(Index)));
    }
    AddChoice(QStringLiteral("Interpolation"), Interps, int(Key.Interp), [Live](SequenceAsset&, const int InIndex)
    {
        if(auto* K = Live()) { K->Interp = Sequencer::ESequenceInterp(InIndex); }
    });
    if(Key.Interp == Sequencer::ESequenceInterp::User)
    {
        AddFloat(QStringLiteral("Arrive tangent"), Key.InTangent, -1e6, 1e6, 4, [Live](SequenceAsset&, const double V) { if(auto* K = Live()) { K->InTangent = float(V); } });
        AddFloat(QStringLiteral("Leave tangent"), Key.OutTangent, -1e6, 1e6, 4, [Live](SequenceAsset&, const double V) { if(auto* K = Live()) { K->OutTangent = float(V); } });
    }
}

void SequencerDetails::BuildSection(const SequencerItem& InItem)
{
    const SequenceTrack* Track = Document.FindTrack(InItem.Track);
    if(!Track) { return; }
    const Sequencer::SequenceSection& Section = Track->Sections[size_t(InItem.Index)];
    const SequencerItem Item = InItem;
    const auto Live = [this, Item]() -> Sequencer::SequenceSection*
    {
        SequenceTrack* T = Document.FindLiveTrack(Item.Track);
        return T ? &T->Sections[size_t(Item.Index)] : nullptr;
    };
    AddNote(QStringLiteral("%1 section").arg(QString::fromLatin1(Sequencer::TrackTypeName(Track->Type))));

    switch(Track->Type)
    {
        case ESequenceTrackType::CameraCut:
        {
            QStringList Cameras;
            int Current = -1;
            const auto& Bindings = Document.GetAsset().Bindings;
            for(size_t Index = 0; Index < Bindings.size(); ++Index)
            {
                Cameras << QString::fromStdString(Bindings[Index].Name.empty() ? Bindings[Index].Id : Bindings[Index].Name);
                if(Bindings[Index].Id == Section.Asset) { Current = int(Index); }
            }
            AddChoice(QStringLiteral("Camera"), Cameras, Current, [Live](SequenceAsset& InOut, const int InIndex)
            {
                if(auto* S = Live(); S && InIndex >= 0 && size_t(InIndex) < InOut.Bindings.size()) { S->Asset = InOut.Bindings[size_t(InIndex)].Id; }
            });
            break;
        }
        case ESequenceTrackType::Animation:
            AddText(QStringLiteral("Clip"), QString::fromStdString(Section.Asset), [Live](SequenceAsset&, const QString& InText)
            {
                if(auto* S = Live()) { S->Asset = InText.toStdString(); }
            }, QStringLiteral("Animation clips (*.nmanim)"));
            break;
        case ESequenceTrackType::Audio:
            AddText(QStringLiteral("Sound"), QString::fromStdString(Section.Asset), [Live](SequenceAsset&, const QString& InText)
            {
                if(auto* S = Live()) { S->Asset = InText.toStdString(); }
            }, QStringLiteral("Sounds (*.nmsound)"));
            break;
        case ESequenceTrackType::PostProcess:
            AddText(QStringLiteral("Stack"), QString::fromStdString(Section.Asset), [Live](SequenceAsset&, const QString& InText)
            {
                if(auto* S = Live()) { S->Asset = InText.toStdString(); }
            }, QStringLiteral("Post Process Stacks (*.pp.nmgraph)"));
            AddFloat(QStringLiteral("Weight"), Section.Volume, 0.0, 1.0, 2, [Live](SequenceAsset&, const double V) { if(auto* S = Live()) { S->Volume = float(V); } });
            break;
        case ESequenceTrackType::SubSequence:
            AddText(QStringLiteral("Sequence"), QString::fromStdString(Section.Asset), [Live](SequenceAsset&, const QString& InText)
            {
                if(auto* S = Live()) { S->Asset = InText.toStdString(); }
            }, QStringLiteral("Sequences (*.nmseq)"));
            break;
        default:
            break;
    }

    AddInt(QStringLiteral("Start"), Section.Start, 0, 1000000, [Live](SequenceAsset&, const int V) { if(auto* S = Live()) { const int L = S->GetLength(); S->Start = V; S->End = V + L; } });
    AddInt(QStringLiteral("Length"), Section.GetLength(), 1, 1000000, [Live](SequenceAsset&, const int V) { if(auto* S = Live()) { S->End = S->Start + V; } });
    if(Track->Type != ESequenceTrackType::CameraCut && Track->Type != ESequenceTrackType::PostProcess)
    {
        AddFloat(QStringLiteral("Play rate"), Section.PlayRate, 0.01, 100.0, 3, [Live](SequenceAsset&, const double V) { if(auto* S = Live()) { S->PlayRate = float(V); } });
        AddInt(QStringLiteral("Offset"), Section.Offset, 0, 1000000, [Live](SequenceAsset&, const int V) { if(auto* S = Live()) { S->Offset = V; } });
    }
    AddInt(QStringLiteral("Blend in"), Section.BlendIn, 0, 1000000, [Live](SequenceAsset&, const int V) { if(auto* S = Live()) { S->BlendIn = V; } });
    if(Track->Type == ESequenceTrackType::Audio || Track->Type == ESequenceTrackType::Animation || Track->Type == ESequenceTrackType::PostProcess)
    {
        AddInt(QStringLiteral("Blend out"), Section.BlendOut, 0, 1000000, [Live](SequenceAsset&, const int V) { if(auto* S = Live()) { S->BlendOut = V; } });
    }
    if(Track->Type == ESequenceTrackType::Audio)
    {
        AddFloat(QStringLiteral("Volume"), Section.Volume, 0.0, 4.0, 2, [Live](SequenceAsset&, const double V) { if(auto* S = Live()) { S->Volume = float(V); } });
    }
    if(Track->Type == ESequenceTrackType::Animation)
    {
        AddCheck(QStringLiteral("Loop"), Section.bLoop, [Live](SequenceAsset&, const bool bOn) { if(auto* S = Live()) { S->bLoop = bOn; } });
    }
}

void SequencerDetails::BuildEvent(const SequencerItem& InItem)
{
    const SequenceTrack* Track = Document.FindTrack(InItem.Track);
    if(!Track) { return; }
    const Sequencer::SequenceEventKey& Event = Track->Events[size_t(InItem.Index)];
    const SequencerItem Item = InItem;
    const auto Live = [this, Item]() -> Sequencer::SequenceEventKey*
    {
        SequenceTrack* T = Document.FindLiveTrack(Item.Track);
        return T ? &T->Events[size_t(Item.Index)] : nullptr;
    };
    AddNote(QStringLiteral("An event: emitted on the World's EventBus as the playhead passes it (Lua: Events.On)"));
    AddInt(QStringLiteral("Frame"), Event.Frame, 0, 1000000, [Live](SequenceAsset&, const int V) { if(auto* E = Live()) { E->Frame = V; } });
    AddText(QStringLiteral("Event"), QString::fromStdString(Event.Event), [Live](SequenceAsset&, const QString& InText) { if(auto* E = Live()) { E->Event = InText.toStdString(); } });
    for(int Arg = 0; Arg < int(Sequencer::SEQUENCE_EVENT_ARGS_MAX); ++Arg)
    {
        const QString Text = size_t(Arg) < Event.Args.size() ? QString::fromStdString(Event.Args[size_t(Arg)].ToText()) : QString();
        AddText(QStringLiteral("Argument %1").arg(Arg + 1), Text, [Live, Arg](SequenceAsset&, const QString& InText)
        {
            auto* E = Live();
            if(!E) { return; }
            if(InText.isEmpty())
            {
                if(size_t(Arg) < E->Args.size()) { E->Args.resize(size_t(Arg)); }
                return;
            }
            if(E->Args.size() <= size_t(Arg)) { E->Args.resize(size_t(Arg) + 1, Sequencer::SequenceEventArg::FromText("")); }
            E->Args[size_t(Arg)] = Sequencer::SequenceEventArg::FromText(InText.toStdString());
        });
    }
    AddNote(QStringLiteral("An argument is a number, true or false, @id for a binding's entity, or text."));
    AddCheck(QStringLiteral("Fire on skip"), Event.bFireOnSkip, [Live](SequenceAsset&, const bool bOn) { if(auto* E = Live()) { E->bFireOnSkip = bOn; } });
}

void SequencerDetails::BuildMarker(const SequencerItem& InItem)
{
    const auto& Marker = Document.GetAsset().Markers[size_t(InItem.Index)];
    const int Index = InItem.Index;
    AddNote(QStringLiteral("A marker: a named frame (Lua: Sequence.SeekToMarker)"));
    AddInt(QStringLiteral("Frame"), Marker.Frame, 0, 1000000, [Index](SequenceAsset& InOut, const int V) { InOut.Markers[size_t(Index)].Frame = V; });
    AddText(QStringLiteral("Name"), QString::fromStdString(Marker.Name), [Index](SequenceAsset& InOut, const QString& InText)
    {
        InOut.Markers[size_t(Index)].Name = InText.toStdString();
    });
}

#endif // WITH_EDITOR
