#if WITH_EDITOR

#include "Private/Editor/UI/UIDesignDocument.h"

#include <QFile>
#include <QFileInfo>
#include <QUndoCommand>
#include <QUndoStack>

#include "Public/UI/UIFile.h"
#include "Public/UI/UIProperty.h"
#include "Private/Editor/UI/UIAssets.h"

/* One edit: the whole document before and after it. */
class UISnapshotCommand : public QUndoCommand
{
public:
    UISnapshotCommand(UIDesignDocument& InDocument, const QString& InLabel, UI::UIDocumentAsset InBefore, UI::UIDocumentAsset InAfter,
                      const int InSelectedBefore, const int InSelectedAfter, const QString& InMergeKey, const bool bInStructure)
        : QUndoCommand(InLabel)
        , Document(InDocument)
        , Before(std::move(InBefore))
        , After(std::move(InAfter))
        , SelectedBefore(InSelectedBefore)
        , SelectedAfter(InSelectedAfter)
        , MergeKey(InMergeKey)
        , bStructure(bInStructure)
    {
    }

    void undo() override { Document.Restore(Before, SelectedBefore, bStructure); }

    void redo() override
    {
        /* The first redo is QUndoStack::push's: the edit is already done. */
        if(bFirst) { bFirst = false; return; }
        Document.Restore(After, SelectedAfter, bStructure);
    }

    int id() const override { return MergeKey.isEmpty() ? -1 : 1; }

    bool mergeWith(const QUndoCommand* InOther) override
    {
        const auto* Other = dynamic_cast<const UISnapshotCommand*>(InOther);
        if(!Other || Other->MergeKey != MergeKey || &Other->Document != &Document) return false;
        After         = Other->After;
        SelectedAfter = Other->SelectedAfter;
        bStructure    = bStructure || Other->bStructure;
        return true;
    }

private:
    UIDesignDocument&   Document;
    UI::UIDocumentAsset Before, After;
    int                 SelectedBefore, SelectedAfter;
    QString             MergeKey;
    bool                bStructure;
    bool                bFirst = true;
};

namespace
{
    std::string Utf8(const QString& InText) { return InText.toUtf8().toStdString(); }
}

UIDesignDocument::UIDesignDocument(QObject* InParent)
    : QObject(InParent)
    , Undo(new QUndoStack(this))
{
    /* A new document: a full-screen Canvas, as File > New makes. */
    UI::UIWidgetDesc Root;
    Root.Type = "Canvas";
    Root.Properties.push_back({ "Name", "Root" });
    Asset.Widgets.push_back(Root);
    Shared = std::make_shared<UI::UIDocumentAsset>(Asset);
    connect(Undo, &QUndoStack::cleanChanged, this, [this](const bool bInClean) { emit DirtyChanged(!bInClean); });
}

UIDesignDocument::~UIDesignDocument() = default;

bool UIDesignDocument::Load(const QString& InPath, QString* OutError)
{
    QFile File(InPath);
    if(!File.open(QIODevice::ReadOnly))
    {
        if(OutError) *OutError = QStringLiteral("cannot be read");
        return false;
    }
    const QByteArray Bytes = File.readAll();
    UI::UIDocumentAsset Loaded;
    std::string Error;
    if(!UI::ParseUIXml(Bytes.toStdString(), Loaded, &Error))
    {
        if(OutError) *OutError = QString::fromStdString(Error);
        return false;
    }
    Path  = InPath;
    Asset = std::move(Loaded);
    Undo->clear();
    Selected = -1;
    Publish(true);
    emit SelectionChanged(Selected);
    return true;
}

bool UIDesignDocument::Save(QString* OutError)
{
    if(Path.isEmpty())
    {
        if(OutError) *OutError = QStringLiteral("it has no file yet");
        return false;
    }
    std::string Error;
    if(!UI::SaveUIXml(Utf8(Path), Asset, &Error))
    {
        if(OutError) *OutError = QString::fromStdString(Error);
        return false;
    }
    EditorUI::Stage(Path);
    Undo->setClean();
    return true;
}

