/*
 * NimbleEditorSequencerTests -- the Sequencer, offscreen: its document and
 * undo, the timeline's mouse work, the curve editor's tangents, the details'
 * edits, and a tab previewing on a level (auto-key, restore, save).
 */

#include <cmath>
#include <cstdio>
#include <string>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFile>
#include <QMouseEvent>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QUndoStack>

#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Sequencer/SequenceFile.h"
#include "Private/Editor/Sequencer/SequencerCurveEditor.h"
#include "Private/Editor/Sequencer/SequencerDetails.h"
#include "Private/Editor/Sequencer/SequencerDocument.h"
#include "Private/Editor/Sequencer/SequencerTab.h"
#include "Private/Editor/Sequencer/SequencerTimeline.h"

using Sequencer::ESequenceTrackType;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

namespace
{
    bool Near(const double InA, const double InB, const double InTolerance = 1e-3) { return std::fabs(InA - InB) <= InTolerance; }

    void Click(QWidget& InWidget, const QPoint& InAt, const Qt::KeyboardModifiers InModifiers = Qt::NoModifier)
    {
        QMouseEvent Press(QEvent::MouseButtonPress, InAt, InWidget.mapToGlobal(InAt), Qt::LeftButton, Qt::LeftButton, InModifiers);
        QApplication::sendEvent(&InWidget, &Press);
        QMouseEvent Release(QEvent::MouseButtonRelease, InAt, InWidget.mapToGlobal(InAt), Qt::LeftButton, Qt::NoButton, InModifiers);
        QApplication::sendEvent(&InWidget, &Release);
    }

    void DragTo(QWidget& InWidget, const QPoint& InFrom, const QPoint& InTo, const Qt::KeyboardModifiers InModifiers = Qt::NoModifier)
    {
        QMouseEvent Press(QEvent::MouseButtonPress, InFrom, InWidget.mapToGlobal(InFrom), Qt::LeftButton, Qt::LeftButton, InModifiers);
        QApplication::sendEvent(&InWidget, &Press);
        for(int Step = 1; Step <= 4; ++Step)
        {
            const QPoint At = InFrom + (InTo - InFrom) * Step / 4;
            QMouseEvent Move(QEvent::MouseMove, At, InWidget.mapToGlobal(At), Qt::NoButton, Qt::LeftButton, InModifiers);
            QApplication::sendEvent(&InWidget, &Move);
        }
        QMouseEvent Release(QEvent::MouseButtonRelease, InTo, InWidget.mapToGlobal(InTo), Qt::LeftButton, Qt::NoButton, InModifiers);
        QApplication::sendEvent(&InWidget, &Release);
    }

    void DoubleClick(QWidget& InWidget, const QPoint& InAt)
    {
        QMouseEvent Event(QEvent::MouseButtonDblClick, InAt, InWidget.mapToGlobal(InAt), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&InWidget, &Event);
    }

    void Pump()
    {
        for(int Index = 0; Index < 5; ++Index) { QApplication::processEvents(); }
    }

    /* 30 fps, 90 frames: a "hero" binding with a Transform track, an event track. */
    Sequencer::SequenceAsset Sample()
    {
        Sequencer::SequenceAsset Asset;
        Asset.FrameRate = 30;
        Asset.Length    = 90;
        Sequencer::SequenceBinding Hero;
        Hero.Id = "hero";
        Hero.Name = "Hero";
        Sequencer::SequenceTrack Transform;
        Transform.Type = ESequenceTrackType::Transform;
        Transform.Normalize();
        Sequencer::SequenceKey A, B;
        A.Frame = 0;  A.Value = 0.0f;   A.Interp = Sequencer::ESequenceInterp::Linear;
        B.Frame = 60; B.Value = 600.0f;
        Transform.Channels[Sequencer::TC_PositionX].Keys = { A, B };
        Hero.Tracks.push_back(Transform);
        Asset.Bindings.push_back(Hero);
        Sequencer::SequenceTrack Events;
        Events.Type = ESequenceTrackType::Event;
        Asset.Tracks.push_back(Events);
        Asset.Normalize();
        return Asset;
    }

