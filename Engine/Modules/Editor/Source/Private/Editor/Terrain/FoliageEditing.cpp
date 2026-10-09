#if WITH_EDITOR

#include "Private/Editor/Terrain/FoliageEditing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include "Public/Core/CoreLog.h"
#include "Public/Editor/EditorConsole.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/RenderCore/Components/TerrainComponent.h"
#include "Public/RenderCore/MeshAsset.h"

#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/EditorHistory.h"

namespace EditorFoliage
{
    namespace
    {
        using Buckets = std::vector<std::vector<GameCore::FoliageInstance>>;

        /* The data a command changed, if the cache still holds it; else null, warned. */
        std::shared_ptr<GameCore::FoliageData> Live(const std::string& InPath, const std::shared_ptr<GameCore::FoliageData>& InData)
        {
            std::shared_ptr<GameCore::FoliageData> Now = GameCore::FoliageAssetCache::FindLoaded(InPath);
            if(!Now || Now != InData)
            {
                CORE_LOG_WARN("[Foliage] '" + InPath + "' was reloaded since this edit; its undo step does nothing")
                return nullptr;
            }
            return Now;
        }

        /* Before/after instances of the buckets a stroke touched. */
        class FoliageBucketsCommand final : public EditorHistory::Command
        {
        public:
            struct Patch
            {
                uint32_t X = 0, Z = 0;
                std::vector<GameCore::FoliageInstance> Before, After;
            };

            FoliageBucketsCommand(EditorDocument& InDocument, const QString& InText, std::string InPath,
                                  std::shared_ptr<GameCore::FoliageData> InData, std::vector<Patch> InPatches)
                : Command(InDocument, InText), Path(std::move(InPath)), Data(std::move(InData)), Patches(std::move(InPatches))
            {
            }

        protected:
            void Apply(const bool bInForward) override
            {
                const std::shared_ptr<GameCore::FoliageData> Target = Live(Path, Data);
                if(!Target) { return; }
                std::lock_guard<std::mutex> Guard(Target->GetLock());
                for(const Patch& Each : Patches)
                {
                    if(std::vector<GameCore::FoliageInstance>* Bucket = Target->FindBucket(Each.X, Each.Z))
                    {
                        *Bucket = bInForward ? Each.After : Each.Before;
                        Target->MarkBucketDirty(Each.X, Each.Z);
                    }
                }
                EditorTerrain::MarkDirty(Path);
            }

        private:
            std::string                            Path;
            std::shared_ptr<GameCore::FoliageData> Data;
            std::vector<Patch>                     Patches;
        };

        /* Before/after types (and, when they changed, every bucket): adding, editing, removing a type. */
        class FoliageTypesCommand final : public EditorHistory::Command
        {
        public:
            enum { ID = 0x464F4C49 };   // "FOLI"

            FoliageTypesCommand(EditorDocument& InDocument, const QString& InText, std::string InPath,
                                std::shared_ptr<GameCore::FoliageData> InData, std::vector<GameCore::FoliageType> InBefore,
                                std::vector<GameCore::FoliageType> InAfter, Buckets InBucketsBefore, Buckets InBucketsAfter,
                                const int InMergeType)
                : Command(InDocument, InText), Path(std::move(InPath)), Data(std::move(InData)), Before(std::move(InBefore)),
                  After(std::move(InAfter)), BucketsBefore(std::move(InBucketsBefore)), BucketsAfter(std::move(InBucketsAfter)),
                  MergeType(InMergeType), TimeMs(QDateTime::currentMSecsSinceEpoch())
            {
            }

            int id() const override { return MergeType >= 0 ? ID : -1; }

            bool mergeWith(const QUndoCommand* InOther) override
            {
                const auto* Other = dynamic_cast<const FoliageTypesCommand*>(InOther);
                if(!Other || Other->MergeType != MergeType || Other->Path != Path || Other->Data != Data
                   || Other->TimeMs - TimeMs > 1000 || !Document.IsLatestRecorded(this))
                {
                    return false;
                }
                After  = Other->After;
                TimeMs = Other->TimeMs;
                return true;
            }