void UIDesignDocument::Edit(const QString& InLabel, const std::function<void(UI::UIDocumentAsset&)>& InEdit,
                            const QString& InMergeKey, const bool bInStructure)
{
    UI::UIDocumentAsset Before = Asset;
    const int SelectedBefore = Selected;
    InEdit(Asset);
    if(UI::WriteUIXml(Asset) == UI::WriteUIXml(Before) && Selected == SelectedBefore)
    {
        return;   // nothing to record
    }
    Publish(bInStructure);

    /* Names that changed: the same widgets renamed, or some gone. */
    QStringList Renamed, Removed;
    if(Before.Widgets.size() == Asset.Widgets.size())
    {
        for(size_t At = 0; At < Asset.Widgets.size(); ++At)
        {
            const std::string Old = Before.Widgets[At].GetName(), New = Asset.Widgets[At].GetName();
            if(Old != New && !Old.empty()) { Renamed << QString::fromStdString(Old) << QString::fromStdString(New); }
        }
    }
    else if(Asset.Widgets.size() < Before.Widgets.size())
    {
        for(const UI::UIWidgetDesc& Each : Before.Widgets)
        {
            const std::string Name = Each.GetName();
            if(!Name.empty() && Asset.FindWidget(Name) < 0) { Removed << QString::fromStdString(Name); }
        }
    }
    Undo->push(new UISnapshotCommand(*this, InLabel, std::move(Before), Asset, SelectedBefore, Selected, InMergeKey, bInStructure));
    for(int At = 0; At + 1 < Renamed.size(); At += 2) { emit WidgetRenamed(Renamed[At], Renamed[At + 1]); }
    if(!Removed.isEmpty()) { emit WidgetsRemoved(Removed); }
}

void UIDesignDocument::SetProperty(const int InWidget, const QString& InName, const QString& InValue)
{
    if(InWidget < 0 || InWidget >= int(Asset.Widgets.size())) return;
    const std::string Name = Utf8(InName), Value = Utf8(InValue);
    Edit(QStringLiteral("Set %1").arg(InName), [&](UI::UIDocumentAsset& InAsset)
    {
        InAsset.Widgets[size_t(InWidget)].Set(Name, Value);
    }, QStringLiteral("set:%1:%2").arg(InWidget).arg(InName), InName == QStringLiteral("Name"));
}

void UIDesignDocument::ResetProperty(const int InWidget, const QString& InName)
{
    if(InWidget < 0 || InWidget >= int(Asset.Widgets.size())) return;
    const std::string Name = Utf8(InName);
    Edit(QStringLiteral("Reset %1").arg(InName), [&](UI::UIDocumentAsset& InAsset)
    {
        InAsset.Widgets[size_t(InWidget)].Remove(Name);
    }, QString(), InName == QStringLiteral("Name"));
}

int UIDesignDocument::AddWidget(const QString& InType, const int InParent, const int InBefore, const std::vector<UI::UIPropertyText>& InProperties)
{
    const UI::UIWidgetType* Type = UI::FindWidgetType(Utf8(InType).c_str());
    if(!Type || Type->IsAbstract() || !Asset.CanTakeChild(InParent)) return -1;

    /* A name of its own: Button, Button2, ... */
    std::string Name = Type->Name;
    for(int Suffix = 2; Asset.FindWidget(Name) >= 0; ++Suffix) { Name = Type->Name + std::to_string(Suffix); }

    UI::UIWidgetDesc Desc;
    Desc.Type = Type->Name;
    Desc.Properties.push_back({ "Name", Name });
    for(const UI::UIPropertyText& Each : InProperties) { Desc.Set(Each.Name, Each.Value); }

    int Added = -1;
    Edit(QStringLiteral("Add %1").arg(InType), [&](UI::UIDocumentAsset& InAsset)
    {
        Added = InAsset.InsertWidgets(InParent, InBefore, { Desc });
        if(Added >= 0) Selected = Added;
    }, QString(), true);
    if(Added >= 0) emit SelectionChanged(Selected);
    return Added;
}

void UIDesignDocument::DeleteSelected()
{
    if(Selected <= 0) return;
    const int Parent = Asset.Widgets[size_t(Selected)].Parent;
    const int Gone = Selected;
    Edit(QStringLiteral("Delete"), [&](UI::UIDocumentAsset& InAsset)
    {
        if(InAsset.RemoveWidget(Gone)) Selected = Parent;
    }, QString(), true);
    emit SelectionChanged(Selected);
}

