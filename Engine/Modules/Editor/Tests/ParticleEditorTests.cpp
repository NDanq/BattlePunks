/*
 * NimbleEditorParticleTests: the Particle Editor's document and panels,
 * offscreen -- no Renderer, no window shown. Edits and their undo, the
 * emitter list, saving and loading, and the panels following the document.
 */

#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QFile>
#include <QGroupBox>
#include <QListWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

#include "Public/RenderCore/ParticleSystemAsset.h"
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

        int Groups = 0;
        for(QGroupBox* Box : Details.findChildren<QGroupBox*>()) { Groups += Box->isVisibleTo(&Details) ? 1 : 0; }
        Expect(Groups == 4, "the details group the emitter's settings: Rendering, Emission, Motion, Over Life");

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
        Document.GetEmitter(2)->Drag = 2.0f;
        Document.CommitLiveEdit(QStringLiteral("Edit Drag"));
        const QList<QToolButton*> Resets = VisibleResets();
        Expect(Resets.size() == 1, "a changed setting shows its reset arrow");
        if(Resets.size() == 1)
        {
            Resets.front()->click();
            Expect(Document.GetEmitter(2)->Drag == 0.0f && Document.GetHistory().undoText() == QStringLiteral("Reset Drag")
                   && VisibleResets().isEmpty(), "which resets it, as an undo step");
        }

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
