#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerDocument.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QFile>
#include <QUndoCommand>
#include <QUndoStack>

#include "Public/Sequencer/SequenceFile.h"

using Sequencer::ESequenceTrackType;
using Sequencer::SequenceAsset;
using Sequencer::SequenceTrack;

namespace
{
    /* Whether the rows the timeline shows differ: bindings, tracks, their names. */
    bool RowsDiffer(const SequenceAsset& InA, const SequenceAsset& InB)
    {
        if(InA.Bindings.size() != InB.Bindings.size() || InA.Tracks.size() != InB.Tracks.size()) { return true; }
        for(size_t Index = 0; Index < InA.Bindings.size(); ++Index)
        {
            const auto& A = InA.Bindings[Index];
            const auto& B = InB.Bindings[Index];
            if(A.Id != B.Id || A.Name != B.Name || A.Tracks.size() != B.Tracks.size()) { return true; }
            for(size_t Track = 0; Track < A.Tracks.size(); ++Track)
            {
                if(A.Tracks[Track].Type != B.Tracks[Track].Type || A.Tracks[Track].Name != B.Tracks[Track].Name
                   || A.Tracks[Track].bMuted != B.Tracks[Track].bMuted || A.Tracks[Track].Field != B.Tracks[Track].Field)
                {
                    return true;
                }
            }
        }
        for(size_t Track = 0; Track < InA.Tracks.size(); ++Track)
        {
            if(InA.Tracks[Track].Type != InB.Tracks[Track].Type || InA.Tracks[Track].Name != InB.Tracks[Track].Name
               || InA.Tracks[Track].bMuted != InB.Tracks[Track].bMuted)
            {
                return true;
            }
        }
        return false;
    }

    bool SameAsset(const SequenceAsset& InA, const SequenceAsset& InB)
    {
        return Sequencer::WriteSequenceBinary(InA) == Sequencer::WriteSequenceBinary(InB);
    }
}

/* One step: the whole asset before and after. Its first redo is the edit
 * itself, already made. */
class SequencerSnapshotCommand final : public QUndoCommand
{
public:
    SequencerSnapshotCommand(SequencerDocument& InDocument, const QString& InText, SequenceAsset InBefore, SequenceAsset InAfter)
        : QUndoCommand(InText), Document(InDocument), Before(std::move(InBefore)), After(std::move(InAfter))
    {
    }

    void undo() override { Document.Restore(Before); }

    void redo() override
    {
        if(bFirst)
        {
            bFirst = false;
            return;
        }
        Document.Restore(After);
    }

private:
    SequencerDocument& Document;
    SequenceAsset      Before;
    SequenceAsset      After;
    bool               bFirst = true;
};

SequencerDocument::SequencerDocument(QObject* InParent)
    : QObject(InParent)
    , History(new QUndoStack(this))
{
}

SequencerDocument::~SequencerDocument() = default;

bool SequencerDocument::Load(const QString& InFilePath, QString& OutError)
{
    QFile File(InFilePath);
    if(!File.open(QIODevice::ReadOnly))
    {
        OutError = QStringLiteral("Cannot read %1").arg(InFilePath);
        return false;
    }
    const QByteArray Bytes = File.readAll();
    SequenceAsset Loaded;
    std::string   Error;
    if(!Sequencer::ReadSequence(std::vector<uint8_t>(Bytes.begin(), Bytes.end()), Loaded, &Error))
    {
        OutError = QString::fromStdString(Error);
        return false;
    }
    Reset(Loaded);
    return true;
}

void SequencerDocument::Reset(const SequenceAsset& InAsset)
{
    Asset = InAsset;
    Asset.Normalize();
    Snapshot.reset();
    Selection.clear();
    CurrentTrack   = SequencerTrackRef();
    CurrentBinding = -1;
    Playhead       = 0.0;
    History->clear();
    emit RowsChanged();
    emit Changed();
    emit SelectionChanged();
    emit PlayheadChanged(Playhead);
}

