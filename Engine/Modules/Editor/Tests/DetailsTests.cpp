/*
 * NimbleEditorDetailsTests -- the Details panel, offscreen: what it builds for
 * a component's properties, struct arrays above all (a RenderComponent's
 * material parameter overrides) -- and the Texture Viewer.
 * NIMBLE_DETAILS_SNAPSHOT=<file.png> also saves the panel as drawn, for a
 * look; NIMBLE_TEXTURE_SNAPSHOT=<file.png> the Texture Viewer.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Content/TextureViewerWindow.h"

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

namespace
{
    /* The combo whose items include InItem. */
    QComboBox* ComboWith(const QWidget& InRoot, const QString& InItem)
    {
        for(QComboBox* Each : InRoot.findChildren<QComboBox*>())
        {
            if(Each->findText(InItem, Qt::MatchStartsWith) >= 0) { return Each; }
        }
        return nullptr;
    }

    QPushButton* Button(const QWidget& InRoot, const QString& InText)
    {
        for(QPushButton* Each : InRoot.findChildren<QPushButton*>())
        {
            if(Each->text() == InText) { return Each; }
        }
        return nullptr;
    }

    bool AnyLabelContains(const QWidget& InRoot, const QString& InText)
    {
        for(const QLabel* Each : InRoot.findChildren<QLabel*>())
        {
            if(Each->text().contains(InText)) { return true; }
        }
        return false;
    }

    bool ReadTextureFile(const QString& InPath, RenderCore::TextureData& OutTexture)
    {
        QFile File(InPath);
        if(!File.open(QIODevice::ReadOnly)) { return false; }
        const QByteArray Bytes = File.readAll();
        EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
        return RenderCore::SerializeTexture(Ar, OutTexture);
    }
}

/* The Texture Viewer, offscreen: settings edited, mips rebuilt and shown, saved. */
static void TestTextureViewer()
{
    std::printf("Texture Viewer\n");
    QTemporaryDir Temp;
    const QString Path = Temp.filePath(QStringLiteral("Gradient.nmtexture"));
    {
        RenderCore::TextureData Texture;
        Texture.Pixels = EngineCore::Image(64, 32, EngineCore::EImageFormat::RGBA8);
        for(int Y = 0; Y < 32; ++Y)
        {
            for(int X = 0; X < 64; ++X)
            {
                uint8_t* Px = Texture.Pixels.GetScanline(Y) + X * 4;
                Px[0] = uint8_t(X * 4); Px[1] = uint8_t(Y * 8); Px[2] = ((X / 8 + Y / 8) & 1) ? 220 : 40; Px[3] = 255;
            }
        }
        Texture.ImportInfo.SourcePath = "Source/Gradient.png";
        Texture.ImportInfo.Settings   = "SRGB=1\nFilter=1\nWrap=0\nMaxDimension=4096\nFlipVertically=0\n";   // from before mips
        RenderCore::SaveTextureFile(Path.toStdString(), Texture);
    }

    TextureViewerWindow* Viewer = TextureViewerWindow::Open(Path, nullptr);
    Expect(Viewer != nullptr, "the viewer opens a texture");
    if(!Viewer) { return; }
    PumpFor(50);

    QComboBox* Levels      = ComboWith(*Viewer, QStringLiteral("0: 64 x 32"));
    QComboBox* MipGen      = ComboWith(*Viewer, QStringLiteral("Kaiser"));
    QComboBox* Compression = ComboWith(*Viewer, QStringLiteral("BC7"));
    QPushButton* Save      = Button(*Viewer, QStringLiteral("Save"));
    Expect(Levels && Levels->count() == 1 && MipGen && Compression && Save, "it lists one level for a texture without mips");
    Expect(AnyLabelContains(*Viewer, QStringLiteral("64 x 32")) && AnyLabelContains(*Viewer, QStringLiteral("No mips"))
           && AnyLabelContains(*Viewer, QStringLiteral("GPU memory 8")), "and its statistics: size, no mips, 8 KB on the GPU");
    if(!Levels || !MipGen || !Compression || !Save) { delete Viewer; return; }
    Expect(!Save->isEnabled(), "nothing to save yet");

    MipGen->setCurrentIndex(MipGen->findData(int(RenderCore::ETextureMipGen::Box)));
    PumpFor(20);
    Expect(Levels->count() == 7 && Levels->itemText(6) == QStringLiteral("6: 1 x 1"), "Box mips: seven levels, 64 x 32 down to 1 x 1");
    Expect(Save->isEnabled() && Viewer->windowTitle().contains(QLatin1Char('*')), "and the texture is dirty");
    Levels->setCurrentIndex(3);   // shown: 8 x 4

    Compression->setCurrentIndex(Compression->findData(int(RenderCore::ETextureCompression::BC7)));
    PumpFor(20);
    Expect(AnyLabelContains(*Viewer, QStringLiteral("no block encoder is integrated")), "compression says it is a stored setting only");

    if(const char* Snapshot = std::getenv("NIMBLE_TEXTURE_SNAPSHOT"))
    {
        Viewer->grab().save(QString::fromUtf8(Snapshot));
        std::printf("  saved %s\n", Snapshot);
    }

    Save->click();
    PumpFor(20);
    RenderCore::TextureData Back;
    Expect(ReadTextureFile(Path, Back) && Back.GetMipCount() == 7 && Back.MipGen == RenderCore::ETextureMipGen::Box
           && Back.Compression == RenderCore::ETextureCompression::BC7, "Save writes the chain and the settings");
    Expect(QString::fromStdString(Back.ImportInfo.Settings).contains(QStringLiteral("MipGen=1"))
           && QString::fromStdString(Back.ImportInfo.Settings).contains(QStringLiteral("Compression=5")),
           "and records them in the import options, so Reimport keeps them");
    Expect(!Save->isEnabled() && !Viewer->windowTitle().contains(QLatin1Char('*')), "and it is clean again");

    MipGen->setCurrentIndex(MipGen->findData(int(RenderCore::ETextureMipGen::None)));
    PumpFor(20);
    Expect(Levels->count() == 1, "None drops the chain");
    Button(*Viewer, QStringLiteral("Revert"))->click();
    PumpFor(20);
    Expect(Levels->count() == 7 && !Save->isEnabled(), "Revert brings back what was saved");

    Expect(TextureViewerWindow::Open(Path, nullptr) == Viewer, "opening it again brings the same window forward");
    delete Viewer;
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
    TestTextureViewer();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