        protected:
            void Apply(const bool bInForward) override
            {
                const std::shared_ptr<GameCore::FoliageData> Target = Live(Path, Data);
                if(!Target) { return; }
                std::lock_guard<std::mutex> Guard(Target->GetLock());
                Target->GetTypes() = bInForward ? After : Before;
                const Buckets& Instances = bInForward ? BucketsAfter : BucketsBefore;
                const uint32_t Side = Target->GetBucketsPerSide();
                if(Instances.size() == size_t(Side) * Side)
                {
                    for(uint32_t Z = 0; Z < Side; ++Z)
                    {
                        for(uint32_t X = 0; X < Side; ++X) { *Target->FindBucket(X, Z) = Instances[size_t(Z) * Side + X]; }
                    }
                }
                Target->MarkAllDirty();
                EditorTerrain::MarkDirty(Path);
            }

        private:
            std::string                            Path;
            std::shared_ptr<GameCore::FoliageData> Data;
            std::vector<GameCore::FoliageType>     Before, After;
            Buckets                                BucketsBefore, BucketsAfter;   // empty: unchanged
            int                                    MergeType;   // -1: never merges
            qint64                                 TimeMs;
        };

        Buckets CopyBuckets(const GameCore::FoliageData& InData)
        {
            Buckets Out;
            const uint32_t Side = InData.GetBucketsPerSide();
            Out.reserve(size_t(Side) * Side);
            for(uint32_t Z = 0; Z < Side; ++Z)
            {
                for(uint32_t X = 0; X < Side; ++X) { Out.push_back(InData.GetBucket(X, Z)); }
            }
            return Out;
        }

        bool Record(QUndoCommand* InCommand)
        {
            EditorDocument* Document = EditorTerrain::GetHost().Document;
            if(!Document) { delete InCommand; return false; }
            return Document->RecordToolCommand(InCommand);
        }

        QString EntityName(const GameCore::EntityID InEntity)
        {
            EditorDocument* Document = EditorTerrain::GetHost().Document;
            return Document ? QString::fromStdString(Document->GetWorld().GetEntity(InEntity).GetName()) : QString();
        }
    }

    // === Finding and making =====================================================

    Target FindFoliage(const EditorTerrain::Target& InTerrain)
    {
        Target Out;
        EditorDocument* Document = EditorTerrain::GetHost().Document;
        if(!InTerrain.IsValid() || !Document) { return Out; }
        const auto* Component = Document->GetWorld().GetEntity(InTerrain.Entity).GetComponent<RenderCore::TerrainComponent>();
        if(!Component || Component->Foliage[0] == '\0') { return Out; }
        Out.Terrain = InTerrain;
        Out.Path    = Component->Foliage;
        Out.Data    = GameCore::FoliageAssetCache::Find(Out.Path);
        return Out;
    }

    Target EnsureFoliage(const EditorTerrain::Target& InTerrain, QString& OutError)
    {
        Target Found = FindFoliage(InTerrain);
        if(Found.IsValid()) { return Found; }
        EditorDocument* Document = EditorTerrain::GetHost().Document;
        if(!InTerrain.IsValid() || !Document)
        {
            OutError = QStringLiteral("Select a terrain first");
            return Target();
        }
        if(Document->IsWorldBorrowed())
        {
            OutError = QStringLiteral("Stop playing first");
            return Target();
        }

        /* Beside the terrain: Ground.nmterrain -> Ground.nmfoliage. */
        std::string Path = InTerrain.Path;
        const size_t Dot = Path.rfind(".nmterrain");
        Path = (Dot != std::string::npos ? Path.substr(0, Dot) : Path) + ".nmfoliage";
        const QString File = EditorTerrain::ProjectFileFor(Path);
        if(File.isEmpty())
        {
            OutError = QStringLiteral("The terrain is not in the project");
            return Target();
        }

        std::shared_ptr<GameCore::FoliageData> Data = QFileInfo::exists(File) ? GameCore::FoliageAssetCache::Find(Path) : nullptr;
        if(!Data)
        {
            Data = std::make_shared<GameCore::FoliageData>();
            Data->Reset(*InTerrain.Data);
            QDir().mkpath(QFileInfo(File).absolutePath());
            if(!GameCore::SaveFoliageFile(File.toStdString(), *Data))
            {
                OutError = QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(File));
                return Target();
            }
            EditorContent::StageAsset(File);
            GameCore::FoliageAssetCache::Store(Path, Data);
        }

