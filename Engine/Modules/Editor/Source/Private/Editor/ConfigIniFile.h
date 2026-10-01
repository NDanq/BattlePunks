#ifndef NIMBLEEDITOR_CONFIGINIFILE_H
#define NIMBLEEDITOR_CONFIGINIFILE_H

#if WITH_EDITOR

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

/*
 * An .ini in Core's config grammar, edited in place: Set rewrites only the
 * key's line, preserving comments and layout.
 *
 * Grammar: whole-line ';' / '#' comments, "[Section]", "Key = Value" (value
 * optionally quoted). Case-insensitive sections and keys.
 */
class ConfigIniFile
{
public:
    /* False if unreadable; a missing file is empty (Save creates it). */
    bool Load(const QString& InPath);

    /* Writes every line back, with the line ending the file had. */
    bool Save(const QString& InPath) const;

    /* Quotes stripped; empty with OutbFound false if absent. */
    QString Get(const QString& InSection, const QString& InKey, bool* OutbFound = nullptr) const;

    /* Replaces in place, or appends to the section (creating it if needed). */
    void Set(const QString& InSection, const QString& InKey, const QString& InValue);

    /* Every Section / Key pair in the file, in file order. */
    QList<QPair<QString, QString>> GetKeys() const;

    const QString& GetError() const { return Error; }

private:
    struct KeyLine
    {
        int     Line = -1;
        QString Section;
        QString Key;
        QString Value;
    };

    /* Re-reads Keys and Sections from Lines. */
    void Index();
    int  FindKey(const QString& InSection, const QString& InKey) const;

    static QString QuoteIfNeeded(const QString& InValue);

    QStringList    Lines;
    QList<KeyLine> Keys;
    QList<QPair<QString, int>> Sections;   // name, header line
    bool           bCrlf = false;
    mutable QString Error;   // Save is const
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_CONFIGINIFILE_H
