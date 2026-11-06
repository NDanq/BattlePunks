#if WITH_EDITOR

#include "Private/Editor/Particles/ParticleDocument.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <QFile>
#include <QSet>
#include <QUndoCommand>
#include <QUndoStack>

using RenderCore::ParticleEmitterDesc;
using RenderCore::ParticleSystemDesc;

namespace
{
    /* Whether the list a panel shows differs: count, order, names, switches. */
    bool ListDiffers(const ParticleSystemDesc& InA, const ParticleSystemDesc& InB)
    {
        if(InA.Emitters.size() != InB.Emitters.size()) { return true; }
        for(size_t Index = 0; Index < InA.Emitters.size(); ++Index)
        {
            if(std::strcmp(InA.Emitters[Index].Name, InB.Emitters[Index].Name) != 0
               || InA.Emitters[Index].bEnabled != InB.Emitters[Index].bEnabled)
            {
                return true;
            }
        }
        return false;
    }

    void SetName(ParticleEmitterDesc& InOut, const QString& InName)
    {
        const QByteArray Utf8 = InName.toUtf8();
        const size_t Length = std::min(size_t(Utf8.size()), sizeof(InOut.Name) - 1);
        std::memset(InOut.Name, 0, sizeof(InOut.Name));
        std::memcpy(InOut.Name, Utf8.constData(), Length);
    }
}

/* One step: the whole system before and after. Its first redo is the edit
 * itself, already made. */
class ParticleSnapshotCommand final : public QUndoCommand
{
public:
    ParticleSnapshotCommand(ParticleDocument& InDocument, const QString& InText,
                            ParticleSystemDesc InBefore, const int InSelectedBefore,
                            ParticleSystemDesc InAfter, const int InSelectedAfter)
        : QUndoCommand(InText)
        , Document(InDocument)
        , Before(std::move(InBefore)), After(std::move(InAfter))
        , SelectedBefore(InSelectedBefore), SelectedAfter(InSelectedAfter)
        , bListChanged(ListDiffers(Before, After))
    {
    }

    void undo() override { Document.Restore(Before, SelectedBefore, bListChanged); }

    void redo() override
    {
        if(bFirst)
        {
            bFirst = false;
            return;
        }
        Document.Restore(After, SelectedAfter, bListChanged);
    }

private:
    ParticleDocument&  Document;
    ParticleSystemDesc Before;
    ParticleSystemDesc After;
    int                SelectedBefore;
    int                SelectedAfter;
    bool               bListChanged;
    bool               bFirst = true;
};

ParticleDocument::ParticleDocument(QObject* InParent)
    : QObject(InParent)
    , History(new QUndoStack(this))
{
}

ParticleDocument::~ParticleDocument() = default;

bool ParticleDocument::Load(const QString& InFilePath, QString& OutError)
{
    QFile File(InFilePath);
    if(!File.open(QIODevice::ReadOnly))
    {
        OutError = QStringLiteral("Cannot read %1").arg(InFilePath);
        return false;
    }
    ParticleSystemDesc Loaded;
    std::string        Error;
    if(!RenderCore::ParseParticleSystem(File.readAll().toStdString(), Loaded, Error))
    {
        OutError = QString::fromStdString(Error);
        return false;
    }
    System    = std::move(Loaded);
    Committed = System;
    Selected  = System.Emitters.empty() ? -1 : 0;
    History->clear();
    emit EmittersChanged();
    emit Changed();
    emit SelectionChanged(Selected);
    return true;
}

bool ParticleDocument::Save(const QString& InFilePath, QString& OutError)
{
    if(!RenderCore::SaveParticleSystemFile(InFilePath.toStdString(), System))
    {
        OutError = QStringLiteral("Could not write %1").arg(InFilePath);
        return false;
    }
    History->setClean();
    return true;
}

ParticleEmitterDesc* ParticleDocument::GetEmitter(const int InIndex)
{
    return InIndex >= 0 && InIndex < int(System.Emitters.size()) ? &System.Emitters[size_t(InIndex)] : nullptr;
}

void ParticleDocument::SetSelected(int InIndex)
{
    InIndex = System.Emitters.empty() ? -1 : std::clamp(InIndex, 0, int(System.Emitters.size()) - 1);
    if(InIndex != Selected)
    {
        Selected = InIndex;
        emit SelectionChanged(Selected);
    }
}

void ParticleDocument::Edit(const QString& InText, const std::function<void(ParticleSystemDesc&)>& InChange, const int InSelectAfter)
{
    ParticleSystemDesc After = System;
    InChange(After);
    const int Count = int(After.Emitters.size());
    const int SelectAfter = Count == 0 ? -1 : std::clamp(InSelectAfter == KEEP_SELECTION ? Selected : InSelectAfter, 0, Count - 1);
    History->push(new ParticleSnapshotCommand(*this, InText, Committed, Selected, After, SelectAfter));
    Restore(After, SelectAfter, ListDiffers(System, After));
}