bool SequencerDocument::Save(const QString& InFilePath, QString& OutError)
{
    std::string Error;
    if(!Sequencer::SaveSequenceXml(InFilePath.toStdString(), Asset, &Error))
    {
        OutError = QString::fromStdString(Error);
        return false;
    }
    History->setClean();
    return true;
}

std::shared_ptr<const SequenceAsset> SequencerDocument::GetSnapshot()
{
    if(!Snapshot) { Snapshot = std::make_shared<SequenceAsset>(Asset); }
    return Snapshot;
}

const SequenceTrack* SequencerDocument::FindTrack(const SequencerTrackRef& InRef) const
{
    if(InRef.Track < 0) { return nullptr; }
    const std::vector<SequenceTrack>* Tracks = &Asset.Tracks;
    if(InRef.Binding >= 0)
    {
        if(size_t(InRef.Binding) >= Asset.Bindings.size()) { return nullptr; }
        Tracks = &Asset.Bindings[size_t(InRef.Binding)].Tracks;
    }
    return size_t(InRef.Track) < Tracks->size() ? &(*Tracks)[size_t(InRef.Track)] : nullptr;
}

SequenceTrack* SequencerDocument::FindLiveTrack(const SequencerTrackRef& InRef)
{
    return const_cast<SequenceTrack*>(FindTrack(InRef));
}

// === Editing =====================================================================

void SequencerDocument::Record(const QString& InText, SequenceAsset InBefore)
{
    Snapshot.reset();
    const bool bRows = RowsDiffer(InBefore, Asset);
    History->push(new SequencerSnapshotCommand(*this, InText, std::move(InBefore), Asset));
    PruneSelection();
    if(bRows) { emit RowsChanged(); }
    emit Changed();
}

void SequencerDocument::Edit(const QString& InText, const std::function<void(SequenceAsset&)>& InChange)
{
    SequenceAsset Before = Asset;
    InChange(Asset);
    Asset.Normalize();
    if(SameAsset(Before, Asset))
    {
        return;
    }
    Record(InText, std::move(Before));
}

void SequencerDocument::BeginDrag()
{
    DragBefore = Asset;
    bDragging  = true;
}

void SequencerDocument::PreviewDrag()
{
    Snapshot.reset();
    emit LiveChanged();
}

void SequencerDocument::EndDrag(const QString& InText, const bool bInCancel)
{
    if(!bDragging) { return; }
    bDragging = false;
    if(bInCancel)
    {
        Asset = DragBefore;
        Snapshot.reset();
        emit Changed();
        return;
    }
    Asset.Normalize();
    if(SameAsset(DragBefore, Asset))
    {
        Snapshot.reset();
        emit Changed();
        return;
    }
    Record(InText, std::move(DragBefore));
}

void SequencerDocument::Restore(const SequenceAsset& InAsset)
{
    const bool bRows = RowsDiffer(Asset, InAsset);
    Asset = InAsset;
    Snapshot.reset();
    PruneSelection();
    if(bRows) { emit RowsChanged(); }
    emit Changed();
}

// === Common edits ================================================================

QString SequencerDocument::UniqueBindingId(const QString& InBase) const
{
    QString Base;
    for(const QChar Ch : InBase)
    {
        if(Ch.isLetterOrNumber() || Ch == QLatin1Char('_')) { Base += Ch.toLower(); }
    }
    if(Base.isEmpty()) { Base = QStringLiteral("binding"); }
    QString Id = Base;
    for(int Suffix = 2; Asset.FindBinding(Id.toStdString()) >= 0; ++Suffix) { Id = Base + QString::number(Suffix); }
    return Id;
}

