/*
 * NimbleEditorDetailsTests -- the Details panel, offscreen: what it builds for
 * a component's properties, struct arrays above all (a RenderComponent's
 * material parameter overrides). NIMBLE_DETAILS_SNAPSHOT=<file.png> also
 * saves the panel as drawn, for a look.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>

#include "Public/Editor/DetailsPanel.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/RenderComponent.h"

using GameCore::EntityID;

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
    /* Events for InMs: the panel reads values back on a short timer. */
    void PumpFor(const int InMs)
    {
        QElapsedTimer Clock;
        Clock.start();
        while(Clock.elapsed() < InMs)
        {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    bool HasLabel(const QWidget& InRoot, const QString& InText)
    {
        for(const QLabel* Each : InRoot.findChildren<QLabel*>())
        {
            if(Each->text() == InText) { return true; }
        }
        return false;
    }

    int CountLabels(const QWidget& InRoot, const QString& InText)
    {
        int Count = 0;
        for(const QLabel* Each : InRoot.findChildren<QLabel*>())
        {
            if(Each->text() == InText) { ++Count; }
        }
        return Count;
    }

    bool HasLineEdit(const QWidget& InRoot, const QString& InText)
    {
        for(const QLineEdit* Each : InRoot.findChildren<QLineEdit*>())
        {
            if(Each->text() == InText) { return true; }
        }
        return false;
    }
}

static void TestMaterialParameters()
{
    std::printf("Material parameter overrides\n");

    EditorDocument Doc;
    const EntityID Brick = Doc.CreateEntity(QStringLiteral("Brick"), EngineCore::Vector3(0.0, 0.0, 0.0));

    RenderCore::RenderComponent Render;
    std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "%s", "Materials/Brick.material.nmgraph");
    Render.SetMaterialParameter(0, "Color", 1.0f, 0.3f, 0.3f, 1.0f);
    Render.SetMaterialParameter(1, "Glow", 2.5f, 0.0f, 0.0f, 0.0f);
    Doc.GetWorld().GetEntity(Brick).AddComponent(Render);

    DetailsPanel Panel(Doc);
    Panel.resize(460, 1600);
    Panel.show();   // it reads values only while visible (offscreen: no window appears)
    Doc.SetSelection(Brick);
    PumpFor(300);

    Expect(HasLabel(Panel, QStringLiteral("Parameters")) && HasLabel(Panel, QStringLiteral("2 entries")),
           "the overrides are one list, with an entry each");
    Expect(HasLabel(Panel, QStringLiteral("    0")) && HasLabel(Panel, QStringLiteral("    1"))
           && !HasLabel(Panel, QStringLiteral("    2")), "each override in use is an element; the unused ones are not shown");
    Expect(CountLabels(Panel, QStringLiteral("        Name")) == 2 && CountLabels(Panel, QStringLiteral("        Slot")) == 2
           && CountLabels(Panel, QStringLiteral("        W")) == 2, "each element shows its members");
    Expect(HasLineEdit(Panel, QStringLiteral("Color")) && HasLineEdit(Panel, QStringLiteral("Glow")),
           "with their values: the parameter names");

    /* Written by the panel's editor, the component sees it. */
    for(QLineEdit* Each : Panel.findChildren<QLineEdit*>())
    {
        if(Each->text() == QStringLiteral("Glow"))
        {
            Each->setText(QStringLiteral("Pulse"));
            emit Each->editingFinished();
        }
    }
    PumpFor(300);
    float Value[4] = {};
    const RenderCore::RenderComponent* Live = Doc.GetWorld().GetEntity(Brick).GetComponent<RenderCore::RenderComponent>();
    Expect(Live && Live->GetMaterialParameter(1, "Pulse", Value) && Value[0] == 2.5f && !Live->GetMaterialParameter(1, "Glow", Value),
           "editing a member writes that element of the component");

    if(const char* Snapshot = std::getenv("NIMBLE_DETAILS_SNAPSHOT"))
    {
        Panel.grab().save(QString::fromUtf8(Snapshot));
        std::printf("  saved %s\n", Snapshot);
    }
}

int main(int argc, char** argv)
{
    /* No display needed. */
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);

    std::printf("NimbleEditorDetailsTests\n");
    TestMaterialParameters();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
