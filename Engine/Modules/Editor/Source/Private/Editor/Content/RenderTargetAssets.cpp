#if WITH_EDITOR

#include "Private/Editor/Content/RenderTargetAssets.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <memory>
#include <set>

#include <QTimer>

#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/SceneCaptureProbeComponent.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Prefab/PrefabAssets.h"

namespace EditorRenderTargets
{
    namespace
    {
        using RenderCore::ERenderTargetKind;

        const char* KindName(const ERenderTargetKind InKind)
        {
            return InKind == ERenderTargetKind::Cube ? "RenderTargetTex3D" : "RenderTargetTex2D";
        }

        bool Save(const QString& InPath, RenderCore::RenderTargetData& InData, QString& OutError)
        {
            if(!RenderCore::SaveRenderTargetFile(InPath.toStdString(), InData))
            {
                OutError = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(InPath));
                return false;
            }
            EditorContent::StageAsset(InPath);   // and forgets the cached one, so a viewport sees it at once
            return true;
        }
    }

    QString CreateRenderTarget(const QString& InFolder, const ERenderTargetKind InKind, QWidget* InParent)
    {
        const QString Extension = InKind == ERenderTargetKind::Cube ? QStringLiteral(".nmrt3d") : QStringLiteral(".nmrt2d");
        bool bOk = false;
        const QString Name = QInputDialog::getText(InParent, QStringLiteral("New %1").arg(QString::fromLatin1(KindName(InKind))),
                                                   QStringLiteral("Name"), QLineEdit::Normal,
                                                   InKind == ERenderTargetKind::Cube ? QStringLiteral("Reflection") : QStringLiteral("Mirror"),
                                                   &bOk).trimmed();
        if(!bOk || Name.isEmpty())
        {
            return QString();
        }
        const QString Path = QDir(InFolder).filePath(Name + Extension);
        if(QFileInfo::exists(Path))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Render Target"), QStringLiteral("%1 already exists.").arg(Name + Extension));
            return QString();
        }
        RenderCore::RenderTargetData Data;
        Data.Desc.Kind   = InKind;
        Data.Desc.Width  = InKind == ERenderTargetKind::Cube ? 128u : 512u;
        Data.Desc.Height = Data.Desc.Width;
        QString Error;
        if(!Save(Path, Data, Error))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Render Target"), Error);
            return QString();
        }
        return Path;
    }

    void EditRenderTarget(const QString& InPath, QWidget* InParent)
    {
        RenderCore::RenderTargetData Data;
        if(!RenderCore::LoadRenderTargetFile(InPath.toStdString(), Data))
        {
            QMessageBox::warning(InParent, QStringLiteral("Render Target"),
                                 QStringLiteral("%1 is not a readable render target.").arg(QDir::toNativeSeparators(InPath)));
            return;
        }
        const bool bCube = Data.Desc.Kind == ERenderTargetKind::Cube;

        QDialog Dialog(InParent);
        Dialog.setWindowTitle(QStringLiteral("%1 -- %2").arg(QFileInfo(InPath).fileName(), QString::fromLatin1(KindName(Data.Desc.Kind))));
        auto* Layout = new QVBoxLayout(&Dialog);
        auto* Form   = new QFormLayout();
        Layout->addLayout(Form);

        auto* Width = new QSpinBox(&Dialog);
        Width->setRange(1, int(RenderCore::RENDER_TARGET_MAX_SIZE));
        Width->setValue(int(Data.Desc.Width));
        Width->setToolTip(bCube ? QStringLiteral("Each of the six faces' side, in texels (a probe's Resolution overrides it)")
                                : QStringLiteral("In texels (a probe's Resolution overrides it, keeping the aspect)"));
        Form->addRow(bCube ? QStringLiteral("Face size") : QStringLiteral("Width"), Width);
        auto* Height = new QSpinBox(&Dialog);
        Height->setRange(1, int(RenderCore::RENDER_TARGET_MAX_SIZE));
        Height->setValue(int(Data.Desc.Height));
        if(!bCube) { Form->addRow(QStringLiteral("Height"), Height); }

        auto* Filter = new QComboBox(&Dialog);
        Filter->addItems({ QStringLiteral("Nearest"), QStringLiteral("Linear") });
        Filter->setCurrentIndex(int(Data.Desc.Filter));
        Form->addRow(QStringLiteral("Filter"), Filter);
        auto* Wrap = new QComboBox(&Dialog);
        Wrap->addItems({ QStringLiteral("Repeat"), QStringLiteral("Clamp"), QStringLiteral("Mirror") });
        Wrap->setCurrentIndex(int(Data.Desc.Wrap));
        Form->addRow(QStringLiteral("Wrap"), Wrap);

        QColor Clear(Data.Desc.Clear.R, Data.Desc.Clear.G, Data.Desc.Clear.B);
        auto* ClearButton = new QPushButton(Clear.name(), &Dialog);
        const auto Paint = [ClearButton](const QColor& InColor)
        {
            ClearButton->setText(InColor.name());
            ClearButton->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; }")
                                           .arg(InColor.name(), InColor.lightness() > 128 ? QStringLiteral("black") : QStringLiteral("white")));
        };
        Paint(Clear);
        QObject::connect(ClearButton, &QPushButton::clicked, &Dialog, [&]()
        {
            const QColor Picked = QColorDialog::getColor(Clear, &Dialog, QStringLiteral("Clear colour"));
            if(Picked.isValid()) { Clear = Picked; Paint(Clear); }
        });
        ClearButton->setToolTip(QStringLiteral("What a capture shows where nothing is drawn"));
        Form->addRow(QStringLiteral("Clear colour"), ClearButton);

        bool bClearBake = false;
        auto* BakeRow   = new QWidget(&Dialog);
        auto* BakeLine  = new QHBoxLayout(BakeRow);
        BakeLine->setContentsMargins(0, 0, 0, 0);
        auto* BakeLabel = new QLabel(Data.Baked.IsNull() ? QStringLiteral("none") : QStringLiteral("%1 x %2").arg(Data.Baked.GetWidth()).arg(Data.Baked.GetHeight()),
                                     BakeRow);
        BakeLine->addWidget(BakeLabel, 1);
        auto* ClearBake = new QPushButton(QStringLiteral("Clear"), BakeRow);
        ClearBake->setEnabled(!Data.Baked.IsNull());
        QObject::connect(ClearBake, &QPushButton::clicked, &Dialog, [&]() { bClearBake = true; BakeLabel->setText(QStringLiteral("none (on OK)")); ClearBake->setEnabled(false); });
        BakeLine->addWidget(ClearBake);
        BakeRow->setToolTip(QStringLiteral("A capture stored in the file (World > Bake Scene Captures): what materials sample until a "
                                           "probe captures live, and always for a probe that never does. A size change drops it."));
        Form->addRow(QStringLiteral("Baked"), BakeRow);

        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
        QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
        QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
        Layout->addWidget(Buttons);
        if(Dialog.exec() != QDialog::Accepted)
        {
            return;
        }

        const uint32_t NewWidth  = uint32_t(Width->value());
        const uint32_t NewHeight = bCube ? NewWidth : uint32_t(Height->value());
        if(bClearBake || NewWidth != Data.Desc.Width || NewHeight != Data.Desc.Height)
        {
            Data.Baked = EngineCore::Image();   // no longer fits
        }
        Data.Desc.Width  = NewWidth;
        Data.Desc.Height = NewHeight;
        Data.Desc.Filter = RenderCore::ETextureFilter(Filter->currentIndex());
        Data.Desc.Wrap   = RenderCore::ETextureWrap(Wrap->currentIndex());
        Data.Desc.Clear  = EngineCore::Color(uint8_t(Clear.red()), uint8_t(Clear.green()), uint8_t(Clear.blue()));
        QString Error;
        if(!Save(InPath, Data, Error))
        {
            QMessageBox::warning(InParent, QStringLiteral("Render Target"), Error);
        }
    }

    bool BakeRenderTarget(const QString& InPath, const EngineCore::Image& InPixels, QString& OutError)
    {
        RenderCore::RenderTargetData Data;
        if(!RenderCore::LoadRenderTargetFile(InPath.toStdString(), Data))
        {
            OutError = QStringLiteral("%1 is not a readable render target").arg(QDir::toNativeSeparators(InPath));
            return false;
        }
        /* What was captured sets the size (a probe's Resolution may differ from the file's). */
        Data.Desc.Width  = Data.Desc.Kind == ERenderTargetKind::Cube ? uint32_t(InPixels.GetHeight()) : uint32_t(InPixels.GetWidth());
        Data.Desc.Height = uint32_t(InPixels.GetHeight());
        if(InPixels.IsNull() || int(RenderCore::RenderTargetTextureWidth(Data.Desc)) != InPixels.GetWidth())
        {
            OutError = QStringLiteral("the capture of %1 is not the shape of its target").arg(QFileInfo(InPath).fileName());
            return false;
        }
        /* RGBA8, as the file stores it (OpenGL reads back BGRA8). */
        EngineCore::Image Pixels(InPixels.GetWidth(), InPixels.GetHeight(), EngineCore::EImageFormat::RGBA8);
        const bool bBgra = InPixels.GetFormat() == EngineCore::EImageFormat::BGRA8;
        for(int Row = 0; Row < InPixels.GetHeight(); ++Row)
        {
            const uint8_t* From = InPixels.GetData() + InPixels.GetStride() * size_t(Row);
            uint8_t*       To   = Pixels.GetData() + Pixels.GetStride() * size_t(Row);
            for(int Column = 0; Column < InPixels.GetWidth(); ++Column, From += 4, To += 4)
            {
                To[0] = bBgra ? From[2] : From[0];
                To[1] = From[1];
                To[2] = bBgra ? From[0] : From[2];
                To[3] = 255;
            }
        }
        Data.Baked = std::move(Pixels);
        return Save(InPath, Data, OutError);
    }

    void BakeSceneCaptures(EditorDocument& InDocument, EditorContext& InContext, QObject* InOwner, std::function<void(const QString&)> InDone)
    {
        /* Every probe captures on the next frame, whatever its frequency. */
        std::set<std::string> Targets;
        InDocument.GetWorld().Each<RenderCore::SceneCaptureProbeComponent>(
            [&Targets](const GameCore::Entity&, RenderCore::SceneCaptureProbeComponent& InProbe)
        {
            const std::string Target(InProbe.Target);
            if(InProbe.bEnabled && !Target.empty() && Target.rfind("transient://", 0) != 0)
            {
                InProbe.bCaptureNow = 1;
                Targets.insert(Target);
            }
        });
        if(Targets.empty())
        {
            InDone(QStringLiteral("Bake Scene Captures: no Scene Capture Probe names a render target file"));
            return;
        }

        /* Read back after that frame's captures; each stored as its target's bake. */
        struct Progress { int Pending = 0; int Baked = 0; QStringList Failed; std::function<void(const QString&)> Done; };
        auto State = std::make_shared<Progress>();
        State->Pending = int(Targets.size());
        State->Done    = std::move(InDone);
        const auto Finish = [State](const QString& InTarget, const QString& InError)
        {
            if(InError.isEmpty()) { ++State->Baked; } else { State->Failed << InTarget + ": " + InError; }
            if(--State->Pending > 0) { return; }
            State->Done(State->Failed.isEmpty()
                        ? QStringLiteral("Baked %1 scene capture(s)").arg(State->Baked)
                        : QStringLiteral("Baked %1 scene capture(s); %2").arg(State->Baked).arg(State->Failed.join(QStringLiteral("; "))));
        };
        for(const std::string& Each : Targets)
        {
            const QString  Runtime = QString::fromStdString(Each);
            const uint64_t Request = InContext.RequestRenderTargetReadback(Runtime);
            if(Request == 0)
            {
                Finish(Runtime, QStringLiteral("the renderer is not running"));
                continue;
            }
            auto* Poll  = new QTimer(InOwner);
            auto  Tries = std::make_shared<int>(0);
            QObject::connect(Poll, &QTimer::timeout, InOwner, [Poll, Tries, Request, Runtime, &InContext, Finish]()
            {
                EngineCore::Image Pixels;
                QString Error;
                if(InContext.TakeRenderTargetReadback(Request, Pixels, Error))
                {
                    Poll->deleteLater();
                    if(Error.isEmpty() && !BakeRenderTarget(EditorPrefabs::ToProjectPath(Runtime), Pixels, Error) && Error.isEmpty())
                    {
                        Error = QStringLiteral("could not be written");
                    }
                    Finish(Runtime, Error);
                    return;
                }
                if(++*Tries > 250)
                {
                    Poll->deleteLater();
                    Finish(Runtime, QStringLiteral("no frame was rendered (is the viewport visible?)"));
                }
            });
            Poll->start(20);
        }
    }

    QString DescribeRenderTarget(const QString& InPath)
    {
        RenderCore::RenderTargetData Data;
        if(!RenderCore::LoadRenderTargetFile(InPath.toStdString(), Data))
        {
            return QStringLiteral("not a readable render target");
        }
        const bool bCube = Data.Desc.Kind == ERenderTargetKind::Cube;
        return QStringLiteral("%1 %2%3").arg(bCube ? QStringLiteral("6 x %1 x %1").arg(Data.Desc.Width)
                                                   : QStringLiteral("%1 x %2").arg(Data.Desc.Width).arg(Data.Desc.Height),
                                             QString::fromLatin1(KindName(Data.Desc.Kind)),
                                             Data.Baked.IsNull() ? QString() : QStringLiteral(", baked"));
    }
}

#endif // WITH_EDITOR
