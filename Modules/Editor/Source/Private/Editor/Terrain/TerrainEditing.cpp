#if WITH_EDITOR

#include "Private/Editor/Terrain/TerrainEditing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>

#include "Public/Core/CoreLog.h"
#include "Public/Editor/EditorConsole.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/RenderCore/Components/TerrainComponent.h"

#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/EditorHistory.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Terrain/TerrainToolsWindow.h"

namespace EditorTerrain
{
    namespace
    {
        Host& TheHost()
        {
            static Host Instance;
            return Instance;
        }

        std::set<std::string>& DirtyPaths()
        {
            static std::set<std::string> Paths;
            return Paths;
        }

        void Point(const EngineCore::Matrix4x4& InMatrix, const EngineCore::Vector3& InPoint, double (&Out)[3])
        {
            for(int Row = 0; Row < 3; ++Row)
            {
                Out[Row] = double(InMatrix.Data[Row][0]) * InPoint.GetX() + double(InMatrix.Data[Row][1]) * InPoint.GetY()
                         + double(InMatrix.Data[Row][2]) * InPoint.GetZ() + double(InMatrix.Data[Row][3]);
            }
        }

        /* Before/after samples of rectangles of one terrain: a stroke (a
         * rectangle per chunk it touched) or an import (the whole grid). */
        class TerrainSamplesCommand final : public EditorHistory::Command
        {
        public:
            struct Patch
            {
                GameCore::TerrainRect Rect;
                std::vector<float>    BeforeHeights, AfterHeights;
                std::vector<uint8_t>  BeforeWeights, AfterWeights;
            };

            TerrainSamplesCommand(EditorDocument& InDocument, const QString& InText, std::string InPath,
                                  std::shared_ptr<GameCore::TerrainData> InData, std::vector<Patch> InPatches)
                : Command(InDocument, InText), Path(std::move(InPath)), Data(std::move(InData)), Patches(std::move(InPatches))
            {
            }

        protected:
            void Apply(const bool bInForward) override
            {
                /* The cache's copy if it is still the one edited; else whatever the path loads now. */
                std::shared_ptr<GameCore::TerrainData> Live = GameCore::TerrainAssetCache::FindLoaded(Path);
                if(!Live || Live != Data)
                {
                    CORE_LOG_WARN("[Terrain] '" + Path + "' was reloaded since this edit; its undo step does nothing")
                    return;
                }
                for(const Patch& Each : Patches)
                {
                    WriteRect(*Data, Each.Rect, bInForward ? Each.AfterHeights : Each.BeforeHeights,
                              bInForward ? Each.AfterWeights : Each.BeforeWeights);
                }
                MarkDirty(Path);
            }

        private:
            std::string                            Path;
            std::shared_ptr<GameCore::TerrainData> Data;
            std::vector<Patch>                     Patches;
        };

        bool RecordPatches(const Target& InTarget, const QString& InLabel, std::vector<TerrainSamplesCommand::Patch> InPatches)
        {
            EditorDocument* Document = TheHost().Document;
            if(!Document || InPatches.empty()) { return false; }
            return Document->RecordToolCommand(new TerrainSamplesCommand(*Document, InLabel, InTarget.Path, InTarget.Data,
                                                                         std::move(InPatches)));
        }

        QString EntityName(const GameCore::EntityID InEntity)
        {
            EditorDocument* Document = TheHost().Document;
            if(!Document) { return QString(); }
            return QString::fromStdString(Document->GetWorld().GetEntity(InEntity).GetName());
        }
    }

    void SetHost(const Host& InHost)
    {
        TheHost() = InHost;
    }

    const Host& GetHost()
    {
        return TheHost();
    }

    // === Targets ==============================================================

    EngineCore::Vector3 Target::ToLocal(const EngineCore::Vector3& InWorld) const
    {
        double Out[3];
        Point(WorldToLocal, InWorld, Out);
        return EngineCore::Vector3(Out[0], Out[1], Out[2]);
    }

