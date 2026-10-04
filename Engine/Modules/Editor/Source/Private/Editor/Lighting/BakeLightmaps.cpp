#if WITH_EDITOR

#include "Private/Editor/Lighting/BakeLightmaps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTimer>

#include "Public/Core/CoreLog.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/RenderCore/BakedLightingAsset.h"
#include "Private/Editor/Lighting/LightBaker.h"
#include "Private/Editor/Lighting/LightBakeWorld.h"

namespace EditorLighting
{
    namespace
    {
        /* What the worker shares with the dialog. */
        struct BakeJob
        {
            std::mutex             Lock;
            std::string            Stage = "Starting";
            float                  Fraction = 0.0f;
            std::atomic<bool>      bCancel{false};
            std::atomic<bool>      bFinished{false};
            LightBake::EBakeStatus Status = LightBake::EBakeStatus::Failed;
            LightBake::Result      Result;
            std::string            Error;
        };
    }

    namespace
    {
        constexpr int MEASURE_SIZE    = 32;     // texels across a material's quad
        constexpr int MEASURE_TIMEOUT = 8000;   // ms for every material's previews

        /* An image's average, linear (each texel display space). */
        LightBake::Vec3 AverageLinear(const QImage& InImage, const float InScale)
        {
            const QImage Image = InImage.convertToFormat(QImage::Format_RGBA8888);
            double Sum[3] = {};
            const int Count = Image.width() * Image.height();
            for(int Y = 0; Y < Image.height(); ++Y)
            {
                const uchar* Row = Image.constScanLine(Y);
                for(int X = 0; X < Image.width(); ++X)
                {
                    for(int C = 0; C < 3; ++C) { Sum[C] += std::pow(double(Row[X * 4 + C]) / 255.0, 2.2); }
                }
            }
            if(Count == 0) { return LightBake::Vec3(); }
            return LightBake::Vec3(float(Sum[0] / Count), float(Sum[1] / Count), float(Sum[2] / Count)) * InScale;
        }

        /* Each material's base colour and emissive, as the renderer draws them;
         * a material it cannot draw is left out (the bake uses its parameters). */
        LightBake::MeasuredMaterials MeasureMaterials(EditorContext& InContext, const std::vector<std::string>& InMaterials,
                                                      QWidget* InParent)
        {
            LightBake::MeasuredMaterials Out;
            struct Pending { std::string Material; uint64_t Base = 0, Glow = 0; QImage BaseImage, GlowImage; bool bBase = false, bGlow = false; };
            std::vector<Pending> Requests;
            for(const std::string& Each : InMaterials)
            {
                Pending P;
                P.Material = Each;
                P.Base = InContext.RequestMaterialPreview(QString::fromStdString(Each), MEASURE_SIZE, EngineCore::Color(255, 255, 255),
                                                          RenderCore::EMaterialPreviewChannel::BaseColor);
                P.Glow = InContext.RequestMaterialPreview(QString::fromStdString(Each), MEASURE_SIZE, EngineCore::Color(255, 255, 255),
                                                          RenderCore::EMaterialPreviewChannel::Emissive);
                if(P.Base && P.Glow) { Requests.push_back(std::move(P)); }
            }
            if(Requests.empty()) { return Out; }

            QProgressDialog Progress(QStringLiteral("Measuring materials..."), QString(), 0, int(Requests.size()), InParent);
            Progress.setWindowTitle(QStringLiteral("Bake Lightmaps"));
            Progress.setWindowModality(Qt::WindowModal);
            Progress.setMinimumDuration(500);
            QElapsedTimer Clock;
            Clock.start();
            QEventLoop Loop;
            QTimer Poll;
            QObject::connect(&Poll, &QTimer::timeout, &Loop, [&]()
            {
                int Done = 0;
                for(Pending& Each : Requests)
                {
                    QString Error;
                    if(!Each.bBase) { Each.bBase = InContext.TakeMaterialPreview(Each.Base, Each.BaseImage, Error); }
                    if(!Each.bGlow) { Each.bGlow = InContext.TakeMaterialPreview(Each.Glow, Each.GlowImage, Error); }
                    Done += Each.bBase && Each.bGlow ? 1 : 0;
                }
                Progress.setValue(Done);
                if(Done == int(Requests.size()) || Clock.elapsed() > MEASURE_TIMEOUT) { Loop.quit(); }
            });
            Poll.start(20);
            Loop.exec();

            for(const Pending& Each : Requests)
            {
                if(Each.BaseImage.isNull() || Each.GlowImage.isNull()) { continue; }
                LightBake::MeasuredMaterial Measured;
                Measured.BaseColor = AverageLinear(Each.BaseImage, 1.0f);
                Measured.Emissive  = AverageLinear(Each.GlowImage, RenderCore::EMISSIVE_PREVIEW_SCALE);
                Out[Each.Material] = Measured;
                CORE_LOG("[Bake] measured " + Each.Material + ": base colour " + std::to_string(Measured.BaseColor.X) + " "
                         + std::to_string(Measured.BaseColor.Y) + " " + std::to_string(Measured.BaseColor.Z) + ", emissive "
                         + std::to_string(Measured.Emissive.X) + " " + std::to_string(Measured.Emissive.Y) + " " + std::to_string(Measured.Emissive.Z))
            }
            if(Out.size() < InMaterials.size())
            {
                CORE_LOG_WARN("[Bake] " + std::to_string(InMaterials.size() - Out.size())
                              + " material(s) could not be drawn to measure (a Null renderer?); they bake by their BaseColor parameters")
            }
            return Out;
        }
    }