int SequencerDocument::AddPossessBinding(const QString& InName, const GameCore::EntityGuid& InGuid)
{
    const QString Id = UniqueBindingId(InName);
    Edit(QStringLiteral("Add Binding"), [&](SequenceAsset& InOut)
    {
        Sequencer::SequenceBinding Binding;
        Binding.Id   = Id.toStdString();
        Binding.Name = InName.toStdString();
        Binding.Kind = Sequencer::ESequenceBindingKind::Possess;
        Binding.Guid = InGuid;
        InOut.Bindings.push_back(std::move(Binding));
    });
    return Asset.FindBinding(Id.toStdString());
}

int SequencerDocument::AddSpawnBinding(const QString& InName, const QString& InPrefab)
{
    const QString Id = UniqueBindingId(InName);
    Edit(QStringLiteral("Add Spawned Binding"), [&](SequenceAsset& InOut)
    {
        Sequencer::SequenceBinding Binding;
        Binding.Id     = Id.toStdString();
        Binding.Name   = InName.toStdString();
        Binding.Kind   = Sequencer::ESequenceBindingKind::Spawn;
        Binding.Prefab = InPrefab.toStdString();
        InOut.Bindings.push_back(std::move(Binding));
    });
    return Asset.FindBinding(Id.toStdString());
}

void SequencerDocument::RemoveBinding(const int InBinding)
{
    if(InBinding < 0 || size_t(InBinding) >= Asset.Bindings.size()) { return; }
    Edit(QStringLiteral("Remove Binding"), [&](SequenceAsset& InOut)
    {
        InOut.Bindings.erase(InOut.Bindings.begin() + InBinding);
    });
}

SequencerTrackRef SequencerDocument::AddTrack(const int InBinding, const ESequenceTrackType InType, const QString& InComponent,
                                              const QString& InField)
{
    if(InBinding >= int(Asset.Bindings.size())) { return SequencerTrackRef(); }
    SequencerTrackRef Made;
    Edit(QStringLiteral("Add %1 Track").arg(QString::fromLatin1(Sequencer::TrackTypeName(InType))), [&](SequenceAsset& InOut)
    {
        SequenceTrack Track;
        Track.Type      = InType;
        Track.Component = InComponent.toStdString();
        Track.Field     = InField.toStdString();
        Track.Normalize();
        std::vector<SequenceTrack>& Tracks = InBinding >= 0 ? InOut.Bindings[size_t(InBinding)].Tracks : InOut.Tracks;
        Tracks.push_back(std::move(Track));
        Made.Binding = InBinding;
        Made.Track   = int(Tracks.size()) - 1;
    });
    if(Made.IsValid()) { SetCurrent(InBinding, Made); }
    return Made;
}

void SequencerDocument::RemoveTrack(const SequencerTrackRef& InTrack)
{
    if(!FindTrack(InTrack)) { return; }
    Edit(QStringLiteral("Remove Track"), [&](SequenceAsset& InOut)
    {
        std::vector<SequenceTrack>& Tracks = InTrack.Binding >= 0 ? InOut.Bindings[size_t(InTrack.Binding)].Tracks : InOut.Tracks;
        Tracks.erase(Tracks.begin() + InTrack.Track);
    });
}

void SequencerDocument::SetKey(const SequencerTrackRef& InTrack, const int InChannel, const int InFrame, const float InValue, const QString& InText)
{
    const SequenceTrack* Track = FindTrack(InTrack);
    if(!Track || InChannel < 0 || size_t(InChannel) >= Track->Channels.size()) { return; }
    Edit(InText, [&](SequenceAsset& InOut)
    {
        SequenceTrack& Live = *FindLiveTrack(InTrack);
        (void)InOut;
        std::vector<Sequencer::SequenceKey>& Keys = Live.Channels[size_t(InChannel)].Keys;
        for(Sequencer::SequenceKey& Key : Keys)
        {
            if(Key.Frame == InFrame)
            {
                Key.Value = InValue;
                return;
            }
        }
        Sequencer::SequenceKey Key;
        Key.Frame = InFrame;
        Key.Value = InValue;
        if(Live.Type == ESequenceTrackType::Bool) { Key.Interp = Sequencer::ESequenceInterp::Constant; }
        Keys.push_back(Key);
    });
    /* The key just set, selected. */
    if(const SequenceTrack* After = FindTrack(InTrack))
    {
        const auto& Keys = After->Channels[size_t(InChannel)].Keys;
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            if(Keys[Index].Frame == InFrame) { SetSelection({ { SequencerItem::EKind::Key, InTrack, InChannel, int(Index) } }); }
        }
    }
}