    EngineCore::Vector3 Target::ToWorld(const EngineCore::Vector3& InLocal) const
    {
        double Out[3];
        Point(LocalToWorld, InLocal, Out);
        return EngineCore::Vector3(Out[0], Out[1], Out[2]);
    }

    float Target::ToLocalLength(const float InWorld) const
    {
        const double SX = std::sqrt(double(LocalToWorld.Data[0][0]) * LocalToWorld.Data[0][0] + double(LocalToWorld.Data[1][0]) * LocalToWorld.Data[1][0]
                                    + double(LocalToWorld.Data[2][0]) * LocalToWorld.Data[2][0]);
        const double SZ = std::sqrt(double(LocalToWorld.Data[0][2]) * LocalToWorld.Data[0][2] + double(LocalToWorld.Data[1][2]) * LocalToWorld.Data[1][2]
                                    + double(LocalToWorld.Data[2][2]) * LocalToWorld.Data[2][2]);
        const double Scale = 0.5 * (SX + SZ);
        return Scale > 1e-9 ? float(double(InWorld) / Scale) : InWorld;
    }

    Target FindTarget(GameCore::World& InWorld, const GameCore::EntityID InEntity)
    {
        Target Result;
        if(!InEntity.IsSet() || !InWorld.IsAlive(InEntity)) { return Result; }
        const GameCore::Entity E = InWorld.GetEntity(InEntity);
        const auto* Terrain = E.GetComponent<RenderCore::TerrainComponent>();
        if(!Terrain || Terrain->Terrain[0] == '\0') { return Result; }
        Result.Path = Terrain->Terrain;
        Result.Data = GameCore::TerrainAssetCache::Find(Result.Path);
        if(!Result.Data) { return Target(); }
        Result.Entity       = InEntity;
        Result.LocalToWorld = E.GetWorldMatrix();
        Result.WorldToLocal = EngineCore::Matrix4x4::Inverse(Result.LocalToWorld);
        return Result;
    }

    Target FindEditTarget()
    {
        EditorDocument* Document = TheHost().Document;
        if(!Document || Document->IsWorldBorrowed()) { return Target(); }
        GameCore::World& World = Document->GetWorld();
        Target Selected = FindTarget(World, Document->GetSelection());
        if(Selected.IsValid()) { return Selected; }

        GameCore::EntityID Only;
        int Count = 0;
        World.Each<RenderCore::TerrainComponent>([&](const GameCore::Entity& InEntity, RenderCore::TerrainComponent&)
        {
            ++Count;
            Only = InEntity.GetID();
        });
        return Count == 1 ? FindTarget(World, Only) : Target();
    }

    bool PickTerrain(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection, Target& OutTarget,
                     EngineCore::Vector3& OutHit)
    {
        EditorDocument* Document = TheHost().Document;
        if(!Document) { return false; }
        NimbleEngine::TerrainRaycastHit Hit;
        if(!NimbleEngine::RaycastTerrain(Document->GetWorld(), InOrigin, InDirection, 1.0e9f, Hit, false)) { return false; }
        OutTarget = FindTarget(Document->GetWorld(), Hit.Entity);
        OutHit    = Hit.Point;
        return OutTarget.IsValid();
    }

    // === Making one ===========================================================

    bool TerrainPathFor(const QString& InName, QString& OutRuntime, QString& OutFile, QString& OutError)
    {
        const QString World = TheHost().GetWorldFile ? TheHost().GetWorldFile() : QString();
        if(World.isEmpty() || !World.endsWith(QStringLiteral(".nmworld"), Qt::CaseInsensitive))
        {
            OutError = QStringLiteral("Save the level first: its terrains go in a Terrain folder beside it");
            return false;
        }
        QString Name = InName.trimmed();
        Name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_\\-]")), QStringLiteral("_"));
        if(Name.isEmpty()) { Name = QStringLiteral("Terrain"); }

