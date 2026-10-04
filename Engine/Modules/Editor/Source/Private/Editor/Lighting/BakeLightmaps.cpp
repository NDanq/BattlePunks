#if WITH_EDITOR

#include "Private/Editor/Lighting/BakeLightmaps.h"

#include <atomic>
#include <chrono>
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

    bool BakeLightmaps(EditorDocument& InDocument, const QString& InWorldFile, QWidget* InParent,
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
        LightBake::GatherScene(InDocument.GetEditedWorld(), *Scene, Report);
        for(const std::string& Each : Report.Warnings) { CORE_LOG_WARN("[Bake] " + Each) }
        if(Report.Meshes == 0)
        {
            QMessageBox::information(InParent, Title, QStringLiteral("Nothing to bake: no entity with a Render component is Static.\n\n"
                                                                     "Set the level geometry's Mobility to Static in the Details panel."));
            return false;
        }
        if(Report.Lights == 0 && !Report.bSky)
        {
            QMessageBox::information(InParent, Title, QStringLiteral("Nothing lights the bake: no light or SkyLight is Static.\n\n"
                                                                     "Set the lights' Mobility to Static in the Details panel."));
            return false;
        }
        CORE_LOG("[Bake] " + std::to_string(Report.Meshes) + " static meshes (" + std::to_string(Report.Lightmapped)
                 + " lightmapped), " + std::to_string(Report.Lights) + " static lights" + (Report.bSky ? ", the sky" : ""))

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