    int FindRow(const SequencerTimeline& InTimeline, const SequencerTimeline::Row::EKind InKind, const int InChannel = -1, const int InBindingTrack = -1)
    {
        const auto& Rows = InTimeline.GetRows();
        for(size_t Index = 0; Index < Rows.size(); ++Index)
        {
            if(Rows[Index].Kind != InKind) { continue; }
            if(InChannel >= 0 && Rows[Index].Channel != InChannel) { continue; }
            if(InBindingTrack != -2 && InBindingTrack != -1 && Rows[Index].Track.Binding != InBindingTrack) { continue; }
            if(InBindingTrack == -2 && Rows[Index].Track.Binding != -1) { continue; }
            return int(Index);
        }
        return -1;
    }
}

static void TestDocument(const QString& InDir)
{
    std::printf("Document\n");
    SequencerDocument Document;
    Document.Reset(Sample());
    Expect(!Document.IsDirty() && Document.GetHistory().count() == 0, "a fresh document is clean");

    const SequencerTrackRef Fov = Document.AddTrack(0, ESequenceTrackType::Float, QStringLiteral("Camera"), QStringLiteral("FovY"));
    Expect(Fov.IsValid() && Document.FindTrack(Fov)->Field == "FovY" && Document.IsDirty(), "a track added, one step");
    Document.SetKey(Fov, 0, 10, 55.0f);
    Document.SetKey(Fov, 0, 10, 50.0f);
    Expect(Document.FindTrack(Fov)->Channels[0].Keys.size() == 1 && Near(Document.FindTrack(Fov)->Channels[0].Keys[0].Value, 50.0),
           "setting a key on a frame with one replaces it");
    Expect(Document.GetSelection().size() == 1, "the key set is selected");
    Document.DeleteSelection();
    Expect(Document.FindTrack(Fov)->Channels[0].Keys.empty(), "deleted");
    Document.GetHistory().undo();
    Expect(Document.FindTrack(Fov)->Channels[0].Keys.size() == 1, "undone");
    Document.GetHistory().redo();
    Expect(Document.FindTrack(Fov)->Channels[0].Keys.empty(), "and redone");

    /* Auto-key: what moved is keyed; a first key off frame 0 also holds its start. */
    GameCore::TransformPose Before, After;
    After.Position = EngineCore::Vector3(0.0, 50.0, 0.0);
    Expect(Document.KeyTransform(0, Before, After, 30), "a move is keyed");
    const auto& Y = Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_PositionY].Keys;
    Expect(Y.size() == 2 && Y[0].Frame == 0 && Near(Y[0].Value, 0.0) && Y[1].Frame == 30 && Near(Y[1].Value, 50.0),
           "Position.Y keyed at 30, held from 0");
    Expect(Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_PositionZ].IsEmpty(), "what did not move is not keyed");
    Expect(!Document.KeyTransform(0, After, After, 40), "nothing moved: nothing keyed");
    Expect(Document.KeyTransform(0, After, After, 40, true), "but K keys the whole pose");
    Expect(!Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_RotationZ].IsEmpty()
           && Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_ScaleX].IsEmpty(), "position and rotation, not a scale of 1");

    const QString Path = InDir + QStringLiteral("/Doc.nmseq");
    QString Error;
    Expect(Document.Save(Path, Error) && !Document.IsDirty(), "saved: " + Error.toStdString());
    SequencerDocument Again;
    Expect(Again.Load(Path, Error) && Again.GetAsset().Bindings[0].Tracks.size() == 2, "loaded back");
}