        const QFileInfo Info(World);
        const QString Folder = QDir(Info.absolutePath()).filePath(Info.completeBaseName() + QStringLiteral("/Terrain"));
        OutFile    = QDir(Folder).filePath(Name + QStringLiteral(".nmterrain"));
        OutRuntime = EditorContent::ToRuntimePath(OutFile);
        if(OutRuntime.isEmpty())
        {
            OutError = QStringLiteral("The level is not inside the project");
            return false;
        }
        if(QFileInfo::exists(OutFile) || GameCore::TerrainAssetCache::FindLoaded(OutRuntime.toStdString()))
        {
            OutError = QStringLiteral("A terrain named %1 already exists for this level").arg(Name);
            return false;
        }
        return true;
    }

    GameCore::EntityID CreateTerrain(const CreateSettings& InSettings, const EngineCore::Vector3& InAt, QString& OutError)
    {
        EditorDocument* Document = TheHost().Document;
        if(!Document) { OutError = QStringLiteral("No level is open"); return GameCore::EntityID(); }
        if(Document->IsWorldBorrowed()) { OutError = QStringLiteral("Stop playing first"); return GameCore::EntityID(); }

        QString Runtime, File;
        if(!TerrainPathFor(InSettings.Name, Runtime, File, OutError)) { return GameCore::EntityID(); }

        auto Data = std::make_shared<GameCore::TerrainData>();
        Data->Reset(InSettings.Resolution, InSettings.Spacing, InSettings.Height);
        Data->LayerNames[0] = "Grass";
        Data->LayerNames[1] = "Earth";
        Data->LayerNames[2] = "Rock";
        Data->LayerNames[3] = "Snow";
        QDir().mkpath(QFileInfo(File).absolutePath());
        if(!GameCore::SaveTerrainFile(File.toStdString(), *Data))
        {
            OutError = QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(File));
            return GameCore::EntityID();
        }
        EditorContent::StageAsset(File);
        const std::string Path = Runtime.toStdString();
        GameCore::TerrainAssetCache::Store(Path, Data);

        const double Half = InSettings.bCentred ? double(Data->GetSize()) * 0.5 : 0.0;
        const EngineCore::Vector3 Corner(InAt.GetX() - Half, InAt.GetY(), InAt.GetZ() - Half);
        const bool bCollision = InSettings.bCollision;
        const GameCore::EntityID Made = Document->CreateEntity(InSettings.Name.trimmed().isEmpty() ? QStringLiteral("Terrain") : InSettings.Name.trimmed(),
                                                               Corner, [&](const GameCore::Entity& InEntity)
        {
            RenderCore::TerrainComponent Terrain;
            std::snprintf(Terrain.Terrain, sizeof(Terrain.Terrain), "%s", Path.c_str());
            InEntity.AddComponent(Terrain);
            if(bCollision)
            {
                GameCore::RigidBodyComponent Body;
                Body.Motion = GameCore::RBM_Static;
                Body.Shape  = GameCore::RBS_HeightField;
                InEntity.AddComponent(Body);
            }
            InEntity.SetMobility(GameCore::EMobility::Static);
        });
        if(Made.IsSet()) { Document->SetSelection(Made); }
        return Made;
    }

    // === Strokes ==============================================================

    void CopyRect(const GameCore::TerrainData& InData, const GameCore::TerrainRect& InRect,
                  std::vector<float>& OutHeights, std::vector<uint8_t>& OutWeights)
    {
        std::lock_guard<std::mutex> Guard(InData.GetLock());
        const uint32_t Resolution = InData.GetResolution();
        const int32_t  Width      = InRect.X1 - InRect.X0 + 1;
        const int32_t  Height     = InRect.Z1 - InRect.Z0 + 1;
        OutHeights.resize(size_t(Width) * size_t(Height));
        OutWeights.resize(size_t(Width) * size_t(Height) * GameCore::TERRAIN_LAYERS);
        for(int32_t Z = 0; Z < Height; ++Z)
        {
            const size_t From = size_t(InRect.Z0 + Z) * Resolution + size_t(InRect.X0);
            std::memcpy(&OutHeights[size_t(Z) * Width], InData.GetHeights() + From, sizeof(float) * size_t(Width));
            std::memcpy(&OutWeights[size_t(Z) * Width * GameCore::TERRAIN_LAYERS], InData.GetWeights() + From * GameCore::TERRAIN_LAYERS,
                        size_t(Width) * GameCore::TERRAIN_LAYERS);
        }
    }

    void WriteRect(GameCore::TerrainData& InData, const GameCore::TerrainRect& InRect,
                   const std::vector<float>& InHeights, const std::vector<uint8_t>& InWeights)
    {
        std::lock_guard<std::mutex> Guard(InData.GetLock());
        const uint32_t Resolution = InData.GetResolution();
        const int32_t  Width      = InRect.X1 - InRect.X0 + 1;
        const int32_t  Height     = InRect.Z1 - InRect.Z0 + 1;
        if(InRect.IsEmpty() || InRect.X1 >= int32_t(Resolution) || InRect.Z1 >= int32_t(Resolution)
           || InHeights.size() != size_t(Width) * size_t(Height)
           || InWeights.size() != size_t(Width) * size_t(Height) * GameCore::TERRAIN_LAYERS)
        {
            return;   // the terrain was resized since: nothing fits
        }
        for(int32_t Z = 0; Z < Height; ++Z)
        {
            const size_t To = size_t(InRect.Z0 + Z) * Resolution + size_t(InRect.X0);
            std::memcpy(InData.GetHeights() + To, &InHeights[size_t(Z) * Width], sizeof(float) * size_t(Width));
            std::memcpy(InData.GetWeights() + To * GameCore::TERRAIN_LAYERS, &InWeights[size_t(Z) * Width * GameCore::TERRAIN_LAYERS],
                        size_t(Width) * GameCore::TERRAIN_LAYERS);
        }
        InData.MarkDirty(InRect);
    }

    Stroke::Stroke(const Target& InTarget, const QString& InLabel)
        : TheTarget(InTarget), Label(InLabel)
    {
    }

    void Stroke::Backup(const GameCore::TerrainRect& InRect)
    {
        const GameCore::TerrainData& Data = *TheTarget.Data;
        const uint32_t Side = Data.GetChunksPerSide();
        if(Side == 0 || InRect.IsEmpty()) { return; }
        const int32_t Quads = int32_t(GameCore::TERRAIN_CHUNK_QUADS);
        const int32_t Last  = int32_t(Side) - 1;
        /* A sample on a shared edge belongs to both chunks: back up both. */
        for(int32_t CZ = std::clamp((InRect.Z0 - 1) / Quads, 0, Last); CZ <= std::clamp(InRect.Z1 / Quads, 0, Last); ++CZ)
        {
            for(int32_t CX = std::clamp((InRect.X0 - 1) / Quads, 0, Last); CX <= std::clamp(InRect.X1 / Quads, 0, Last); ++CX)
            {
                const auto Key = std::make_pair(uint32_t(CX), uint32_t(CZ));
                if(Before.count(Key)) { continue; }
                Chunk& Saved = Before[Key];
                Saved.Rect = Data.GetChunkSamples(uint32_t(CX), uint32_t(CZ));
                CopyRect(Data, Saved.Rect, Saved.Heights, Saved.Weights);
            }
        }
    }

    GameCore::TerrainRect Stroke::Dab(const EngineCore::Vector3& InWorldCentre, const float InWorldRadius,
                                      const GameCore::TerrainBrush& InBrush, const GameCore::TerrainBrushOp& InOp)
    {
        if(bEnded || !TheTarget.IsValid()) { return GameCore::TerrainRect(); }
        const EngineCore::Vector3 Local = TheTarget.ToLocal(InWorldCentre);
        GameCore::TerrainBrush Brush = InBrush;
        Brush.X      = float(Local.GetX());
        Brush.Z      = float(Local.GetZ());
        Brush.Radius = TheTarget.ToLocalLength(InWorldRadius);
        Backup(TheTarget.Data->GetSamplesInRadius(Brush.X, Brush.Z, Brush.Radius));
        const GameCore::TerrainRect Changed = GameCore::ApplyTerrainBrush(*TheTarget.Data, Brush, InOp);
        if(!Changed.IsEmpty()) { MarkDirty(TheTarget.Path); }
        return Changed;
    }

    void Stroke::End(const bool bInCancel)
    {
        if(bEnded) { return; }
        bEnded = true;
        if(Before.empty() || !TheTarget.IsValid()) { return; }

        if(bInCancel)
        {
            for(const auto& Each : Before) { WriteRect(*TheTarget.Data, Each.second.Rect, Each.second.Heights, Each.second.Weights); }
            return;
        }

        std::vector<TerrainSamplesCommand::Patch> Patches;
        Patches.reserve(Before.size());
        for(auto& Each : Before)
        {
            TerrainSamplesCommand::Patch Patch;
            Patch.Rect          = Each.second.Rect;
            Patch.BeforeHeights = std::move(Each.second.Heights);
            Patch.BeforeWeights = std::move(Each.second.Weights);
            CopyRect(*TheTarget.Data, Patch.Rect, Patch.AfterHeights, Patch.AfterWeights);
            if(Patch.AfterHeights == Patch.BeforeHeights && Patch.AfterWeights == Patch.BeforeWeights) { continue; }
            Patches.push_back(std::move(Patch));
        }
        Before.clear();
        RecordPatches(TheTarget, Label + QLatin1Char(' ') + EntityName(TheTarget.Entity), std::move(Patches));
    }

    // === Saving ===============================================================

    void MarkDirty(const std::string& InPath)
    {
        if(!InPath.empty()) { DirtyPaths().insert(InPath); }
    }

    bool IsDirty(const std::string& InPath)
    {
        return DirtyPaths().count(InPath) != 0;
    }

    QString ProjectFileFor(const std::string& InRuntimePath)
    {
        const QString Runtime = QString::fromStdString(InRuntimePath);
        if(Runtime.isEmpty() || Runtime.startsWith(QStringLiteral("Engine/"))) { return QString(); }
        return QDir(EditorGraphs::GetProjectRoot()).filePath(Runtime);
    }

    bool SaveDirty(QStringList* OutErrors)
    {
        bool bAll = true;
        for(auto It = DirtyPaths().begin(); It != DirtyPaths().end();)
        {
            const std::shared_ptr<GameCore::TerrainData> Data = GameCore::TerrainAssetCache::FindLoaded(*It);
            const QString File = ProjectFileFor(*It);
            if(!Data || File.isEmpty())
            {
                It = DirtyPaths().erase(It);
                continue;
            }
            QDir().mkpath(QFileInfo(File).absolutePath());
            if(!GameCore::SaveTerrainFile(File.toStdString(), *Data))
            {
                bAll = false;
                if(OutErrors) { OutErrors->push_back(QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(File))); }
                ++It;
                continue;
            }
            EditorContent::StageAsset(File);
            It = DirtyPaths().erase(It);
        }
        return bAll;
    }

    void DiscardUnsaved()
    {
        for(const std::string& Each : DirtyPaths()) { GameCore::TerrainAssetCache::Invalidate(Each); }
        DirtyPaths().clear();
    }

    // === Heightmaps ===========================================================

    bool ImportHeightmap(const Target& InTarget, const QString& InImage, const float InLow, const float InHigh, QString& OutError)
    {
        if(!InTarget.IsValid()) { OutError = QStringLiteral("No terrain to import into"); return false; }
        QImage Image(InImage);
        if(Image.isNull()) { OutError = QStringLiteral("Cannot read %1").arg(QDir::toNativeSeparators(InImage)); return false; }
        Image = Image.convertToFormat(QImage::Format_Grayscale16);

        GameCore::TerrainData& Data = *InTarget.Data;
        const uint32_t N = Data.GetResolution();
        GameCore::TerrainRect All;
        All.Add(0, 0);
        All.Add(int32_t(N) - 1, int32_t(N) - 1);

        TerrainSamplesCommand::Patch Patch;
        Patch.Rect = All;
        CopyRect(Data, All, Patch.BeforeHeights, Patch.BeforeWeights);
        Patch.AfterHeights.resize(Patch.BeforeHeights.size());
        Patch.AfterWeights = Patch.BeforeWeights;

        /* Image row 0 is the top: the terrain's far edge (+Z). Bilinear. */
        const int W = Image.width(), H = Image.height();
        const auto Grey = [&](const int InX, const int InY)
        {
            const auto* Row = reinterpret_cast<const uint16_t*>(Image.constScanLine(std::clamp(InY, 0, H - 1)));
            return float(Row[std::clamp(InX, 0, W - 1)]) / 65535.0f;
        };
        for(uint32_t Z = 0; Z < N; ++Z)
        {
            for(uint32_t X = 0; X < N; ++X)
            {
                const float U = N > 1 ? float(X) / float(N - 1) * float(W - 1) : 0.0f;
                const float V = N > 1 ? (1.0f - float(Z) / float(N - 1)) * float(H - 1) : 0.0f;
                const int   X0 = int(U), Y0 = int(V);
                const float FX = U - float(X0), FY = V - float(Y0);
                const float Top    = Grey(X0, Y0) + (Grey(X0 + 1, Y0) - Grey(X0, Y0)) * FX;
                const float Bottom = Grey(X0, Y0 + 1) + (Grey(X0 + 1, Y0 + 1) - Grey(X0, Y0 + 1)) * FX;
                Patch.AfterHeights[size_t(Z) * N + X] = InLow + (Top + (Bottom - Top) * FY) * (InHigh - InLow);
            }
        }
        WriteRect(Data, All, Patch.AfterHeights, Patch.AfterWeights);
        MarkDirty(InTarget.Path);
        std::vector<TerrainSamplesCommand::Patch> Patches;
        Patches.push_back(std::move(Patch));
        RecordPatches(InTarget, QStringLiteral("Import Heightmap"), std::move(Patches));
        return true;
    }

    bool ExportHeightmap(const Target& InTarget, const QString& InImage, QString& OutError)
    {
        if(!InTarget.IsValid()) { OutError = QStringLiteral("No terrain to export"); return false; }
        const GameCore::TerrainData& Data = *InTarget.Data;
        std::lock_guard<std::mutex> Guard(Data.GetLock());
        const uint32_t N = Data.GetResolution();
        QImage Image(int(N), int(N), QImage::Format_Grayscale16);
        const float Low = Data.GetMinHeight(), Range = std::max(Data.GetMaxHeight() - Low, 1e-6f);
        for(uint32_t Z = 0; Z < N; ++Z)
        {
            auto* Row = reinterpret_cast<uint16_t*>(Image.scanLine(int(N - 1 - Z)));
            for(uint32_t X = 0; X < N; ++X)
            {
                Row[X] = uint16_t(std::clamp(std::lround((Data.GetHeight(int32_t(X), int32_t(Z)) - Low) / Range * 65535.0f), 0L, 65535L));
            }
        }
        QDir().mkpath(QFileInfo(InImage).absolutePath());
        if(!Image.save(InImage)) { OutError = QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(InImage)); return false; }
        return true;
    }

    // === Console ==============================================================

    QString GetConsoleUsage()
    {
        return QStringLiteral("<create [name] [resolution] [spacing] | select [name] | stats | "
                              "sculpt <Raise|Lower|Smooth|Flatten|Noise|SetHeight> <x> <z> <radius> [strength] [seconds] [height] | "
                              "paint <layer 0-3> <x> <z> <radius> [strength] [seconds] | import <image> <low> <high> | export <image> | "
                              "tool <Raise|Lower|Smooth|Flatten|Noise|SetHeight|Paint|SmoothPaint|off> | show>");
    }

    void RunConsoleCommand(EditorConsole& InConsole, const QStringList& InArgs)
    {
        const QString Verb = InArgs.value(0);
        EditorDocument* Document = TheHost().Document;
        if(Verb.isEmpty() || !Document)
        {
            InConsole.PrintError(QStringLiteral("Usage: terrain ") + GetConsoleUsage());
            return;
        }

        if(Verb == QStringLiteral("create"))
        {
            CreateSettings Settings;
            if(InArgs.size() > 1) { Settings.Name = InArgs[1]; }
            if(InArgs.size() > 2) { Settings.Resolution = InArgs[2].toUInt(); }
            if(InArgs.size() > 3) { Settings.Spacing = InArgs[3].toFloat(); }
            QString Error;
            const GameCore::EntityID Made = CreateTerrain(Settings, EngineCore::Vector3(0.0, 0.0, 0.0), Error);
            if(!Made.IsSet()) { InConsole.PrintError(Error); return; }
            InConsole.Print(QStringLiteral("Created terrain %1").arg(EntityName(Made)));
            return;
        }
        if(Verb == QStringLiteral("select"))
        {
            GameCore::EntityID Found;
            const QString Wanted = InArgs.value(1);
            Document->GetWorld().Each<RenderCore::TerrainComponent>([&](const GameCore::Entity& InEntity, RenderCore::TerrainComponent&)
            {
                if(!Found.IsSet() && (Wanted.isEmpty() || QString::fromStdString(InEntity.GetName()) == Wanted)) { Found = InEntity.GetID(); }
            });
            if(!Found.IsSet()) { InConsole.PrintError(QStringLiteral("No terrain %1").arg(Wanted)); return; }
            Document->SetSelection(Found);
            return;
        }
        if(Verb == QStringLiteral("show"))
        {
            TerrainToolsWindow::Show();
            return;
        }
        if(Verb == QStringLiteral("tool"))
        {
            if(!TerrainToolsWindow::SelectTool(InArgs.value(1))) { InConsole.PrintError(QStringLiteral("Usage: terrain tool <name|off>")); }
            return;
        }

        const Target Edit = FindEditTarget();
        if(!Edit.IsValid())
        {
            InConsole.PrintError(QStringLiteral("Select a terrain first (or have exactly one in the level)"));
            return;
        }
        if(Verb == QStringLiteral("stats"))
        {
            const GameCore::TerrainData& Data = *Edit.Data;
            InConsole.Print(QStringLiteral("%1: %2, %3 x %3 samples %4 apart (%5 a side), heights %6 to %7, %8 chunks, revision %9%10")
                            .arg(EntityName(Edit.Entity), QString::fromStdString(Edit.Path))
                            .arg(Data.GetResolution()).arg(double(Data.GetSpacing())).arg(double(Data.GetSize()))
                            .arg(double(Data.GetMinHeight()), 0, 'f', 1).arg(double(Data.GetMaxHeight()), 0, 'f', 1)
                            .arg(Data.GetChunksPerSide() * Data.GetChunksPerSide()).arg(Data.GetRevision())
                            .arg(IsDirty(Edit.Path) ? QStringLiteral(", unsaved") : QString()));
            return;
        }
        if(Verb == QStringLiteral("export"))
        {
            QString Error;
            const QString File = QFileInfo(InArgs.mid(1).join(QLatin1Char(' '))).absoluteFilePath();
            if(!ExportHeightmap(Edit, File, Error)) { InConsole.PrintError(Error); }
            else { InConsole.Print(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(File))); }
            return;
        }
        if(Verb == QStringLiteral("import"))
        {
            if(InArgs.size() < 4) { InConsole.PrintError(QStringLiteral("Usage: terrain import <image> <low> <high>")); return; }
            QString Error;
            if(!ImportHeightmap(Edit, QFileInfo(InArgs[1]).absoluteFilePath(), InArgs[2].toFloat(), InArgs[3].toFloat(), Error))
            {
                InConsole.PrintError(Error);
            }
            return;
        }
        if(Verb == QStringLiteral("sculpt") || Verb == QStringLiteral("paint"))
        {
            const bool bPaint = Verb == QStringLiteral("paint");
            if(InArgs.size() < 5)
            {
                InConsole.PrintError(QStringLiteral("Usage: terrain ") + GetConsoleUsage());
                return;
            }
            GameCore::TerrainBrushOp Op;
            QString Label;
            if(bPaint)
            {
                Op.Tool  = GameCore::TT_Paint;
                Op.Layer = uint8_t(std::clamp(InArgs[1].toInt(), 0, int(GameCore::TERRAIN_LAYERS) - 1));
                const std::string& Layer = Edit.Data->LayerNames[Op.Layer];
                Label = QStringLiteral("Paint ") + (Layer.empty() ? QStringLiteral("Layer %1").arg(Op.Layer) : QString::fromStdString(Layer));
            }
            else
            {
                QString Name = InArgs[1];
                const bool bLower = Name.compare(QStringLiteral("Lower"), Qt::CaseInsensitive) == 0;
                if(bLower) { Name = QStringLiteral("Raise"); }
                if(Name.compare(QStringLiteral("SetHeight"), Qt::CaseInsensitive) == 0) { Name = QStringLiteral("Set Height"); }
                Op.Tool = GameCore::FindTerrainTool(Name.toUtf8().constData());
                if(Op.Tool >= GameCore::TT_Count || Op.Tool == GameCore::TT_Paint || Op.Tool == GameCore::TT_SmoothWeights)
                {
                    InConsole.PrintError(QStringLiteral("Unknown sculpt tool %1").arg(InArgs[1]));
                    return;
                }
                if(bLower) { Op.Rate = -Op.Rate; }
                Label = QStringLiteral("Sculpt ") + (bLower ? QStringLiteral("Lower") : Name);
            }
            const double X = InArgs[2].toDouble(), Z = InArgs[3].toDouble();
            const float Radius   = InArgs[4].toFloat();
            GameCore::TerrainBrush Brush;
            Brush.Strength = InArgs.size() > 5 ? InArgs[5].toFloat() : 0.5f;
            const float Seconds = InArgs.size() > 6 ? InArgs[6].toFloat() : 0.5f;

            /* The centre on the ground: from well above the point, straight down. */
            EngineCore::Vector3 Centre(X, 0.0, Z);
            float Ground = 0.0f;
            if(NimbleEngine::SampleGround(Document->GetWorld(), Centre, Ground)) { Centre = EngineCore::Vector3(X, double(Ground), Z); }
            const EngineCore::Vector3 Local = Edit.ToLocal(Centre);
            Op.TargetHeight = InArgs.size() > 7 ? float(Edit.ToLocal(EngineCore::Vector3(X, InArgs[7].toDouble(), Z)).GetY())
                                                : Edit.Data->SampleHeight(float(Local.GetX()), float(Local.GetZ()));
            Stroke Strokes(Edit, Label);
            const int Dabs = std::max(1, int(std::lround(double(Seconds) * 60.0)));
            Op.DeltaSeconds = Seconds / float(Dabs);
            for(int Each = 0; Each < Dabs; ++Each) { Strokes.Dab(Centre, Radius, Brush, Op); }
            Strokes.End(false);
            InConsole.Print(QStringLiteral("%1 at %2, %3 (radius %4, %5 dabs)").arg(Label).arg(X).arg(Z).arg(double(Radius)).arg(Dabs));
            return;
        }
        InConsole.PrintError(QStringLiteral("Usage: terrain ") + GetConsoleUsage());
    }
}

#endif // WITH_EDITOR
