#include "Private/Editor/EditorEditActions.h"

#include <vector>

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMenu>
#include <QWidget>

#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

namespace EditorEditActions
{
    namespace
    {
        constexpr int ActionCount = int(EAction::Count);
    }

    QString GetLabel(const EAction InAction)
    {
        switch(InAction)
        {
            case EAction::Cut:       return QStringLiteral("Cu&t");
            case EAction::Copy:      return QStringLiteral("&Copy");
            case EAction::Paste:     return QStringLiteral("&Paste");
            case EAction::Duplicate: return QStringLiteral("D&uplicate");
            case EAction::Rename:    return QStringLiteral("&Rename...");
            case EAction::Delete:    return QStringLiteral("&Delete");
            case EAction::Count:     break;
        }
        return QString();
    }

    QKeySequence GetKey(const EAction InAction)
    {
        switch(InAction)
        {
            case EAction::Cut:       return QKeySequence::Cut;
            case EAction::Copy:      return QKeySequence::Copy;
            case EAction::Paste:     return QKeySequence::Paste;
            case EAction::Duplicate: return QKeySequence(Qt::CTRL | Qt::Key_D);
            case EAction::Rename:    return QKeySequence(Qt::Key_F2);
            case EAction::Delete:    return QKeySequence::Delete;
            case EAction::Count:     break;
        }
        return QKeySequence();
    }

    bool IsAvailable(const EAction InAction, const EditorDocument& InDocument)
    {
        switch(InAction)
        {
            case EAction::Paste:  return InDocument.CanPaste();
            case EAction::Rename: return InDocument.GetSelectedEntities().size() == 1;
            default:              return !InDocument.GetSelectedEntities().empty();
        }
    }

    void Run(const EAction InAction, EditorDocument& InDocument, const std::function<void()>& InRename)
    {
        if(!IsAvailable(InAction, InDocument))
        {
            return;
        }
        switch(InAction)
        {
            case EAction::Cut:       InDocument.CutSelection();       break;
            case EAction::Copy:      InDocument.CopySelection();      break;
            case EAction::Paste:     InDocument.Paste();              break;
            case EAction::Duplicate: InDocument.DuplicateSelection(); break;
            case EAction::Rename:    if(InRename) { InRename(); }     break;
            case EAction::Delete:    InDocument.DeleteSelection();    break;
            case EAction::Count:     break;
        }
    }

    void PromptRename(QWidget* InParent, EditorDocument& InDocument)
    {
        const GameCore::EntityID Entity = InDocument.GetSelection();
        if(!InDocument.GetWorld().IsAlive(Entity))
        {
            return;
        }
        const QString Old = QString::fromUtf8(InDocument.GetWorld().GetEntity(Entity).GetName());
        bool bOk = false;
        const QString Name = QInputDialog::getText(InParent, QStringLiteral("Rename"), QStringLiteral("Name"), QLineEdit::Normal, Old, &bOk);
        if(bOk && Name != Old && InDocument.GetWorld().IsAlive(Entity))
        {
            InDocument.RenameEntity(Entity, Name.trimmed());
        }
    }

    void Install(QWidget* InOwner, EditorDocument& InDocument, std::function<void()> InRename, std::function<bool()> InAllowed)
    {
        QAction* Actions[ActionCount] = {};
        for(int Index = 0; Index < ActionCount; ++Index)
        {
            const EAction Which = EAction(Index);
            QAction* Action = new QAction(GetLabel(Which), InOwner);
            Action->setShortcut(GetKey(Which));
            Action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            QObject::connect(Action, &QAction::triggered, InOwner, [&InDocument, Which, InRename, InAllowed]()
            {
                if(!InAllowed || InAllowed())
                {
                    Run(Which, InDocument, InRename);
                }
            });
            InOwner->addAction(Action);
            Actions[Index] = Action;
        }

        /* Enabled as the document allows: a disabled action leaves its key to
         * the widget (Delete with nothing selected, F2 with several). */
        const auto Update = [&InDocument, Actions = std::vector<QAction*>(Actions, Actions + ActionCount)]()
        {
            for(int Index = 0; Index < ActionCount; ++Index)
            {
                Actions[size_t(Index)]->setEnabled(IsAvailable(EAction(Index), InDocument));
            }
        };
        QObject::connect(&InDocument, &EditorDocument::SelectionChanged, InOwner, Update);
        QObject::connect(&InDocument, &EditorDocument::WorldReset, InOwner, Update);
        if(qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        {
            QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, InOwner, Update);
        }
        Update();
    }

    std::vector<QAction*> AddToMenu(QMenu* InMenu, EditorDocument& InDocument, const std::function<void()>& InRename)
    {
        std::vector<QAction*> Items;
        for(int Index = 0; Index < ActionCount; ++Index)
        {
            const EAction Which = EAction(Index);
            if(Which == EAction::Rename || Which == EAction::Delete)
            {
                InMenu->addSeparator();
            }
            QAction* Action = InMenu->addAction(GetLabel(Which));
            Action->setShortcut(GetKey(Which));
            Action->setShortcutContext(Qt::WidgetShortcut);   // shown, not taken: the menu itself owns it
            Action->setEnabled(IsAvailable(Which, InDocument));
            QObject::connect(Action, &QAction::triggered, InMenu, [&InDocument, Which, InRename]() { Run(Which, InDocument, InRename); });
            Items.push_back(Action);
        }
        return Items;
    }

    void Refresh(const std::vector<QAction*>& InItems, const EditorDocument& InDocument)
    {
        for(size_t Index = 0; Index < InItems.size() && Index < size_t(ActionCount); ++Index)
        {
            InItems[Index]->setEnabled(IsAvailable(EAction(Index), InDocument));
        }
    }
}
