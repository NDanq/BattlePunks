#ifndef NIMBLEEDITOR_PARTICLEDOCUMENT_H
#define NIMBLEEDITOR_PARTICLEDOCUMENT_H

#if WITH_EDITOR

#include <functional>

#include <QObject>
#include <QString>

#include "Public/RenderCore/ParticleSystemAsset.h"

class QUndoStack;

/*
 * One particle system open in the Particle Editor: the asset
 * (RenderCore::ParticleSystemDesc), the emitter selected, and an undo
 * history of its own. Every edit is a before/after snapshot of the whole
 * system -- a few emitters of plain data -- as a graph's are.
 *
 * Two ways to edit, both one undo step:
 *
 *   Edit(Text, Change)   a change made here (add, remove, rename, move...)
 *   CommitLiveEdit(Text) a change already written into an emitter's bytes
 *                        by a property widget (GetEmitter), recorded after
 *
 * Signals: Changed after any change to the system, undo included;
 * EmittersChanged when the list itself did (count, order, names, switches);
 * SelectionChanged; and the history's own.
 */
class ParticleDocument : public QObject
{
    Q_OBJECT

public:
    explicit ParticleDocument(QObject* InParent = nullptr);
    ~ParticleDocument() override;

    /* From a file on disk; false with OutError, leaving the document as it was. */
    bool Load(const QString& InFilePath, QString& OutError);

    /* To a file on disk; marks the history clean. */
    bool Save(const QString& InFilePath, QString& OutError);

    const RenderCore::ParticleSystemDesc& GetSystem() const { return System; }
    int GetEmitterCount() const { return int(System.Emitters.size()); }

    /* The live emitter (null if InIndex is out of range): what property
     * widgets read and write. Re-fetch on every access -- the list may move. */
    RenderCore::ParticleEmitterDesc* GetEmitter(int InIndex);

    int  GetSelected() const { return Selected; }
    void SetSelected(int InIndex);

    /* InSelectAfter: the emitter selected once it is made (and again on redo); KEEP_SELECTION keeps it. */
    static constexpr int KEEP_SELECTION = -100;
    void Edit(const QString& InText, const std::function<void(RenderCore::ParticleSystemDesc&)>& InChange,
              int InSelectAfter = KEEP_SELECTION);
    void CommitLiveEdit(const QString& InText);

    /* A change written into an emitter's bytes and not done yet (a key being
     * dragged): shown, not recorded. LiveChanged, not Changed. */
    void PreviewLiveEdit() { emit LiveChanged(); }

    // --- The emitter list ----------------------------------------------------
    // Each one undo step, selecting what it made or moved.

    /* False (nothing done) at PARTICLE_SYSTEM_EMITTERS_MAX. */
    bool AddEmitter();
    bool DuplicateEmitter(int InIndex);
    void RemoveEmitter(int InIndex);
    void MoveEmitter(int InIndex, int InBy);
    void RenameEmitter(int InIndex, const QString& InName);
    void SetEmitterEnabled(int InIndex, bool bInEnabled);

    QUndoStack& GetHistory() { return *History; }
    bool IsDirty() const;

signals:
    void Changed();
    void LiveChanged();
    void EmittersChanged();
    void SelectionChanged(int InIndex);

private:
    friend class ParticleSnapshotCommand;

    /* Undo and redo: the system and selection as they were. */
    void Restore(const RenderCore::ParticleSystemDesc& InSystem, int InSelected, bool bInListChanged);

    /* "Smoke", "Smoke 2"...: a name no other emitter has. */
    QString UniqueName(const QString& InBase) const;

    RenderCore::ParticleSystemDesc System;
    RenderCore::ParticleSystemDesc Committed;   // as of the last recorded step: a live edit's "before"
    int                            Selected = -1;
    QUndoStack*                    History  = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLEDOCUMENT_H
