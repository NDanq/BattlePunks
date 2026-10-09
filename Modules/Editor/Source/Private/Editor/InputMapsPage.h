#ifndef NIMBLEEDITOR_INPUTMAPSPAGE_H
#define NIMBLEEDITOR_INPUTMAPSPAGE_H

#if WITH_EDITOR

#include <vector>

#include <QString>
#include <QStringList>
#include <QWidget>

class ConfigIniFile;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * Project Settings > Input: the game's input maps (NIMBLE_DEFINE_INPUT_MAP,
 * NimbleEngine/Input.h) -- every action and axis, its keys, and what each was
 * given in code -- to rebind for the whole project. A change is written to
 * the project's .ini as [Input_<Map>] <Entry>=<keys>, the form a map reads its
 * overrides in, and set in this process at once.
 *
 * Maps come from the game module the editor has loaded; a map only the .ini
 * names (no module, or one not built) is listed from the file, and edits the
 * same way.
 *
 * Keys are typed ("Space, Gamepad.South"; '-' negates one on an axis, "*0.5"
 * scales it), picked from the list of every key, or caught: Press a Key...
 * takes the next key or mouse button. Two entries of one map sharing a key
 * are flagged.
 */
class InputMapsPage : public QWidget
{
    Q_OBJECT

public:
    InputMapsPage(const ConfigIniFile& InFile, QWidget* InParent = nullptr);

    /* Every changed binding into InOutFile, and into the running maps. One
     * "Input_<Map>.<Entry> = <keys>" line per change, for the log. */
    QStringList Apply(ConfigIniFile& InOutFile);

private:
    struct Entry
    {
        QString          Map;
        QString          Name;
        QString          Baseline;   // what the file says, else the code's; what Apply compares with
        QString          Default;    // what the code gives it (empty if not registered)
        bool             bAxis   = false;
        bool             bInFile = false;
        QTreeWidgetItem* Item    = nullptr;
    };

    void   Populate(const ConfigIniFile& InFile);
    Entry* Selected();
    void   OnSelectionChanged();
    void   AppendKey(const QString& InKey);
    void   CaptureKey();
    void   ResetSelected();
    /* Every entry's font (changed, or set in the file) and conflict note. */
    void   RefreshAll();

    std::vector<Entry> Entries;
    QTreeWidget*       Tree        = nullptr;
    QComboBox*         KeyChoice   = nullptr;
    QPushButton*       AddKey      = nullptr;
    QPushButton*       Capture     = nullptr;
    QPushButton*       Reset       = nullptr;
    QLabel*            Note        = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_INPUTMAPSPAGE_H