    bool BakeLightmaps(EditorDocument& InDocument, EditorContext& InContext, const QString& InWorldFile, QWidget* InParent,
                       const std::function<void(const QString&)>& InStatus)
    {
        const QString Title = QStringLiteral("Bake Lightmaps");
        if(&InDocument.GetWorld() != &InDocument.GetEditedWorld())
        {
            QMessageBox::information(InParent, Title, QStringLiteral("Stop playing first: the bake is of the edited world."));
            return false;
        }
        if(InWorldFile.isEmpty())
        {
            QMessageBox::information(InParent, Title, QStringLiteral("Save the world first: its bake is saved beside it."));
            return false;
        }

        GameCore::WorldSettings Settings = InDocument.GetWorldSettings();
        if(!Settings.Lighting.bBakedLighting)
        {
            if(QMessageBox::question(InParent, Title,
                                     QStringLiteral("Baked lighting is off for this world (World > World Settings).\n\n"
                                                    "Turn it on and bake?")) != QMessageBox::Yes)
            {
                return false;
            }
            Settings.Lighting.bBakedLighting = true;
            InDocument.SetWorldSettings(Settings);
        }

        // --- Gather, here: it reads the World.
        auto Scene = std::make_shared<LightBake::Scene>();
        LightBake::GatherReport Report;
        const LightBake::MeasuredMaterials Measured =
            MeasureMaterials(InContext, LightBake::ListBakeMaterials(InDocument.GetEditedWorld()), InParent);
        LightBake::GatherScene(InDocument.GetEditedWorld(), *Scene, Report, &Measured);
        for(const std::string& Each : Report.Warnings) { CORE_LOG_WARN("[Bake] " + Each) }
        if(Report.Meshes == 0)
        {
            QMessageBox::information(InParent, Title, QStringLiteral("Nothing to bake: no entity with a Render component is Static.\n\n"
                                                                     "Set the level geometry's Mobility to Static in the Details panel."));
            return false;
        }
        if(Report.Lights == 0 && !Report.bAnySky)
        {
            QMessageBox::information(InParent, Title, QStringLiteral("Nothing lights the bake: no light or SkyLight is Static or Stationary.\n\n"
                                                                     "Set the lights' Mobility in the Details panel."));
            return false;
        }
        CORE_LOG("[Bake] " + std::to_string(Report.Meshes) + " static meshes (" + std::to_string(Report.Lightmapped)
                 + " lightmapped), " + std::to_string(Report.Lights) + " static or stationary lights"
                 + (Report.bSky ? ", the sky" : (Report.bAnySky ? ", the sky's bounce" : "")))

        LightBake::Settings Config;
        Config.TexelsPerUnit   = Settings.Lighting.TexelsPerUnit;
        Config.Bounces         = Settings.Lighting.Bounces;
        Config.SamplesPerTexel = Settings.Lighting.SamplesPerTexel;
        Config.ProbeSpacing    = Settings.Lighting.ProbeSpacing;
        Config.MaxAtlasSize    = Settings.Lighting.MaxAtlasSize;

        // --- Bake, on a worker; the dialog polls it.
        auto Job = std::make_shared<BakeJob>();
        std::thread Worker([Job, Scene, Config]()
        {
            Job->Status = LightBake::Bake(*Scene, Config, Job->Result, Job->Error,
                                          [&Job](const char* InStage, const float InFraction)
                                          {
                                              std::lock_guard<std::mutex> Guard(Job->Lock);
                                              Job->Stage    = InStage;
                                              Job->Fraction = InFraction;
                                          },
                                          &Job->bCancel);
            Job->bFinished = true;
        });

        QProgressDialog Progress(QStringLiteral("Starting..."), QStringLiteral("Cancel"), 0, 1000, InParent);
        Progress.setWindowTitle(Title);
        Progress.setWindowModality(Qt::WindowModal);
        Progress.setMinimumDuration(0);
        Progress.setValue(0);

        QElapsedTimer Clock;
        Clock.start();
        QEventLoop Loop;
        QTimer Poll;
        QObject::connect(&Poll, &QTimer::timeout, &Loop, [&]()
        {
            if(Progress.wasCanceled()) { Job->bCancel = true; }
            {
                std::lock_guard<std::mutex> Guard(Job->Lock);
                Progress.setLabelText(QStringLiteral("%1 (%2 s)").arg(QString::fromStdString(Job->Stage)).arg(Clock.elapsed() / 1000));
                Progress.setValue(int(Job->Fraction * 999.0f));
            }
            if(Job->bFinished) { Loop.quit(); }
        });
        Poll.start(100);
        Loop.exec();
        Worker.join();
        Progress.close();

        if(Job->Status == LightBake::EBakeStatus::Cancelled)
        {
            InStatus(QStringLiteral("Bake cancelled; the world keeps its last bake"));
            return false;
        }
        if(Job->Status == LightBake::EBakeStatus::Failed)
        {
            QMessageBox::warning(InParent, Title, QStringLiteral("The bake failed: %1").arg(QString::fromStdString(Job->Error)));
            return false;
        }

        // --- Save, and point the world at it.
        std::string BakedData, Error;
        if(!LightBake::SaveBake(InWorldFile.toStdString(), *Scene, Job->Result, Report.bSky, BakedData, Error))
        {
            QMessageBox::warning(InParent, Title, QStringLiteral("The bake could not be saved: %1").arg(QString::fromStdString(Error)));
            return false;
        }
        RenderCore::BakedLightingCache::Invalidate(BakedData);
        Settings = InDocument.GetWorldSettings();
        Settings.Lighting.BakedData = BakedData;
        InDocument.SetWorldSettings(Settings);

        const QString Done = QStringLiteral("Baked %1 lightmap(s) and %2 light probes in %3 s -- save the world to keep it")
                             .arg(Job->Result.Atlases.size()).arg(Job->Result.Probes.Probes.size())
                             .arg(double(Clock.elapsed()) / 1000.0, 0, 'f', 1);
        CORE_LOG("[Bake] " + Done.toStdString())
        InStatus(Done);
        return true;
    }
}

#endif // WITH_EDITOR