static void TestTimeline()
{
    std::printf("Timeline\n");
    SequencerDocument Document;
    Document.Reset(Sample());
    SequencerTimeline Timeline(Document);
    Timeline.resize(900, 300);
    Timeline.show();
    Timeline.SetPixelsPerFrame(6.0);
    Pump();

    /* Scrub: the ruler moves the playhead, on whole frames. */
    Click(Timeline, QPoint(int(Timeline.XOfFrame(20.3)), 10));
    Expect(Near(Document.GetPlayhead(), 20.0), "the ruler scrubs to a frame");

    /* The Position.X channel row: its key at 60. */
    const int Channel = FindRow(Timeline, SequencerTimeline::Row::EKind::Channel, Sequencer::TC_PositionX);
    Expect(Channel >= 0, "a keyed channel has a row");
    const int Y = Timeline.RowRect(Channel).center().y();
    Click(Timeline, QPoint(int(Timeline.XOfFrame(60)), Y));
    Expect(Document.GetSelection().size() == 1 && Document.GetSelection()[0].Index == 1, "a click selects a key");

    /* Drag it 10 frames later: one step, still selected. */
    const int Steps = Document.GetHistory().count();
    DragTo(Timeline, QPoint(int(Timeline.XOfFrame(60)), Y), QPoint(int(Timeline.XOfFrame(70)), Y));
    const auto& Keys = Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_PositionX].Keys;
    Expect(Keys.size() == 2 && Keys[1].Frame == 70, "dragged to frame 70: " + std::to_string(Keys.size() > 1 ? Keys[1].Frame : -1));
    Expect(Document.GetHistory().count() == Steps + 1 && Document.GetHistory().undoText() == QStringLiteral("Move"), "as one Move step");
    Expect(Document.GetSelection().size() == 1 && Document.GetSelection()[0].Index == 1, "and still selected");

    /* Alt-drag copies. */
    DragTo(Timeline, QPoint(int(Timeline.XOfFrame(70)), Y), QPoint(int(Timeline.XOfFrame(80)), Y), Qt::AltModifier);
    const auto& Copied = Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_PositionX].Keys;
    Expect(Copied.size() == 3 && Copied[1].Frame == 70 && Copied[2].Frame == 80, "Alt-drag left the original and made a copy");

    /* Box select both keys from 60 to 85. */
    DragTo(Timeline, QPoint(int(Timeline.XOfFrame(65)), Y - 8), QPoint(int(Timeline.XOfFrame(85)), Y + 8));
    Expect(Document.GetSelection().size() == 2, "a box selects what it covers");

    /* Double-click the event lane: an event there. */
    const int EventRow = FindRow(Timeline, SequencerTimeline::Row::EKind::Track, -1, -2);
    Expect(EventRow >= 0, "the sequence's event track has a row");
    DoubleClick(Timeline, QPoint(int(Timeline.XOfFrame(45)), Timeline.RowRect(EventRow).center().y()));
    Expect(Document.GetAsset().Tracks[0].Events.size() == 1 && Document.GetAsset().Tracks[0].Events[0].Frame == 45, "a double-click adds an event");

    /* Collapsing the binding hides its tracks. */
    const size_t Before = Timeline.GetRows().size();
    Document.SetCollapsed(QStringLiteral("b:hero"), true);
    Expect(Timeline.GetRows().size() < Before, "a collapsed binding hides its rows");
}

static void TestCurves()
{
    std::printf("Curve editor\n");
    SequencerDocument Document;
    Document.Reset(Sample());
    SequencerCurveEditor Curves(Document);
    Curves.resize(800, 300);
    Curves.show();
    Document.SetCurrent(0, { 0, 0 });
    Pump();
    Curves.FitView();
    Expect(Curves.GetChannels().size() == 1 && Curves.GetChannels()[0] == Sequencer::TC_PositionX, "the keyed channel is shown");

    /* Drag the key at 60 up: its value changes. */
    const QPoint At = Curves.ToScreen(60, 600).toPoint();
    DragTo(Curves, At, At + QPoint(0, -30));
    const auto& Keys = Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_PositionX].Keys;
    Expect(Keys[1].Value > 600.0f && Keys[1].Frame == 60, "a key dragged up rises");

    /* Its leaving handle, dragged: the key becomes User with that slope. */
    Click(Curves, Curves.ToScreen(0, 0).toPoint());
    const QPointF Key = Curves.ToScreen(0, 0);
    const QPointF Handle = Key + QPointF(46, 0);   // a flat start: the handle lies along +X
    DragTo(Curves, Handle.toPoint(), (Handle + QPointF(0, -40)).toPoint());
    const auto& First = Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_PositionX].Keys[0];
    Expect(First.Interp == Sequencer::ESequenceInterp::User && First.OutTangent > 0.0f, "a tangent handle makes a User key");
}

static void TestDetails()
{
    std::printf("Details\n");
    SequencerDocument Document;
    Document.Reset(Sample());
    SequencerDetails Details(Document);
    Details.show();
    Document.SetSelection({ { SequencerItem::EKind::Key, { 0, 0 }, Sequencer::TC_PositionX, 1 } });
    Pump();
    auto* Frame = Details.findChild<QSpinBox*>(QStringLiteral("Frame"));
    auto* Value = Details.findChild<QDoubleSpinBox*>(QStringLiteral("Value"));
    Expect(Frame && Value && Frame->value() == 60 && Near(Value->value(), 600.0), "a key's frame and value");
    if(Value)
    {
        Value->setValue(250.0);
        emit Value->editingFinished();
    }
    Expect(Near(Document.GetAsset().Bindings[0].Tracks[0].Channels[Sequencer::TC_PositionX].Keys[1].Value, 250.0)
           && Document.GetHistory().undoText() == QStringLiteral("Set Value"), "an edit is one step");

    Document.ClearSelection();
    Document.SetCurrent(-1, SequencerTrackRef());
    Pump();
    Expect(Details.findChild<QSpinBox*>(QStringLiteral("Length (frames)")) != nullptr, "nothing picked: the sequence's own settings");
}

