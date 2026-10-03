#ifndef NIMBLEEDITOR_NEWPROJECTWIZARD_H
#define NIMBLEEDITOR_NEWPROJECTWIZARD_H

#if WITH_EDITOR

#include <QWizard>

class QLabel;
class QLineEdit;
class QPlainTextEdit;

/*
 * File > New Project: a new game in Games/ (or anywhere), made from
 * Engine/Templates/Game by ProjectTemplate.h.
 *
 *   Project   the name and the location, checked as they are typed
 *   Created   what was written, and how to build and open it
 *
 * The editor edits the project it was built with, so the new game is opened
 * by building its own tree (its CMakePresets.json), not from here. Modal.
 */
class NewProjectWizard : public QWizard
{
    Q_OBJECT

public:
    explicit NewProjectWizard(QWidget* InParent);

private:
    enum EPage { Page_Project, Page_Created };

    QWizardPage* BuildProjectPage();
    QWizardPage* BuildCreatedPage();

    bool validateCurrentPage() override;
    void RefreshStatus();

    QLineEdit*      NameEdit     = nullptr;
    QLineEdit*      LocationEdit = nullptr;
    QLabel*         Status       = nullptr;
    QLabel*         Summary      = nullptr;
    QPlainTextEdit* Files        = nullptr;
    QString         ProjectDir;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_NEWPROJECTWIZARD_H