void SequencerDocument::AddSection(const SequencerTrackRef& InTrack, const int InStart, const int InEnd, const QString& InAsset)
{
    if(!FindTrack(InTrack)) { return; }
    Edit(QStringLiteral("Add Section"), [&](SequenceAsset&)
    {
        Sequencer::SequenceSection Section;
        Section.Start = InStart;
        Section.End   = std::max(InEnd, InStart + 1);
        Section.Asset = InAsset.toStdString();
        FindLiveTrack(InTrack)->Sections.push_back(std::move(Section));
    });
    if(const SequenceTrack* After = FindTrack(InTrack))
    {
        for(size_t Index = 0; Index < After->Sections.size(); ++Index)
        {
            if(After->Sections[Index].Start == InStart) { SetSelection({ { SequencerItem::EKind::Section, InTrack, 0, int(Index) } }); }
        }
    }
}

void SequencerDocument::AddEvent(const SequencerTrackRef& InTrack, const int InFrame, const QString& InName)
{
    if(!FindTrack(InTrack)) { return; }
    Edit(QStringLiteral("Add Event"), [&](SequenceAsset&)
    {
        Sequencer::SequenceEventKey Key;
        Key.Frame = InFrame;
        Key.Event = InName.toStdString();
        FindLiveTrack(InTrack)->Events.push_back(std::move(Key));
    });
    if(const SequenceTrack* After = FindTrack(InTrack))
    {
        for(size_t Index = 0; Index < After->Events.size(); ++Index)
        {
            if(After->Events[Index].Frame == InFrame) { SetSelection({ { SequencerItem::EKind::Event, InTrack, 0, int(Index) } }); }
        }
    }
}

void SequencerDocument::AddMarker(const int InFrame, const QString& InName)
{
    Edit(QStringLiteral("Add Marker"), [&](SequenceAsset& InOut)
    {
        SequenceAsset::Marker Marker;
        Marker.Frame = InFrame;
        Marker.Name  = InName.toStdString();
        InOut.Markers.push_back(std::move(Marker));
    });
}

void SequencerDocument::DeleteSelection()
{
    if(Selection.empty()) { return; }
    std::vector<SequencerItem> Doomed = Selection;
    /* Highest index first, so earlier ones keep their place. */
    std::sort(Doomed.begin(), Doomed.end(), [](const SequencerItem& L, const SequencerItem& R) { return L.Index > R.Index; });
    Edit(Doomed.size() == 1 ? QStringLiteral("Delete") : QStringLiteral("Delete %1 Items").arg(Doomed.size()), [&](SequenceAsset& InOut)
    {
        for(const SequencerItem& Item : Doomed)
        {
            if(Item.Kind == SequencerItem::EKind::Marker)
            {
                if(Item.Index >= 0 && size_t(Item.Index) < InOut.Markers.size()) { InOut.Markers.erase(InOut.Markers.begin() + Item.Index); }
                continue;
            }
            SequenceTrack* Track = FindLiveTrack(Item.Track);
            if(!Track || Item.Index < 0) { continue; }
            switch(Item.Kind)
            {
                case SequencerItem::EKind::Key:
                    if(size_t(Item.Channel) < Track->Channels.size() && size_t(Item.Index) < Track->Channels[size_t(Item.Channel)].Keys.size())
                    {
                        auto& Keys = Track->Channels[size_t(Item.Channel)].Keys;
                        Keys.erase(Keys.begin() + Item.Index);
                    }
                    break;
                case SequencerItem::EKind::Section:
                    if(size_t(Item.Index) < Track->Sections.size()) { Track->Sections.erase(Track->Sections.begin() + Item.Index); }
                    break;
                case SequencerItem::EKind::Event:
                    if(size_t(Item.Index) < Track->Events.size()) { Track->Events.erase(Track->Events.begin() + Item.Index); }
                    break;
                case SequencerItem::EKind::Marker:
                    break;
            }
        }
    });
    SetSelection({});
}