        /* Named in the component, as a Details edit would: selected first, so the change is recorded. */
        Document->SetSelection(InTerrain.Entity);
        if(auto* Component = Document->GetWorld().GetEntity(InTerrain.Entity).GetComponent<RenderCore::TerrainComponent>())
        {
            std::snprintf(Component->Foliage, sizeof(Component->Foliage), "%s", Path.c_str());
            Document->NotifyPropertyEdited(InTerrain.Entity, RenderCore::TerrainComponent::StaticType().ID);
        }
        return FindFoliage(InTerrain);
    }

    // === Types ==================================================================

    int AddType(const Target& InTarget, const QString& InMeshPath, const QString& InName, QString& OutError)
    {
        if(!InTarget.IsValid()) { OutError = QStringLiteral("No foliage to add to"); return -1; }
        const std::string Mesh = InMeshPath.toStdString();
        if(Mesh.empty() || !RenderCore::MeshAssetCache::Find(Mesh))
        {
            OutError = QStringLiteral("%1 is not a mesh that loads").arg(InMeshPath);
            return -1;
        }
        std::vector<GameCore::FoliageType> Before, After;
        {
            std::lock_guard<std::mutex> Guard(InTarget.Data->GetLock());
            Before = InTarget.Data->GetTypes();
            if(Before.size() >= GameCore::FOLIAGE_TYPES_MAX) { OutError = QStringLiteral("Too many foliage types"); return -1; }
            GameCore::FoliageType Made;
            Made.Name    = InName.trimmed().isEmpty() ? QFileInfo(InMeshPath).completeBaseName().toStdString() : InName.trimmed().toStdString();
            Made.Mesh[0] = Mesh;
            After = Before;
            After.push_back(Made);
            InTarget.Data->GetTypes() = After;
            InTarget.Data->MarkAllDirty();
        }
        EditorTerrain::MarkDirty(InTarget.Path);
        Record(new FoliageTypesCommand(*EditorTerrain::GetHost().Document, QStringLiteral("Add Foliage %1").arg(QString::fromStdString(After.back().Name)),
                                       InTarget.Path, InTarget.Data, Before, After, {}, {}, -1));
        return int(After.size()) - 1;
    }

    bool SetType(const Target& InTarget, const uint16_t InType, const GameCore::FoliageType& InValue)
    {
        if(!InTarget.IsValid()) { return false; }
        std::vector<GameCore::FoliageType> Before, After;
        {
            std::lock_guard<std::mutex> Guard(InTarget.Data->GetLock());
            Before = InTarget.Data->GetTypes();
            if(InType >= Before.size()) { return false; }
            After = Before;
            After[InType] = InValue;
            InTarget.Data->GetTypes() = After;
            InTarget.Data->MarkAllDirty();
        }
        EditorTerrain::MarkDirty(InTarget.Path);
        Record(new FoliageTypesCommand(*EditorTerrain::GetHost().Document, QStringLiteral("Edit Foliage %1").arg(QString::fromStdString(InValue.Name)),
                                       InTarget.Path, InTarget.Data, Before, After, {}, {}, int(InType)));
        return true;
    }

    bool RemoveType(const Target& InTarget, const uint16_t InType)
    {
        if(!InTarget.IsValid()) { return false; }
        std::vector<GameCore::FoliageType> Before, After;
        Buckets BucketsBefore, BucketsAfter;
        QString Name;
        {
            std::lock_guard<std::mutex> Guard(InTarget.Data->GetLock());
            if(InType >= InTarget.Data->GetTypes().size()) { return false; }
            Before        = InTarget.Data->GetTypes();
            BucketsBefore = CopyBuckets(*InTarget.Data);
            Name          = QString::fromStdString(Before[InType].Name);
            InTarget.Data->RemoveType(InType);
            After         = InTarget.Data->GetTypes();
            BucketsAfter  = CopyBuckets(*InTarget.Data);
        }
        EditorTerrain::MarkDirty(InTarget.Path);
        Record(new FoliageTypesCommand(*EditorTerrain::GetHost().Document, QStringLiteral("Remove Foliage %1").arg(Name),
                                       InTarget.Path, InTarget.Data, Before, After, std::move(BucketsBefore), std::move(BucketsAfter), -1));
        return true;
    }

    // === Strokes ================================================================

    Stroke::Stroke(const Target& InTarget, const QString& InLabel)
        : TheTarget(InTarget), Label(InLabel)
    {
    }

    GameCore::FoliageBrushResult Stroke::Dab(const EngineCore::Vector3& InWorldCentre, const float InWorldRadius,
                                             const GameCore::TerrainBrush& InBrush, GameCore::FoliageBrushOp& InOutOp)
    {
        GameCore::FoliageBrushResult Result;
        if(bEnded || !TheTarget.IsValid()) { return Result; }
        const EngineCore::Vector3 Local = TheTarget.Terrain.ToLocal(InWorldCentre);
        GameCore::TerrainBrush Brush = InBrush;
        Brush.X      = float(Local.GetX());
        Brush.Z      = float(Local.GetZ());
        Brush.Radius = TheTarget.Terrain.ToLocalLength(InWorldRadius);

        /* Every bucket the brush, or a paint's spacing check, may change: backed up first. */
        {
            std::lock_guard<std::mutex> Guard(TheTarget.Data->GetLock());
            const float    Size = TheTarget.Data->GetBucketSize();
            const int32_t  Last = int32_t(TheTarget.Data->GetBucketsPerSide()) - 1;
            const auto Clamp = [&](const float InValue) { return std::clamp(int32_t(std::floor(InValue / Size)), 0, std::max(Last, 0)); };
            for(int32_t Z = Clamp(Brush.Z - Brush.Radius); Z <= Clamp(Brush.Z + Brush.Radius) && Last >= 0; ++Z)
            {
                for(int32_t X = Clamp(Brush.X - Brush.Radius); X <= Clamp(Brush.X + Brush.Radius); ++X)
                {
                    const auto Key = std::make_pair(uint32_t(X), uint32_t(Z));
                    if(!Before.count(Key)) { Before[Key] = TheTarget.Data->GetBucket(uint32_t(X), uint32_t(Z)); }
                }
            }
        }
        Result = GameCore::ApplyFoliageBrush(*TheTarget.Data, *TheTarget.Terrain.Data, Brush, InOutOp);
        if(Result.Added || Result.Removed) { EditorTerrain::MarkDirty(TheTarget.Path); }
        return Result;
    }

    void Stroke::End(const bool bInCancel)
    {
        if(bEnded) { return; }
        bEnded = true;
        if(Before.empty() || !TheTarget.IsValid()) { return; }

        std::vector<FoliageBucketsCommand::Patch> Patches;
        {
            std::lock_guard<std::mutex> Guard(TheTarget.Data->GetLock());
            for(auto& Each : Before)
            {
                std::vector<GameCore::FoliageInstance>* Bucket = TheTarget.Data->FindBucket(Each.first.first, Each.first.second);
                if(!Bucket) { continue; }
                if(bInCancel)
                {
                    *Bucket = Each.second;
                    TheTarget.Data->MarkBucketDirty(Each.first.first, Each.first.second);
                    continue;
                }
                if(Bucket->size() == Each.second.size()
                   && std::equal(Bucket->begin(), Bucket->end(), Each.second.begin(), [](const GameCore::FoliageInstance& A, const GameCore::FoliageInstance& B)
                      { return A.Seed == B.Seed && A.Type == B.Type && A.X == B.X && A.Z == B.Z; }))
                {
                    continue;   // untouched
                }
                FoliageBucketsCommand::Patch Patch;
                Patch.X      = Each.first.first;
                Patch.Z      = Each.first.second;
                Patch.Before = std::move(Each.second);
                Patch.After  = *Bucket;
                Patches.push_back(std::move(Patch));
            }
        }
        Before.clear();
        if(bInCancel || Patches.empty()) { return; }
        Record(new FoliageBucketsCommand(*EditorTerrain::GetHost().Document, Label + QLatin1Char(' ') + EntityName(TheTarget.Terrain.Entity),
                                         TheTarget.Path, TheTarget.Data, std::move(Patches)));
    }

    // === Console ================================================================

    bool SetTypeField(GameCore::FoliageType& InOutType, const QString& InField, const QString& InValue)
    {
        bool bOk = true;
        const float Number = InValue.toFloat(&bOk);
        const QString Field = InField.toLower();
        const auto Float = [&](float& OutValue) { if(bOk) { OutValue = Number; } return bOk; };
        if(Field == QStringLiteral("name"))     { InOutType.Name = InValue.toStdString(); return true; }
        if(Field == QStringLiteral("mesh"))     { InOutType.Mesh[0] = InValue.toStdString(); return true; }
        if(Field == QStringLiteral("mesh1") || Field == QStringLiteral("meshlod1")) { InOutType.Mesh[1] = InValue.toStdString(); return true; }
        if(Field == QStringLiteral("mesh2") || Field == QStringLiteral("meshlod2")) { InOutType.Mesh[2] = InValue.toStdString(); return true; }
        if(Field == QStringLiteral("material")) { InOutType.Material = InValue.toStdString(); return true; }
        if(Field == QStringLiteral("spacing"))        { return Float(InOutType.Spacing); }
        if(Field == QStringLiteral("scalemin"))       { return Float(InOutType.ScaleMin); }
        if(Field == QStringLiteral("scalemax"))       { return Float(InOutType.ScaleMax); }
        if(Field == QStringLiteral("aligntonormal"))  { return Float(InOutType.AlignToNormal); }
        if(Field == QStringLiteral("maxslope"))       { return Float(InOutType.MaxSlope); }
        if(Field == QStringLiteral("sink"))           { return Float(InOutType.Sink); }
        if(Field == QStringLiteral("culldistance"))   { return Float(InOutType.CullDistance); }
        if(Field == QStringLiteral("lod1density"))    { return Float(InOutType.LODDensity[1]); }
        if(Field == QStringLiteral("lod2density"))    { return Float(InOutType.LODDensity[2]); }
        if(Field == QStringLiteral("colorvariation")) { return Float(InOutType.ColorVariation); }
        if(Field == QStringLiteral("castshadows"))    { InOutType.bCastShadows = (InValue == QStringLiteral("1") || InValue.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) ? 1 : 0; return true; }
        if(Field == QStringLiteral("randomyaw"))      { InOutType.bRandomYaw = (InValue == QStringLiteral("1") || InValue.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) ? 1 : 0; return true; }
        return false;
    }

    QString GetConsoleUsage()
    {
        return QStringLiteral("<add <mesh> [name] [spacing] | set <type> <field> <value> | remove <type> | "
                              "paint <x> <z> <radius> [strength] [seconds] [type,...] | erase <x> <z> <radius> [strength] [seconds] [type,...] | "
                              "clear [type] | stats>");
    }

    void RunConsoleCommand(EditorConsole& InConsole, const QStringList& InArgs)
    {
        const QString Verb = InArgs.value(0);
        EditorDocument* Document = EditorTerrain::GetHost().Document;
        const EditorTerrain::Target Terrain = EditorTerrain::FindEditTarget();
        if(Verb.isEmpty() || !Document)
        {
            InConsole.PrintError(QStringLiteral("Usage: foliage ") + GetConsoleUsage());
            return;
        }
        if(!Terrain.IsValid())
        {
            InConsole.PrintError(QStringLiteral("Select a terrain first (or have exactly one in the level)"));
            return;
        }
        if(Document->IsWorldBorrowed())
        {
            InConsole.PrintError(QStringLiteral("Not while playing; 'stop' first"));
            return;
        }
        QString Error;
        const Target Foliage = Verb == QStringLiteral("stats") ? FindFoliage(Terrain) : EnsureFoliage(Terrain, Error);
        if(!Foliage.IsValid())
        {
            InConsole.PrintError(Error.isEmpty() ? QStringLiteral("This terrain has no foliage yet") : Error);
            return;
        }

        /* A type by index or name. */
        const auto FindType = [&](const QString& InWhich) -> int
        {
            std::lock_guard<std::mutex> Guard(Foliage.Data->GetLock());
            const std::vector<GameCore::FoliageType>& Types = Foliage.Data->GetTypes();
            bool bNumber = false;
            const int Index = InWhich.toInt(&bNumber);
            if(bNumber) { return Index >= 0 && size_t(Index) < Types.size() ? Index : -1; }
            for(size_t Each = 0; Each < Types.size(); ++Each)
            {
                if(QString::fromStdString(Types[Each].Name).compare(InWhich, Qt::CaseInsensitive) == 0) { return int(Each); }
            }
            return -1;
        };

        if(Verb == QStringLiteral("stats"))
        {
            std::lock_guard<std::mutex> Guard(Foliage.Data->GetLock());
            InConsole.Print(QStringLiteral("%1: %2 instances in %3 x %3 buckets%4").arg(QString::fromStdString(Foliage.Path))
                            .arg(qulonglong(Foliage.Data->GetInstanceCount())).arg(Foliage.Data->GetBucketsPerSide())
                            .arg(EditorTerrain::IsDirty(Foliage.Path) ? QStringLiteral(", unsaved") : QString()));
            const std::vector<GameCore::FoliageType>& Types = Foliage.Data->GetTypes();
            for(size_t Each = 0; Each < Types.size(); ++Each)
            {
                InConsole.Print(QStringLiteral("  %1 %2: %3 instances, %4, spacing %5")
                                .arg(Each).arg(QString::fromStdString(Types[Each].Name)).arg(qulonglong(Foliage.Data->GetInstanceCount(uint16_t(Each))))
                                .arg(QString::fromStdString(Types[Each].Mesh[0])).arg(double(Types[Each].Spacing)));
            }
            return;
        }
        if(Verb == QStringLiteral("add"))
        {
            if(InArgs.size() < 2) { InConsole.PrintError(QStringLiteral("Usage: foliage add <mesh> [name] [spacing]")); return; }
            const int Made = AddType(Foliage, InArgs[1], InArgs.value(2), Error);
            if(Made < 0) { InConsole.PrintError(Error); return; }
            if(InArgs.size() > 3)
            {
                GameCore::FoliageType Type = Foliage.Data->GetTypes()[size_t(Made)];
                if(SetTypeField(Type, QStringLiteral("Spacing"), InArgs[3])) { SetType(Foliage, uint16_t(Made), Type); }
            }
            InConsole.Print(QStringLiteral("Added foliage type %1").arg(Made));
            return;
        }
        if(Verb == QStringLiteral("set"))
        {
            const int Type = FindType(InArgs.value(1));
            if(Type < 0 || InArgs.size() < 4) { InConsole.PrintError(QStringLiteral("Usage: foliage set <type> <field> <value>")); return; }
            GameCore::FoliageType Value = Foliage.Data->GetTypes()[size_t(Type)];
            if(!SetTypeField(Value, InArgs[2], InArgs.mid(3).join(QLatin1Char(' '))))
            {
                InConsole.PrintError(QStringLiteral("Unknown field or bad value: %1").arg(InArgs[2]));
                return;
            }
            SetType(Foliage, uint16_t(Type), Value);
            return;
        }
        if(Verb == QStringLiteral("remove"))
        {
            const int Type = FindType(InArgs.value(1));
            if(Type < 0 || !RemoveType(Foliage, uint16_t(Type))) { InConsole.PrintError(QStringLiteral("No foliage type %1").arg(InArgs.value(1))); }
            return;
        }
        if(Verb == QStringLiteral("clear"))
        {
            /* An erase over the whole terrain, every instance at once. */
            Stroke Clearing(Foliage, QStringLiteral("Clear Foliage"));
            GameCore::FoliageBrushOp Op;
            Op.Tool = GameCore::FT_Erase;
            Op.DeltaSeconds = 1.0f;
            if(InArgs.size() > 1)
            {
                const int Type = FindType(InArgs[1]);
                if(Type < 0) { InConsole.PrintError(QStringLiteral("No foliage type %1").arg(InArgs[1])); return; }
                Op.Types.push_back(uint16_t(Type));
            }
            GameCore::TerrainBrush Brush;
            Brush.Strength = 1.0f;
            Brush.Falloff  = GameCore::TF_Sphere;
            const float Size = Terrain.Data->GetSize();
            const EngineCore::Vector3 Centre = Terrain.ToWorld(EngineCore::Vector3(Size * 0.5, 0.0, Size * 0.5));
            /* Past the corners (0.71 of the side from the middle), in world units. */
            const float Radius = Size * 0.75f / std::max(Terrain.ToLocalLength(1.0f), 1e-6f);
            uint32_t Removed = 0;
            for(int Pass = 0; Pass < 4; ++Pass) { Removed += Clearing.Dab(Centre, Radius, Brush, Op).Removed; }
            Clearing.End(false);
            InConsole.Print(QStringLiteral("Removed %1 instances").arg(Removed));
            return;
        }
        if(Verb == QStringLiteral("paint") || Verb == QStringLiteral("erase"))
        {
            if(InArgs.size() < 4) { InConsole.PrintError(QStringLiteral("Usage: foliage ") + GetConsoleUsage()); return; }
            GameCore::FoliageBrushOp Op;
            Op.Tool = Verb == QStringLiteral("paint") ? GameCore::FT_Paint : GameCore::FT_Erase;
            for(const QString& Which : InArgs.value(6).split(QLatin1Char(','), Qt::SkipEmptyParts))
            {
                const int Type = FindType(Which);
                if(Type < 0) { InConsole.PrintError(QStringLiteral("No foliage type %1").arg(Which)); return; }
                Op.Types.push_back(uint16_t(Type));
            }
            const double X = InArgs[1].toDouble(), Z = InArgs[2].toDouble();
            GameCore::TerrainBrush Brush;
            Brush.Strength = InArgs.size() > 4 ? InArgs[4].toFloat() : 0.5f;
            const float Seconds = InArgs.size() > 5 ? InArgs[5].toFloat() : 1.0f;
            const int   Dabs    = std::max(1, int(std::lround(double(Seconds) * 60.0)));
            Op.DeltaSeconds = Seconds / float(Dabs);
            Op.Seed = uint32_t(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF) | 1u;
            Stroke Painting(Foliage, Op.Tool == GameCore::FT_Paint ? QStringLiteral("Paint Foliage") : QStringLiteral("Erase Foliage"));
            uint32_t Added = 0, Removed = 0;
            for(int Each = 0; Each < Dabs; ++Each)
            {
                const GameCore::FoliageBrushResult Result = Painting.Dab(EngineCore::Vector3(X, 0.0, Z), InArgs[3].toFloat(), Brush, Op);
                Added   += Result.Added;
                Removed += Result.Removed;
            }
            Painting.End(false);
            InConsole.Print(QStringLiteral("%1 at %2, %3: %4 added, %5 removed").arg(Verb).arg(X).arg(Z).arg(Added).arg(Removed));
            return;
        }
        InConsole.PrintError(QStringLiteral("Usage: foliage ") + GetConsoleUsage());
    }
}

#endif // WITH_EDITOR