void ParticleDocument::CommitLiveEdit(const QString& InText)
{
    History->push(new ParticleSnapshotCommand(*this, InText, Committed, Selected, System, Selected));
    Committed = System;
    emit Changed();
}

void ParticleDocument::Restore(const ParticleSystemDesc& InSystem, const int InSelected, const bool bInListChanged)
{
    System    = InSystem;
    Committed = System;
    if(bInListChanged) { emit EmittersChanged(); }
    emit Changed();
    /* Announced even if unchanged after a list change: the details show the emitter now at that index. */
    if(bInListChanged) { Selected = -2; }
    SetSelected(InSelected);
}

QString ParticleDocument::UniqueName(const QString& InBase) const
{
    QSet<QString> Taken;
    for(const ParticleEmitterDesc& Each : System.Emitters) { Taken.insert(QString::fromUtf8(Each.Name)); }
    if(!Taken.contains(InBase)) { return InBase; }
    for(int Number = 2;; ++Number)
    {
        const QString Name = QStringLiteral("%1 %2").arg(InBase).arg(Number);
        if(!Taken.contains(Name)) { return Name; }
    }
}

bool ParticleDocument::AddEmitter()
{
    if(System.Emitters.size() >= RenderCore::PARTICLE_SYSTEM_EMITTERS_MAX) { return false; }
    const QString Name = UniqueName(QStringLiteral("Emitter"));
    Edit(QStringLiteral("Add Emitter"), [&](ParticleSystemDesc& InOut)
    {
        ParticleEmitterDesc& Added = InOut.Emitters.emplace_back();
        SetName(Added, Name);
    }, int(System.Emitters.size()));
    return true;
}

bool ParticleDocument::DuplicateEmitter(const int InIndex)
{
    if(!GetEmitter(InIndex) || System.Emitters.size() >= RenderCore::PARTICLE_SYSTEM_EMITTERS_MAX) { return false; }
    const QString Name = UniqueName(QString::fromUtf8(System.Emitters[size_t(InIndex)].Name));
    Edit(QStringLiteral("Duplicate Emitter"), [&](ParticleSystemDesc& InOut)
    {
        ParticleEmitterDesc Copy = InOut.Emitters[size_t(InIndex)];
        SetName(Copy, Name);
        Copy.Seed += 1;   // the copy's particles not in step with the original's
        InOut.Emitters.insert(InOut.Emitters.begin() + InIndex + 1, Copy);
    }, InIndex + 1);
    return true;
}

void ParticleDocument::RemoveEmitter(const int InIndex)
{
    if(!GetEmitter(InIndex)) { return; }
    Edit(QStringLiteral("Remove Emitter"), [InIndex](ParticleSystemDesc& InOut)
    {
        InOut.Emitters.erase(InOut.Emitters.begin() + InIndex);
    }, InIndex);
}

void ParticleDocument::MoveEmitter(const int InIndex, const int InBy)
{
    const int To = InIndex + InBy;
    if(!GetEmitter(InIndex) || !GetEmitter(To)) { return; }
    Edit(QStringLiteral("Move Emitter"), [InIndex, To](ParticleSystemDesc& InOut)
    {
        std::swap(InOut.Emitters[size_t(InIndex)], InOut.Emitters[size_t(To)]);
    }, To);
}

void ParticleDocument::RenameEmitter(const int InIndex, const QString& InName)
{
    const QString Name = InName.trimmed();
    if(!GetEmitter(InIndex) || Name.isEmpty() || Name == QString::fromUtf8(System.Emitters[size_t(InIndex)].Name)) { return; }
    Edit(QStringLiteral("Rename Emitter"), [InIndex, &Name](ParticleSystemDesc& InOut) { SetName(InOut.Emitters[size_t(InIndex)], Name); });
}

void ParticleDocument::SetEmitterEnabled(const int InIndex, const bool bInEnabled)
{
    if(!GetEmitter(InIndex) || (System.Emitters[size_t(InIndex)].bEnabled != 0) == bInEnabled) { return; }
    Edit(bInEnabled ? QStringLiteral("Enable Emitter") : QStringLiteral("Disable Emitter"), [InIndex, bInEnabled](ParticleSystemDesc& InOut)
    {
        InOut.Emitters[size_t(InIndex)].bEnabled = bInEnabled ? 1 : 0;
    });
}

bool ParticleDocument::IsDirty() const
{
    return !History->isClean();
}

#endif // WITH_EDITOR
