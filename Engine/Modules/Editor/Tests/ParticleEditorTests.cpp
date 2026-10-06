/*
 * NimbleEditorParticleTests: the Particle Editor's document and panels,
 * offscreen -- no Renderer, no window shown. Edits and their undo, the
 * emitter list, saving and loading, and the panels following the document.
 */

#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QComboBox>
#include <QMouseEvent>
#include <QFile>
#include <QAction>
#include <QMenu>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Private/Editor/Particles/ParticleCurveWidgets.h"
#include "Private/Editor/Particles/ParticleDocument.h"
#include "Private/Editor/Particles/ParticleEmitterPanels.h"

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

namespace
{
    const char* const SYSTEM =
        "NimbleParticleSystem 1\n"
        "\n"
        "Emitter Smoke\n"
        "    SpawnRate 70\n"
        "    Lifetime 6\n"
        "\n"
        "Emitter Sparks\n"
        "    SpawnRate 200\n";

    QString Write(const QString& InDir, const char* InName, const char* InText)
    {
        const QString Path = InDir + QStringLiteral("/") + QString::fromLatin1(InName);
        QFile File(Path);
        File.open(QIODevice::WriteOnly | QIODevice::Truncate);
        File.write(InText);
        return Path;
    }

    QString NameOf(ParticleDocument& InDocument, const int InIndex)
    {
        const RenderCore::ParticleEmitterDesc* Emitter = InDocument.GetEmitter(InIndex);
        return Emitter ? QString::fromUtf8(Emitter->Name) : QString();
    }

    void TestDocument(const QString& InDir)
    {
        std::printf("[document]\n");
        ParticleDocument Document;
        QString Error;
        Expect(!Document.Load(Write(InDir, "Broken.nmparticle", "Emitter A\n"), Error) && !Error.isEmpty(),
               "a file that is not a particle system is refused, saying why");

        const QString Path = Write(InDir, "Flare.nmparticle", SYSTEM);
        Expect(Document.Load(Path, Error) && Document.GetEmitterCount() == 2 && Document.GetSelected() == 0 && !Document.IsDirty(),
               "a system loads clean, its first emitter selected");

        /* A property widget writes the bytes; the document records it after. */
        Document.GetEmitter(0)->SpawnRate = 10.0f;
        Document.CommitLiveEdit(QStringLiteral("Edit Spawn Rate"));
        Expect(Document.IsDirty() && Document.GetHistory().undoText() == QStringLiteral("Edit Spawn Rate"), "a live edit is one undo step");
        Document.GetHistory().undo();
        Expect(Document.GetEmitter(0)->SpawnRate == 70.0f && !Document.IsDirty(), "undone, the value is back and the system clean");
        Document.GetHistory().redo();
        Expect(Document.GetEmitter(0)->SpawnRate == 10.0f, "and redone");

        Expect(Document.AddEmitter() && Document.GetEmitterCount() == 3 && Document.GetSelected() == 2
               && NameOf(Document, 2) == QStringLiteral("Emitter"), "Add makes a default emitter at the end and selects it");
        Expect(Document.DuplicateEmitter(0) && NameOf(Document, 1) == QStringLiteral("Smoke 2") && Document.GetSelected() == 1
               && Document.GetEmitter(1)->SpawnRate == 10.0f && Document.GetEmitter(1)->Seed != Document.GetEmitter(0)->Seed,
               "Duplicate copies after the original under a free name, seeded apart, and selects the copy");
        Document.MoveEmitter(3, 1);
        Expect(Document.GetEmitterCount() == 4 && NameOf(Document, 3) == QStringLiteral("Emitter") && NameOf(Document, 1) == QStringLiteral("Smoke 2"), "a move off the end does nothing");
        Document.MoveEmitter(1, 1);
        Expect(NameOf(Document, 2) == QStringLiteral("Smoke 2") && NameOf(Document, 1) == QStringLiteral("Sparks") && Document.GetSelected() == 2,
               "Move swaps neighbours and follows the moved one");
        Document.RenameEmitter(2, QStringLiteral("  Plume  "));
        Expect(NameOf(Document, 2) == QStringLiteral("Plume"), "Rename trims");
        Document.RenameEmitter(2, QStringLiteral("   "));
        Expect(NameOf(Document, 2) == QStringLiteral("Plume"), "an empty name is refused");
        Document.SetEmitterEnabled(1, false);
        Expect(Document.GetEmitter(1)->bEnabled == 0, "an emitter is switched off");
        Document.RemoveEmitter(3);
        Expect(Document.GetEmitterCount() == 3 && Document.GetSelected() == 2, "Remove selects the one now last");

        const int Steps = Document.GetHistory().count();
        for(int Each = 0; Each < Steps; ++Each) { Document.GetHistory().undo(); }
        Expect(Document.GetEmitterCount() == 2 && NameOf(Document, 1) == QStringLiteral("Sparks") && Document.GetEmitter(0)->SpawnRate == 70.0f
               && !Document.IsDirty(), "every step undone: the system as loaded");
        for(int Each = 0; Each < Steps; ++Each) { Document.GetHistory().redo(); }
        Expect(Document.GetEmitterCount() == 3 && NameOf(Document, 2) == QStringLiteral("Plume") && Document.GetSelected() == 2,
               "and all redone, the selection where the last step left it");

        while(Document.AddEmitter()) {}
        Expect(Document.GetEmitterCount() == int(RenderCore::PARTICLE_SYSTEM_EMITTERS_MAX) && !Document.DuplicateEmitter(0),
               "at most eight emitters");

        Expect(Document.Save(Path, Error) && !Document.IsDirty(), "Save writes the file and marks the system clean");
        ParticleDocument Reloaded;
        Expect(Reloaded.Load(Path, Error) && Reloaded.GetEmitterCount() == Document.GetEmitterCount()
               && QString::fromUtf8(Reloaded.GetEmitter(2)->Name) == QStringLiteral("Plume") && Reloaded.GetEmitter(1)->bEnabled == 0,
               "and reads back the same");
    }