void UIDesignDocument::DuplicateSelected()
{
    if(Selected <= 0) return;
    const int Source = Selected;
    const int Parent = Asset.Widgets[size_t(Source)].Parent;
    if(!Asset.CanTakeChild(Parent)) return;
    std::vector<UI::UIWidgetDesc> Copy = Asset.CopyWidgets(Source);

    /* The copy's names their own, so actions and Lua find each. */
    for(UI::UIWidgetDesc& Each : Copy)
    {
        const std::string Name = Each.GetName();
        if(Name.empty()) continue;
        std::string Fresh = Name + "Copy";
        for(int Suffix = 2; Asset.FindWidget(Fresh) >= 0; ++Suffix) { Fresh = Name + "Copy" + std::to_string(Suffix); }
        Each.Set("Name", Fresh);
    }
    const int After = Asset.GetSubtreeEnd(Source);
    const int Before = After < int(Asset.Widgets.size()) && Asset.Widgets[size_t(After)].Parent == Parent ? After : -1;
    Edit(QStringLiteral("Duplicate"), [&](UI::UIDocumentAsset& InAsset)
    {
        const int Made = InAsset.InsertWidgets(Parent, Before, Copy);
        if(Made >= 0) Selected = Made;
    }, QString(), true);
    emit SelectionChanged(Selected);
}

bool UIDesignDocument::MoveWidget(const int InIndex, const int InNewParent, const int InBefore)
{
    int Moved = -1;
    UI::UIDocumentAsset Probe = Asset;
    if(Probe.MoveWidget(InIndex, InNewParent, InBefore) < 0) return false;
    Edit(QStringLiteral("Move"), [&](UI::UIDocumentAsset& InAsset)
    {
        Moved = InAsset.MoveWidget(InIndex, InNewParent, InBefore);
        if(Moved >= 0) Selected = Moved;
    }, QString(), true);
    emit SelectionChanged(Selected);
    return Moved >= 0;
}

QString UIDesignDocument::CopySelected() const
{
    return Selected >= 0 ? QString::fromStdString(UI::WriteUIXmlFragment(Asset, Selected)) : QString();
}

bool UIDesignDocument::Paste(const QString& InXml)
{
    std::vector<UI::UIWidgetDesc> Widgets;
    if(!UI::ParseUIXmlFragment(Utf8(InXml), Widgets) || Widgets.empty()) return false;

    /* Into the selection if it has room, else beside it. */
    int Parent = Selected >= 0 ? Selected : 0;
    if(!Asset.CanTakeChild(Parent) && Parent > 0) Parent = Asset.Widgets[size_t(Parent)].Parent;
    if(!Asset.CanTakeChild(Parent)) return false;

    for(UI::UIWidgetDesc& Each : Widgets)
    {
        const std::string Name = Each.GetName();
        if(Name.empty() || Asset.FindWidget(Name) < 0) continue;
        std::string Fresh = Name + "Copy";
        for(int Suffix = 2; Asset.FindWidget(Fresh) >= 0; ++Suffix) { Fresh = Name + "Copy" + std::to_string(Suffix); }
        Each.Set("Name", Fresh);
    }
    Edit(QStringLiteral("Paste"), [&](UI::UIDocumentAsset& InAsset)
    {
        const int Made = InAsset.InsertWidgets(Parent, -1, Widgets);
        if(Made >= 0) Selected = Made;
    }, QString(), true);
    emit SelectionChanged(Selected);
    return true;
}

void UIDesignDocument::Select(const int InWidget)
{
    const int Next = InWidget >= 0 && InWidget < int(Asset.Widgets.size()) ? InWidget : -1;
    if(Next == Selected) return;
    Selected = Next;
    emit SelectionChanged(Selected);
}

bool UIDesignDocument::IsDirty() const
{
    return !Undo->isClean();
}

void UIDesignDocument::MarkClean()
{
    Undo->setClean();
}

void UIDesignDocument::Restore(const UI::UIDocumentAsset& InAsset, const int InSelected, const bool bInStructure)
{
    Asset = InAsset;
    const bool bSelection = InSelected != Selected;
    Selected = InSelected < int(Asset.Widgets.size()) ? InSelected : -1;
    Publish(bInStructure);
    if(bSelection) emit SelectionChanged(Selected);
}

void UIDesignDocument::Publish(const bool bInStructure)
{
    Shared = std::make_shared<UI::UIDocumentAsset>(Asset);
    emit Changed(bInStructure);
}

#endif // WITH_EDITOR
