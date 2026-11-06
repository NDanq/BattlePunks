#ifndef NIMBLEEDITOR_BEHAVIORTREEEDITORWINDOW_H
#define NIMBLEEDITOR_BEHAVIORTREEEDITORWINDOW_H

#if WITH_EDITOR && WITH_AICORE

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QMainWindow>
#include <QString>

#include "Public/AICore/BehaviorTree.h"

class GraphView;
class QAction;
class QDoubleSpinBox;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsSimpleTextItem;
class QLabel;
class QLineEdit;
class QListWidget;
class QTableWidget;
class QTimer;
class QWidget;

/*
 * A behavior tree (.nmbt; Engine/Modules/AICore/README.md, "Behavior trees")
 * in three panes:
 *
 *   left     the tree's name, the blackboard's keys (name, type, default) and
 *            the palette: the composites, every registered task, decorator
 *            and service. Double-click (or Add) puts the palette's pick under
 *            the selected node -- after it, for a task -- and a decorator or
 *            service onto it
 *   centre   the tree, laid out top-down: a box a node -- its name, type,
 *            decorators, services and considerations, and its order among
 *            its siblings (a Selector's priority). With Debug in PIE on, the
 *            running branch is lit blue and each utility child shows its
 *            live score
 *   right    the selected node: name, weight, params; its decorators (and
 *            abort modes) and services, their params as "Key=Value; ...";
 *            its considerations, the selected one's response curve drawn
 *
 * Move Up / Down reorder a node among its siblings; Cut, Copy, Paste and
 * Duplicate move whole branches. Every edit is undoable. Save writes the XML
 * (SaveBehaviorTreeXml) and restages it: running brains restart on the new
 * tree. A tree naming types this editor has not registered (a game's tasks
 * before its module loads) still opens; those nodes are drawn red, and the
 * status line names the first problem. One window per file.
 */
class BehaviorTreeEditorWindow final : public QMainWindow
{
    Q_OBJECT

public:
    /* The window for InPath (a .nmbt on disk), opened or raised; null (and
     * a message) if it cannot be read. */
    static BehaviorTreeEditorWindow* Open(const QString& InPath, QWidget* InParent);
    /* Asks for a name and writes a new tree in InFolder -- a Selector with a
     * Wait -- then opens it. */
    static BehaviorTreeEditorWindow* CreateIn(const QString& InFolder, QWidget* InParent);

    ~BehaviorTreeEditorWindow() override;

    const QString& GetPath() const { return Path; }
    bool Save();
    void SetDebugging(bool bInOn);
    /* The tree in view: all of it if readable, else from the root down. */
    void FrameTree();
    /* Selects the first node named (or, unnamed, typed) InName, depth-first,
     * and centres it ("ai.opentree ... <node>"). False if there is none. */
    bool SelectByName(const QString& InName);

    /* The editable tree; flattened depth-first, as the file reads, for the
     * asset and the debugger. */
    struct Node
    {
        AICore::BTNodeDef Def;
        std::vector<std::unique_ptr<Node>> Children;
        Node* Parent = nullptr;
        int   Flat = -1;    // index in the last flattening
    };

protected:
    void closeEvent(class QCloseEvent* InEvent) override;

private:
    explicit BehaviorTreeEditorWindow(const QString& InPath);
    bool Load(QString& OutError);
    void SetTree(const AICore::BehaviorTreeAsset& InAsset);
    void BuildUi();
    QWidget* BuildLeft();
    QWidget* BuildDetails();

    AICore::BehaviorTreeAsset Flatten();
    std::string Snapshot();
    void Rebuild();          // the canvas from the tree
    void FillDetails();      // the right pane from the selection
    void FillBlackboard();
    void FillConsiderationCurve();
    /* After any change: dirty, undo, the canvas (and details unless an edit
     * of them made it), the status line. */
    void Edited(bool bInRefillDetails = true);
    void Select(Node* InNode);
    void Insert(std::unique_ptr<Node> InNode);
    void AddFromPalette();
    void DeleteSelected();
    void MoveSelected(int InBy);
    void CopySelected(bool bInCut);
    void Paste();
    void Undo(bool bInRedo);
    void ApplyDetails();     // the right pane's widgets -> the selected node
    void ApplyBlackboard();
    void UpdateTitle();
    void UpdateDebug();

    QString Path;
    QString Name;
    std::vector<AICore::BlackboardKey> Keys;
    std::unique_ptr<Node> Root;
    Node* Selected = nullptr;
    std::unique_ptr<Node> Clipboard;
    std::vector<std::string> UndoStack, RedoStack;
    std::string Current;     // the tree as last snapshotted
    bool  bDirty = false;
    bool  bFilling = false;

    std::unordered_map<const Node*, QGraphicsRectItem*>       Boxes;
    std::unordered_map<const Node*, QGraphicsSimpleTextItem*> Scores;

    QGraphicsScene* Scene = nullptr;
    GraphView*      View = nullptr;
    QLineEdit*      TreeName = nullptr;
    QTableWidget*   Blackboard = nullptr;
    QListWidget*    Palette = nullptr;
    QLabel*         NodeTitle = nullptr;
    QLineEdit*      NodeName = nullptr;
    QDoubleSpinBox* NodeWeight = nullptr;
    QTableWidget*   Params = nullptr;
    QTableWidget*   Decorators = nullptr;
    QTableWidget*   Services = nullptr;
    QTableWidget*   Considerations = nullptr;
    QWidget*        Curve = nullptr;
    QWidget*        DetailsBody = nullptr;
    QLabel*         StatusLine = nullptr;
    QLabel*         DebugLine = nullptr;
    QAction*        DebugAction = nullptr;
    QTimer*         DebugTimer = nullptr;
};

#endif // WITH_EDITOR && WITH_AICORE

#endif //NIMBLEEDITOR_BEHAVIORTREEEDITORWINDOW_H
