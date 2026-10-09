#ifndef NIMBLEEDITOR_FOLIAGEEDITING_H
#define NIMBLEEDITOR_FOLIAGEEDITING_H

#if WITH_EDITOR

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "Public/GameCore/Foliage.h"
#include "Public/Math/Vector.h"

#include "Private/Editor/Terrain/TerrainEditing.h"

class EditorConsole;

/*
 * The editor's side of foliage (GameCore's Foliage.h): a terrain's foliage
 * file made on first use, its types edited and its instances painted, each
 * change one undo step, written with the level as terrain is
 * (EditorTerrain::MarkDirty / SaveDirty).
 */
namespace EditorFoliage
{
    /* A terrain and its foliage. */
    struct Target
    {
        EditorTerrain::Target                  Terrain;
        std::string                            Path;   // the .nmfoliage's runtime path
        std::shared_ptr<GameCore::FoliageData> Data;

        bool IsValid() const { return Terrain.IsValid() && Data != nullptr; }
    };

    /* InTerrain's foliage, if its Terrain component names one that loads. */
    Target FindFoliage(const EditorTerrain::Target& InTerrain);

    /* The same, making the foliage first if it has none: a .nmfoliage beside
     * the .nmterrain, named in the Terrain component's Foliage (one undo step
     * with the entity selected). Invalid (OutError) if it cannot be written. */
    Target EnsureFoliage(const EditorTerrain::Target& InTerrain, QString& OutError);

    // === Types ==============================================================

    /* A type for InMeshPath (a runtime .nmmesh path), named InName (else the
     * mesh's), with the defaults; one undo step. Returns its index; -1 (OutError) if not. */
    int  AddType(const Target& InTarget, const QString& InMeshPath, const QString& InName, QString& OutError);

    /* InType replaced by InValue (its instances keep their places); one undo step, merged with
     * the next edit of the same type within a second (a spin box held down). */
    bool SetType(const Target& InTarget, uint16_t InType, const GameCore::FoliageType& InValue);

    /* InType and its instances gone, the types after it renumbered; one undo step. */
    bool RemoveType(const Target& InTarget, uint16_t InType);

    // === Strokes ============================================================

    /* As EditorTerrain::Stroke, for foliage: buckets backed up before their
     * first change; End records what changed as one step ("Paint Foliage Grass"). */
    class Stroke
    {
    public:
        Stroke(const Target& InTarget, const QString& InLabel);

        /* One dab at InWorldCentre, InWorldRadius wide. */
        GameCore::FoliageBrushResult Dab(const EngineCore::Vector3& InWorldCentre, float InWorldRadius,
                                         const GameCore::TerrainBrush& InBrush, GameCore::FoliageBrushOp& InOutOp);

        /* Records it (if it changed anything); bInCancel puts it back instead. */
        void End(bool bInCancel);

        const Target& GetTarget() const { return TheTarget; }

    private:
        Target  TheTarget;
        QString Label;
        std::map<std::pair<uint32_t, uint32_t>, std::vector<GameCore::FoliageInstance>> Before;
        bool    bEnded = false;
    };

    /* "foliage ...": add, set, remove, paint, erase, clear, stats. */
    void RunConsoleCommand(EditorConsole& InConsole, const QStringList& InArgs);
    QString GetConsoleUsage();

    /* A type's fields by console name ("Spacing", "CullDistance", "LOD1Density"...);
     * false for an unknown field or bad value. */
    bool SetTypeField(GameCore::FoliageType& InOutType, const QString& InField, const QString& InValue);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_FOLIAGEEDITING_H
