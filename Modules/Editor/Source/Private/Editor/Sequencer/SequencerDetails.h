#ifndef NIMBLEEDITOR_SEQUENCERDETAILS_H
#define NIMBLEEDITOR_SEQUENCERDETAILS_H

#if WITH_EDITOR

#include <functional>

#include <QWidget>

#include "Private/Editor/Sequencer/SequencerDocument.h"

class QFormLayout;

/*
 * The Sequencer's details: a form for what is selected -- a key, section,
 * event or marker -- else the current track or binding, else the sequence
 * itself (rate and length). Each field is one undo step as it is
 * committed (Enter, or focus leaving).
 */
class SequencerDetails : public QWidget
{
    Q_OBJECT

public:
    explicit SequencerDetails(SequencerDocument& InDocument, QWidget* InParent = nullptr);

    /* Rebuilds the form for what is selected now. */
    void Rebuild();

signals:
    /* The binding's "Bind to Selected Entity" button. */
    void BindToSelectedRequested(int InBinding);

private:
    void BuildSequence();
    void BuildBinding(int InBinding);
    void BuildTrack(const SequencerTrackRef& InTrack);
    void BuildKey(const SequencerItem& InItem);
    void BuildSection(const SequencerItem& InItem);
    void BuildEvent(const SequencerItem& InItem);
    void BuildMarker(const SequencerItem& InItem);

    /* Rows of the form, each committing through Document.Edit. */
    void AddInt(const QString& InLabel, int InValue, int InMin, int InMax, const std::function<void(Sequencer::SequenceAsset&, int)>& InSet);
    void AddFloat(const QString& InLabel, double InValue, double InMin, double InMax, int InDecimals,
                  const std::function<void(Sequencer::SequenceAsset&, double)>& InSet);
    void AddText(const QString& InLabel, const QString& InValue, const std::function<void(Sequencer::SequenceAsset&, const QString&)>& InSet,
                 const QString& InBrowseFilter = QString());
    void AddCheck(const QString& InLabel, bool bInValue, const std::function<void(Sequencer::SequenceAsset&, bool)>& InSet);
    void AddChoice(const QString& InLabel, const QStringList& InChoices, int InCurrent,
                   const std::function<void(Sequencer::SequenceAsset&, int)>& InSet);
    void AddNote(const QString& InText);

    SequencerDocument& Document;
    QWidget*           Body = nullptr;
    QFormLayout*       Form = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERDETAILS_H
