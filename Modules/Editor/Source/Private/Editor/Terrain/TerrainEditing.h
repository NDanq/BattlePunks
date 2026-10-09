#ifndef NIMBLEEDITOR_TERRAINEDITING_H
#define NIMBLEEDITOR_TERRAINEDITING_H

#if WITH_EDITOR

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/TerrainBrush.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

class EditorConsole;
class EditorContext;
class EditorDocument;
class ViewportWidget;

/*
 * The editor's side of terrain (GameCore's Terrain.h, RenderCore's Terrain
 * component): finding the terrain to edit, making one, strokes as undo
 * steps, and writing edited heightfields when the level is saved.
 *
 * WHERE THE DATA LIVES. A terrain's samples are TerrainAssetCache's one copy,
 * which brushes write and the viewport, physics and Play read. The file
 * (Content/Levels/<World>/Terrain/<Name>.nmterrain) is written only when the
 * level is saved (AboutToSaveWorld) -- an edited terrain is "dirty" until
 * then, its stroke in the level's history like any other edit, so the
 * title's * and Save cover it. Opening or making another level drops unsaved
 * edits (the next use reloads the file).
 */
namespace EditorTerrain
{
    /* What the module edits through: the level's document and context, the
     * main viewport, and where the level's file is. Set by the main window. */
    struct Host
    {
        EditorContext*            Context  = nullptr;
        EditorDocument*           Document = nullptr;
        ViewportWidget*           Viewport = nullptr;
        std::function<QString()>  GetWorldFile;   // absolute .nmworld; empty if unsaved
    };
    void SetHost(const Host& InHost);
    const Host& GetHost();

    /* A terrain entity of the edited World and its data. */
    struct Target
    {
        GameCore::EntityID                     Entity;
        std::string                            Path;   // runtime path
        std::shared_ptr<GameCore::TerrainData> Data;
        EngineCore::Matrix4x4                  LocalToWorld;
        EngineCore::Matrix4x4                  WorldToLocal;

        bool IsValid() const { return Entity.IsSet() && Data != nullptr; }

        /* World <-> terrain space. */
        EngineCore::Vector3 ToLocal(const EngineCore::Vector3& InWorld) const;
        EngineCore::Vector3 ToWorld(const EngineCore::Vector3& InLocal) const;
        /* World units -> terrain units on the ground (the X/Z scales' mean). */
        float ToLocalLength(float InWorld) const;
    };

    /* InEntity's terrain, if it has one that loads. */
    Target FindTarget(GameCore::World& InWorld, GameCore::EntityID InEntity);

    /* What the tools edit: the selected entity's terrain, else the edited
     * World's only one. Invalid if neither. */
    Target FindEditTarget();

    /* The terrain under a world ray, and where: the nearest visible one. */
    bool PickTerrain(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection,
                     Target& OutTarget, EngineCore::Vector3& OutHit);

    // === Making one =========================================================

    struct CreateSettings
    {
        QString  Name       = QStringLiteral("Terrain");
        uint32_t Resolution = 257;      // samples a side
        float    Spacing    = 100.0f;   // world units between samples
        float    Height     = 0.0f;
        bool     bCentred   = true;     // its middle at InAt, else its corner
        bool     bCollision = true;     // a Static Rigid Body (physics' heightfield)
    };

    /* Where a new terrain named InName goes for the level: its runtime path
     * and project file. False (OutError) if the level has never been saved,
     * or the name is taken. */
    bool TerrainPathFor(const QString& InName, QString& OutRuntime, QString& OutFile, QString& OutError);

    /* A flat terrain at InAt and its entity (Terrain, Transform, and a Static
     * Rigid Body if asked), one undo step, selected. Its file is written now
     * and again whenever the level is saved. Unset (OutError) on failure. */
    GameCore::EntityID CreateTerrain(const CreateSettings& InSettings, const EngineCore::Vector3& InAt, QString& OutError);

    // === Strokes ============================================================

    /*
     * One stroke: the chunks it touches are copied before their first dab,
     * so End records exactly what changed as one undo step ("Sculpt Raise
     * Ground", "Paint Rock Ground"). Dabs go through GameCore's
     * ApplyTerrainBrush, in the terrain's own space.
     */
    class Stroke
    {
    public:
        explicit Stroke(const Target& InTarget, const QString& InLabel);

        /* One dab centred on InWorldCentre (a point on, or above, the
         * terrain), InWorldRadius wide. Returns the samples changed. */
        GameCore::TerrainRect Dab(const EngineCore::Vector3& InWorldCentre, float InWorldRadius,
                                  const GameCore::TerrainBrush& InBrush, const GameCore::TerrainBrushOp& InOp);

        /* Records the stroke (if it changed anything); bInCancel puts the samples back instead. */
        void End(bool bInCancel);

        bool IsEmpty() const { return Before.empty(); }
        const Target& GetTarget() const { return TheTarget; }

    private:
        struct Chunk
        {
            GameCore::TerrainRect Rect;
            std::vector<float>    Heights;
            std::vector<uint8_t>  Weights;
        };

        void Backup(const GameCore::TerrainRect& InRect);

        Target                                       TheTarget;
        QString                                      Label;
        std::map<std::pair<uint32_t, uint32_t>, Chunk> Before;
        bool                                         bEnded = false;
    };

    /* The samples of InRect, copied out / written back (locks InData; MarkDirty on write). */
    void CopyRect(const GameCore::TerrainData& InData, const GameCore::TerrainRect& InRect,
                  std::vector<float>& OutHeights, std::vector<uint8_t>& OutWeights);
    void WriteRect(GameCore::TerrainData& InData, const GameCore::TerrainRect& InRect,
                   const std::vector<float>& InHeights, const std::vector<uint8_t>& InWeights);

    // === Saving =============================================================

    /* InPath's samples (a .nmterrain's, or a .nmfoliage's) changed since its file was written. */
    void MarkDirty(const std::string& InPath);
    bool IsDirty(const std::string& InPath);

    /* Writes every dirty terrain to its project file and stages it. Called
     * from AboutToSaveWorld. Returns false if one could not be written. */
    bool SaveDirty(QStringList* OutErrors = nullptr);

    /* Forgets unsaved edits: their next use reloads the file. Called when the
     * edited World is replaced (New, Open). */
    void DiscardUnsaved();

    /* The project file for a terrain's runtime path; empty if not in the project. */
    QString ProjectFileFor(const std::string& InRuntimePath);

    // === Heightmaps ========================================================

    /* An image's grey (8 or 16 bits a channel) as heights between InLow and
     * InHigh, resampled to the terrain's resolution: one undo step. */
    bool ImportHeightmap(const Target& InTarget, const QString& InImage, float InLow, float InHigh, QString& OutError);
    /* The heights as a 16-bit greyscale PNG, the lowest black and the highest white. */
    bool ExportHeightmap(const Target& InTarget, const QString& InImage, QString& OutError);

    // === Console ===========================================================

    /* "terrain ...": create, select, sculpt, paint, import, export, stats, tool, show. */
    void RunConsoleCommand(EditorConsole& InConsole, const QStringList& InArgs);
    QString GetConsoleUsage();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_TERRAINEDITING_H
