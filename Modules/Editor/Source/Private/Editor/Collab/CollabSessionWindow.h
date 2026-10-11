#ifndef NIMBLEEDITOR_COLLABSESSIONWINDOW_H
#define NIMBLEEDITOR_COLLABSESSIONWINDOW_H

#if WITH_EDITOR

#include <QDockWidget>

class EditorCollabSession;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

/*
 * The Session dock (View > Session; File > Collaboration Session): the
 * server's address, your name and the session password, Join / Leave, and
 * everyone in the session -- their HEAD, open level, whether they're playing,
 * and how many files they have changed (the tooltip lists them; a file you
 * have changed too is marked).
 *
 * The address and name are Editor Settings > Collaboration's
 * (Editor.CollabServer, Editor.CollabUserName), saved on a successful join;
 * the password is never saved.
 */
class CollabSessionWindow : public QDockWidget
{
    Q_OBJECT

public:
    CollabSessionWindow(EditorCollabSession& InSession, QWidget* InParent);

    QTreeWidget* GetRoster() const { return Roster; }
    QLineEdit*   GetAddressEdit() const { return Address; }

private:
    void UpdateState();
    void UpdateRoster();

    EditorCollabSession& Session;
    QLineEdit*   Address   = nullptr;
    QLineEdit*   UserName  = nullptr;
    QLineEdit*   Password  = nullptr;
    QPushButton* JoinButton  = nullptr;
    QPushButton* LeaveButton = nullptr;
    QLabel*      StateLabel = nullptr;
    QTreeWidget* Roster     = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_COLLABSESSIONWINDOW_H