    void TestPanels(const QString& InDir)
    {
        std::printf("[panels]\n");
        ParticleDocument Document;
        QString Error;
        Document.Load(Write(InDir, "Panels.nmparticle", SYSTEM), Error);

        ParticleEmitterListPanel    List(Document);
        ParticleEmitterDetailsPanel Details(Document);
        auto* Rows = List.findChild<QListWidget*>();
        Expect(Rows && Rows->count() == 2 && Rows->item(1)->text() == QStringLiteral("Sparks") && Rows->currentRow() == 0,
               "the list shows each emitter, the selected one current");

        Rows->setCurrentRow(1);
        Expect(Document.GetSelected() == 1, "picking a row selects its emitter");
        Rows->item(1)->setCheckState(Qt::Unchecked);
        Expect(Document.GetEmitter(1)->bEnabled == 0, "its check box switches it off, as an undo step");
        Document.GetHistory().undo();
        Expect(Rows->item(1)->checkState() == Qt::Checked, "and undo puts the box back");

        Document.AddEmitter();
        Expect(Rows->count() == 3 && Rows->currentRow() == 2, "an added emitter appears, current");

        /* Sections are drawn as components are: a header toggle each. */
        const auto SectionNames = [&Details]()
        {
            QStringList Out;
            for(QToolButton* Button : Details.findChildren<QToolButton*>())
            {
                if(Button->isCheckable() && Button->arrowType() != Qt::NoArrow) { Out << Button->text(); }
            }
            return Out;
        };
        Expect(SectionNames() == QStringList({ "Rendering", "Emission", "Velocity", "Size", "Color" }),
               "a fresh emitter's sections: Rendering, Emission, its Velocity module, Size, Color");

        /* The default emitter shows no reset arrows; a changed setting one, which resets it. */
        const auto VisibleResets = [&Details]()
        {
            QList<QToolButton*> Out;
            for(QToolButton* Button : Details.findChildren<QToolButton*>())
            {
                if(Button->text() == QString(QChar(0x21BA)) && Button->isVisibleTo(&Details)) { Out << Button; }
            }
            return Out;
        };
        Expect(VisibleResets().isEmpty(), "an emitter at its defaults shows no reset arrows");
        Document.GetEmitter(2)->VelocitySpread = 15.0f;
        Document.CommitLiveEdit(QStringLiteral("Edit Velocity Spread"));
        const QList<QToolButton*> Resets = VisibleResets();
        Expect(Resets.size() == 1, "a changed setting shows its reset arrow");
        if(Resets.size() == 1)
        {
            Resets.front()->click();
            Expect(Document.GetEmitter(2)->VelocitySpread == 0.0f && Document.GetHistory().undoText() == QStringLiteral("Reset Velocity Spread")
                   && VisibleResets().isEmpty(), "which resets it, as an undo step");
        }

        /* Only the settings its type and spawn mode use. */
        const auto Shows = [&Details](const char* InLabel)
        {
            for(QLabel* Label : Details.findChildren<QLabel*>())
            {
                if(Label->text() == QString::fromLatin1(InLabel) && Label->isVisibleTo(&Details)) { return true; }
            }
            return false;
        };
        Expect(Shows("Billboard") && Shows("Spawn Rate") && !Shows("Beam End") && !Shows("Mesh Orientation") && !Shows("Burst Count"),
               "a continuous Sprite shows its billboard and spawn rate, no beam, mesh or burst settings");
        Document.Edit(QStringLiteral("Edit Type"), [](RenderCore::ParticleSystemDesc& InOut) { InOut.Emitters[2].Type = RenderCore::PT_Beam; });
        QApplication::processEvents();
        Expect(Shows("Beam End") && Shows("Beam Noise") && !Shows("Spawn Rate") && !Shows("Velocity") && !Shows("Billboard") && !Shows("Mesh"),
               "made a Beam, it shows the beam's settings and none of spawning, motion or billboards");
        Document.Edit(QStringLiteral("Edit Type"), [](RenderCore::ParticleSystemDesc& InOut)
        {
            InOut.Emitters[2].Type      = RenderCore::PT_Mesh;
            InOut.Emitters[2].SpawnMode = RenderCore::PSM_Burst;
        });
        QApplication::processEvents();
        Expect(Shows("Mesh Orientation") && Shows("Burst Count") && Shows("Burst Interval") && !Shows("Spawn Rate") && !Shows("Billboard"),
               "a bursting Mesh shows its orientation and burst settings");
        Document.GetHistory().undo();
        QApplication::processEvents();
        Expect(Shows("Beam End") && !Shows("Burst Count"), "undo puts the beam's settings back");

        /* Modules: added from "+ Add Module", removed with their x, each one undo step. */
        Document.Edit(QStringLiteral("Edit Type"), [](RenderCore::ParticleSystemDesc& InOut)
        {
            InOut.Emitters[2].Type      = RenderCore::PT_Sprite;
            InOut.Emitters[2].SpawnMode = RenderCore::PSM_Continuous;
        });
        QApplication::processEvents();
        const auto AddModule = [&Details](const QString& InName)
        {
            for(QToolButton* Button : Details.findChildren<QToolButton*>())
            {
                if(Button->text() != QStringLiteral("+ Add Module") || !Button->menu()) { continue; }
                emit Button->menu()->aboutToShow();
                for(QAction* Action : Button->menu()->actions())
                {
                    if(Action->text() == InName) { Action->trigger(); return true; }
                }
            }
            return false;
        };
        const auto Offered = [&Details]()
        {
            QStringList Out;
            for(QToolButton* Button : Details.findChildren<QToolButton*>())
            {
                if(Button->text() != QStringLiteral("+ Add Module") || !Button->menu()) { continue; }
                emit Button->menu()->aboutToShow();
                for(QAction* Action : Button->menu()->actions()) { if(Action->isEnabled()) { Out << Action->text(); } }
            }
            return Out;
        };
        Expect(Offered() == QStringList({ "Shape", "Forces", "Rotation", "Collision", "Flipbook" }),
               "a sprite with Velocity is offered Shape, Forces, Rotation, Collision and Flipbook");
        Expect(!Shows("Shape") && !Shows("Radius") && AddModule(QStringLiteral("Shape")), "Shape is not there until added from + Add Module");
        QApplication::processEvents();
        Expect(Document.GetEmitter(2)->HasModule(RenderCore::PEM_Shape) && Document.GetHistory().undoText() == QStringLiteral("Add Shape")
               && Shows("Radius") && Shows("On Surface") && Shows("Outward Speed") && !Shows("Box Size") && !Shows("Cone Angle"),
               "added, a Sphere shows its radius, surface and outward speed, as one undo step");
        Document.Edit(QStringLiteral("Edit Shape"), [](RenderCore::ParticleSystemDesc& InOut) { InOut.Emitters[2].Shape = RenderCore::PS_Box; });
        QApplication::processEvents();
        Expect(Shows("Box Size") && !Shows("Radius"), "a Box its size instead of a radius");
        Document.Edit(QStringLiteral("Edit Shape"), [](RenderCore::ParticleSystemDesc& InOut) { InOut.Emitters[2].Shape = RenderCore::PS_Cone; });
        QApplication::processEvents();
        Expect(Shows("Cone Angle") && Shows("Radius") && !Shows("Ring Width"), "a Cone its angle");

        QToolButton* RemoveShape = nullptr;
        for(QToolButton* Button : Details.findChildren<QToolButton*>())
        {
            if(Button->text() == QString(QChar(0x2715)) && Button->toolTip().contains(QStringLiteral("Remove Shape"))) { RemoveShape = Button; }
        }
        Expect(RemoveShape != nullptr, "an optional module has a remove button");
        bool bRequiredRemovable = false;
        for(QToolButton* Button : Details.findChildren<QToolButton*>())
        {
            bRequiredRemovable = bRequiredRemovable || Button->toolTip().contains(QStringLiteral("Remove Emission"))
                              || Button->toolTip().contains(QStringLiteral("Remove Color"));
        }
        Expect(!bRequiredRemovable, "and the sections every emitter needs have none");
        if(RemoveShape) { RemoveShape->click(); }
        QApplication::processEvents();
        Expect(!Document.GetEmitter(2)->HasModule(RenderCore::PEM_Shape) && Document.GetEmitter(2)->Shape == RenderCore::PS_Sphere
               && !Shows("Radius") && !SectionNames().contains(QStringLiteral("Shape")),
               "removed, its section goes and its settings go back to their defaults");
        Document.GetHistory().undo();
        QApplication::processEvents();
        Expect(Document.GetEmitter(2)->HasModule(RenderCore::PEM_Shape) && Document.GetEmitter(2)->Shape == RenderCore::PS_Cone && Shows("Cone Angle"),
               "and undo puts it back as it was");

        Document.Edit(QStringLiteral("Edit Type"), [](RenderCore::ParticleSystemDesc& InOut) { InOut.Emitters[2].Type = RenderCore::PT_Ribbon; });
        QApplication::processEvents();
        Expect(!Offered().contains(QStringLiteral("Flipbook")) && Offered().contains(QStringLiteral("Collision")),
               "a ribbon is offered Collision, not Flipbook");
        Document.GetHistory().undo();
        QApplication::processEvents();
        Expect(AddModule(QStringLiteral("Collision")), "Collision is added from the menu");
        QApplication::processEvents();
        Expect(Shows("Response") && Shows("Height") && Shows("Bounce") && Shows("Friction"), "and shows its plane and bounce");
        Document.GetHistory().undo();
        QApplication::processEvents();

        /* A section's Reset: every setting in it, one step. */
        Document.GetEmitter(2)->VelocitySpread = 40.0f;
        Document.CommitLiveEdit(QStringLiteral("Edit Velocity Spread"));
        QToolButton* ResetVelocity = nullptr;
        for(QToolButton* Button : Details.findChildren<QToolButton*>())
        {
            if(Button->text() == QStringLiteral("Reset") && Button->toolTip().contains(QStringLiteral("of Velocity"))) { ResetVelocity = Button; }
        }
        Expect(ResetVelocity && ResetVelocity->isEnabled(), "a changed section's Reset is enabled");
        if(ResetVelocity) { ResetVelocity->click(); }
        QApplication::processEvents();
        Expect(Document.GetEmitter(2)->VelocitySpread == 0.0f && Document.GetHistory().undoText() == QStringLiteral("Reset Velocity"),
               "and resets the section");

        /* Collapsed, a section keeps its header; it stays collapsed through a rebuild. */
        for(QToolButton* Button : Details.findChildren<QToolButton*>())
        {
            if(Button->isCheckable() && Button->text() == QStringLiteral("Emission")) { Button->setChecked(false); }
        }
        const bool bHidden = !Shows("Spawn Rate") && SectionNames().contains(QStringLiteral("Emission"));
        Document.Edit(QStringLiteral("Edit Shape"), [](RenderCore::ParticleSystemDesc& InOut) { InOut.Emitters[2].Shape = RenderCore::PS_Ring; });
        QApplication::processEvents();
        Expect(bHidden && !Shows("Spawn Rate") && Shows("Ring Width"), "a collapsed section hides its rows, and stays collapsed when rebuilt");
        for(QToolButton* Button : Details.findChildren<QToolButton*>())
        {
            if(Button->isCheckable() && Button->text() == QStringLiteral("Emission")) { Button->setChecked(true); }
        }
        QApplication::processEvents();

        /* Over life: choosing a gradient seeds it from Start/End, in one step, and shows the gradient editor. */
        Details.resize(420, 1400);
        Details.show();
        QApplication::processEvents();
        QComboBox* ColorMode = nullptr;
        for(QComboBox* Each : Details.findChildren<QComboBox*>())
        {
            if(Each->count() == 2 && Each->itemText(1) == QStringLiteral("Gradient")) { ColorMode = Each; }
        }
        Expect(ColorMode && Shows("Start Color") && Shows("Fade In") && !Shows("Color Gradient"), "Start/End shows its colours and fades");
        const int StepsBefore = Document.GetHistory().count();
        if(ColorMode) { ColorMode->setCurrentIndex(1); }
        /* Twice: the rebuild, then the new rows shown (Qt shows them a turn later in a visible panel). */
        QApplication::processEvents();
        QApplication::processEvents();
        const RenderCore::ParticleEmitterDesc& Graded = *Document.GetEmitter(2);
        Expect(Graded.ColorMode == RenderCore::POL_Keys && Graded.ColorKeyCount == 4 && Graded.ColorKeys[0].Color.A == 0
               && Document.GetHistory().count() == StepsBefore + 1,
               "a Gradient chosen starts as Start/End did -- four keys, faded in and out -- in one undo step");
        auto* Gradient = Details.findChild<ParticleGradientEdit*>();
        Expect(Gradient && Gradient->isVisibleTo(&Details) && Gradient->GetKeys().size() == 4 && !Shows("Start Color") && !Shows("Fade In"),
               "the gradient editor shows its keys, in place of Start/End's settings");
        if(Gradient)
        {
            /* A double-click on the strip adds a key there: one undo step. */
            const QPointF At(Gradient->width() * 0.4, 12.0);
            QMouseEvent Click(QEvent::MouseButtonDblClick, At, Gradient->mapToGlobal(At), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(Gradient, &Click);
            QApplication::processEvents();
            Expect(Document.GetEmitter(2)->ColorKeyCount == 5 && Document.GetHistory().undoText() == QStringLiteral("Edit Color Gradient"),
                   "a double-click on the gradient adds a key, recorded");
            Document.GetHistory().undo();
            QApplication::processEvents();
            Expect(Document.GetEmitter(2)->ColorKeyCount == 4 && Gradient->GetKeys().size() == 4, "and undo takes it away, editor and all");
        }
        QComboBox* SizeMode = nullptr;
        for(QComboBox* Each : Details.findChildren<QComboBox*>())
        {
            if(Each->count() == 2 && Each->itemText(1) == QStringLiteral("Curve")) { SizeMode = Each; }
        }
        if(SizeMode) { SizeMode->setCurrentIndex(1); }
        QApplication::processEvents();
        QApplication::processEvents();
        auto* Curve = Details.findChild<ParticleCurveEdit*>();
        Expect(SizeMode && Curve && Document.GetEmitter(2)->SizeKeyCount == 2 && Curve->GetKeys().size() == 2 && !Shows("Start Size"),
               "a size Curve chosen starts as Start to End, in the curve editor");
        Details.hide();

        Document.RemoveEmitter(0);
        Document.RemoveEmitter(0);
        Document.RemoveEmitter(0);
        Expect(Rows->count() == 0 && Document.GetSelected() == -1, "with every emitter removed, nothing is selected");
    }
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);

    std::printf("NimbleEditorParticleTests\n");
    QTemporaryDir Dir;
    TestDocument(Dir.path());
    TestPanels(Dir.path());

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