float SequencerDocument::EvaluateChannel(const SequencerTrackRef& InTrack, const int InChannel, const double InFrame, const float InDefault) const
{
    const SequenceTrack* Track = FindTrack(InTrack);
    if(!Track || InChannel < 0 || size_t(InChannel) >= Track->Channels.size()) { return InDefault; }
    return Track->Channels[size_t(InChannel)].Evaluate(InFrame, InDefault);
}

bool SequencerDocument::KeyTransform(const int InBinding, const GameCore::TransformPose& InBefore, const GameCore::TransformPose& InAfter,
                                     const int InFrame, const bool bInAll)
{
    if(InBinding < 0 || size_t(InBinding) >= Asset.Bindings.size()) { return false; }

    const auto Flatten = [](const GameCore::TransformPose& InPose, float (&OutValues)[Sequencer::TC_Count])
    {
        const EngineCore::Vector3* Parts[3] = { &InPose.Position, &InPose.Rotation, &InPose.Scale };
        for(size_t Channel = 0; Channel < Sequencer::TC_Count; ++Channel)
        {
            const EngineCore::Vector3& Part = *Parts[Channel / 3];
            const size_t Axis = Channel % 3;
            OutValues[Channel] = float(Axis == 0 ? Part.GetX() : Axis == 1 ? Part.GetY() : Part.GetZ());
        }
    };
    float Before[Sequencer::TC_Count], After[Sequencer::TC_Count];
    Flatten(InBefore, Before);
    Flatten(InAfter, After);

    bool bChanged[Sequencer::TC_Count] = {};
    bool bAny = false;
    const SequenceTrack* Existing = nullptr;
    for(const SequenceTrack& Track : Asset.Bindings[size_t(InBinding)].Tracks)
    {
        if(Track.Type == ESequenceTrackType::Transform) { Existing = &Track; break; }
    }
    for(size_t Channel = 0; Channel < Sequencer::TC_Count; ++Channel)
    {
        bChanged[Channel] = std::fabs(After[Channel] - Before[Channel]) > 1e-4f;
        if(bInAll)
        {
            /* Position and rotation always; scale where it is keyed or not 1. */
            const bool bScale = Channel >= Sequencer::TC_ScaleX;
            bChanged[Channel] = !bScale || std::fabs(After[Channel] - 1.0f) > 1e-4f || (Existing && !Existing->Channels[Channel].IsEmpty());
        }
        bAny = bAny || bChanged[Channel];
    }
    if(!bAny) { return false; }

    Edit(QStringLiteral("Auto-key"), [&](SequenceAsset& InOut)
    {
        auto& Tracks = InOut.Bindings[size_t(InBinding)].Tracks;
        auto Found = std::find_if(Tracks.begin(), Tracks.end(), [](const SequenceTrack& InTrack) { return InTrack.Type == ESequenceTrackType::Transform; });
        if(Found == Tracks.end())
        {
            SequenceTrack Track;
            Track.Type = ESequenceTrackType::Transform;
            Track.Normalize();
            Found = Tracks.insert(Tracks.begin(), std::move(Track));
        }
        for(size_t Channel = 0; Channel < Sequencer::TC_Count; ++Channel)
        {
            if(!bChanged[Channel]) { continue; }
            auto& Keys = Found->Channels[Channel].Keys;
            /* A channel keyed for the first time away from frame 0 holds
             * where it was until then. */
            if(Keys.empty() && InFrame > 0)
            {
                Sequencer::SequenceKey Start;
                Start.Frame = 0;
                Start.Value = Before[Channel];
                Keys.push_back(Start);
            }
            bool bSet = false;
            for(Sequencer::SequenceKey& Key : Keys)
            {
                if(Key.Frame == InFrame) { Key.Value = After[Channel]; bSet = true; }
            }
            if(!bSet)
            {
                Sequencer::SequenceKey Key;
                Key.Frame = InFrame;
                Key.Value = After[Channel];
                Keys.push_back(Key);
            }
        }
    });
    return true;
}