static void TestTab(const QString& InDir)
{
    std::printf("Tab preview\n");
    EditorContext Context;
    EditorDocument& Level = Context.GetLevel();
    GameCore::World& World = Level.GetEditedWorld();
    const GameCore::EntityID Hero = Level.CreateEntity(QStringLiteral("Hero"), EngineCore::Vector3(-50.0, 0.0, 0.0));
    const auto X = [&]() { return World.GetEntity(Hero).GetComponent<GameCore::TransformComponent>()->Position.GetX(); };

    /* A sequence file binding Hero by name. */
    const QString Path = InDir + QStringLiteral("/Tab.nmseq");
    std::string Error;
    Expect(Sequencer::SaveSequenceXml(Path.toStdString(), Sample(), &Error), "the sequence is written");

    {
        SequencerTab Tab(Context);
        QString LoadError;
        Expect(Tab.Load(Path, LoadError), "opened: " + LoadError.toStdString());
        Tab.GetDocument().SetPlayhead(30.0);
        Expect(Near(X(), 300.0), "the preview shows frame 30 on the level: " + std::to_string(X()));

        /* Saving the level: the preview steps aside for the write, and comes back. */
        double SeenWhileSaving = 0.0;
        QObject::connect(&Level, &EditorDocument::AboutToSaveWorld, [&]() { SeenWhileSaving = X(); });
        Level.SaveWorld(InDir + QStringLiteral("/Level.nmworld"));
        Expect(Near(SeenWhileSaving, -50.0) && Near(X(), 300.0), "the saved level holds Hero where it was, not the preview");

        /* Auto-key: a move of Hero keys it at the playhead. */
        Tab.SetAutoKey(true);
        World.GetEntity(Hero).GetComponent<GameCore::TransformComponent>()->Position.SetY(75.0f);
        Level.NotifyPropertyEdited(Hero, GameCore::TransformComponent::StaticType().ID);
        Pump();
        Tab.FlushAutoKey();
        const auto& Tracks = Tab.GetDocument().GetAsset().Bindings[0].Tracks;
        Expect(!Tracks[0].Channels[Sequencer::TC_PositionY].IsEmpty() && Tab.GetDocument().GetHistory().undoText() == QStringLiteral("Auto-key"),
               "auto-key keyed the move");
        Tab.GetDocument().SetPlayhead(45.0);
        Expect(Near(World.GetEntity(Hero).GetComponent<GameCore::TransformComponent>()->Position.GetY(), 75.0, 0.01), "and the preview plays it");

        /* A binding for the selected entity. */
        const GameCore::EntityID Prop = Level.CreateEntity(QStringLiteral("Prop"), EngineCore::Vector3());
        Level.SetSelection(Prop);
        Expect(Tab.AddSelectedEntity() == 1 && Tab.GetDocument().GetAsset().Bindings[1].Name == "Prop", "the selected entity is bound");

        /* SEQUENCER_SNAPSHOT=<png>: the tab as drawn, for a look. */
        if(!qEnvironmentVariableIsEmpty("SEQUENCER_SNAPSHOT"))
        {
            Tab.resize(1500, 640);
            Tab.show();
            Tab.GetDocument().SetCurrent(0, { 0, 0 });
            Tab.GetTimeline().FitAll();
            Tab.GetDocument().SetSelection({ { SequencerItem::EKind::Key, { 0, 0 }, Sequencer::TC_PositionX, 1 } });
            Pump();
            Tab.grab().save(QString::fromLocal8Bit(qgetenv("SEQUENCER_SNAPSHOT")));
        }

        Expect(Tab.Save(), "saved");
        Expect(Tab.ConfirmClose(), "closes clean");
    }
    Expect(Near(X(), -50.0), "closed: the level is as it was: " + std::to_string(X()));
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);
    QTemporaryDir Dir;

    std::printf("NimbleEditorSequencerTests\n");
    TestDocument(Dir.path());
    TestTimeline();
    TestCurves();
    TestDetails();
    TestTab(Dir.path());

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
