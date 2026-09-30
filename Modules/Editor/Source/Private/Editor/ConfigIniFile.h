#ifndef NIMBLEEDITOR_CONFIGINIFILE_H
#define NIMBLEEDITOR_CONFIGINIFILE_H

#if WITH_EDITOR

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

/*
 * An .ini in Core's config grammar (CoreConfig.cpp's LoadConfigText), edited
 * in place: Set changes one key's line and leaves every other line -- the
 * comments that explain each key, blank lines, ordering -- exactly as it was.
 * A config file here is documentation as much as data (see Sandbox's
 * DefaultEngine.ini), so rewriting it from the registry would lose the part
 * people read.
 *
 * The grammar, as Core reads it: whole-line comments starting ';' or '#',
 * "[Section]" headers, "Key = Value" with the value optionally in double
 * quotes. No inline comments. Sections and keys match case-insensitively.
 *
 * Private to the editor.
 */
class ConfigIniFile
{
public:
    /* False if the file exists but cannot be read; a missing file is an
     * empty document, so Save creates it. */
    bool Load(const QString& InPath);

    /* Writes every line back, with the line ending the file had. */
    bool Save(const QString& InPath) const;

    /* The value as Core would read it (quotes stripped); OutbFound false and
     * an empty string when the key is not in the file. */
    QString Get(const QString& InSection, const QString& InKey, bool* OutbFound = nullptr) const;

    /* Replaces the key's value on its own line, or adds the key at the end of
     * its section -- adding the section at the end of the file if needed. */
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