// === Selection and playhead ========================================================

void SequencerDocument::SetSelection(const std::vector<SequencerItem>& InItems)
{
    if(InItems.size() == Selection.size() && std::equal(InItems.begin(), InItems.end(), Selection.begin()))
    {
        return;
    }
    Selection = InItems;
    if(!Selection.empty() && Selection.front().Kind != SequencerItem::EKind::Marker)
    {
        CurrentTrack   = Selection.front().Track;
        CurrentBinding = CurrentTrack.Binding;
    }
    emit SelectionChanged();
}

bool SequencerDocument::IsSelected(const SequencerItem& InItem) const
{
    return std::find(Selection.begin(), Selection.end(), InItem) != Selection.end();
}

void SequencerDocument::SetCurrent(const int InBinding, const SequencerTrackRef& InTrack)
{
    if(CurrentBinding == InBinding && CurrentTrack == InTrack) { return; }
    CurrentBinding = InBinding;
    CurrentTrack   = InTrack;
    emit SelectionChanged();
}

void SequencerDocument::SetPlayhead(double InFrame)
{
    InFrame = std::clamp(InFrame, 0.0, double(std::max(Asset.Length, 0)));
    if(InFrame == Playhead) { return; }
    Playhead = InFrame;
    emit PlayheadChanged(Playhead);
}

bool SequencerDocument::IsCollapsed(const QString& InKey) const
{
    return std::find(Collapsed.begin(), Collapsed.end(), InKey) != Collapsed.end();
}

void SequencerDocument::SetCollapsed(const QString& InKey, const bool bInCollapsed)
{
    const auto Found = std::find(Collapsed.begin(), Collapsed.end(), InKey);
    if(bInCollapsed == (Found != Collapsed.end())) { return; }
    if(bInCollapsed) { Collapsed.push_back(InKey); }
    else             { Collapsed.erase(Found); }
    emit RowsChanged();
}

void SequencerDocument::PruneSelection()
{
    std::vector<SequencerItem> Kept;
    for(const SequencerItem& Item : Selection)
    {
        if(Item.Kind == SequencerItem::EKind::Marker)
        {
            if(Item.Index >= 0 && size_t(Item.Index) < Asset.Markers.size()) { Kept.push_back(Item); }
            continue;
        }
        const SequenceTrack* Track = FindTrack(Item.Track);
        if(!Track || Item.Index < 0) { continue; }
        const bool bThere =
            (Item.Kind == SequencerItem::EKind::Key && size_t(Item.Channel) < Track->Channels.size()
             && size_t(Item.Index) < Track->Channels[size_t(Item.Channel)].Keys.size())
            || (Item.Kind == SequencerItem::EKind::Section && size_t(Item.Index) < Track->Sections.size())
            || (Item.Kind == SequencerItem::EKind::Event && size_t(Item.Index) < Track->Events.size());
        if(bThere) { Kept.push_back(Item); }
    }
    const bool bSelectionChanged = Kept.size() != Selection.size();
    Selection = std::move(Kept);
    if(CurrentTrack.IsValid() && !FindTrack(CurrentTrack)) { CurrentTrack = SequencerTrackRef(); }
    if(CurrentBinding >= int(Asset.Bindings.size()))       { CurrentBinding = -1; }
    if(bSelectionChanged) { emit SelectionChanged(); }
    if(Playhead > double(Asset.Length)) { SetPlayhead(double(Asset.Length)); }
}

bool SequencerDocument::IsDirty() const
{
    return !History->isClean();
}

#endif // WITH_EDITOR
