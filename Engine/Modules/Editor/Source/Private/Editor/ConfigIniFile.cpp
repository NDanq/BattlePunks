#if WITH_EDITOR

#include "Private/Editor/ConfigIniFile.h"

#include <algorithm>

#include <QFile>
#include <QSaveFile>

namespace
{
    QString StripQuotes(const QString& InValue)
    {
        if(InValue.size() >= 2 && InValue.startsWith(QLatin1Char('"')) && InValue.endsWith(QLatin1Char('"')))
        {
            return InValue.mid(1, InValue.size() - 2);
        }
        return InValue;
    }

    bool Same(const QString& InA, const QString& InB)
    {
        return InA.compare(InB, Qt::CaseInsensitive) == 0;
    }
}

bool ConfigIniFile::Load(const QString& InPath)
{
    Lines.clear();
    Error.clear();
    bCrlf = false;

    QFile File(InPath);
    if(File.exists())
    {
        if(!File.open(QIODevice::ReadOnly))
        {
            Error = File.errorString();
            Index();
            return false;
        }
        const QString Text = QString::fromUtf8(File.readAll());
        bCrlf = Text.contains(QStringLiteral("\r\n"));
        Lines = QString(Text).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).split(QLatin1Char('\n'));
        /* A trailing newline is a line ending, not an empty last line. */
        if(!Lines.isEmpty() && Lines.back().isEmpty())
        {
            Lines.removeLast();
        }
    }
    Index();
    return true;
}

bool ConfigIniFile::Save(const QString& InPath) const
{
    const QString Eol  = bCrlf ? QStringLiteral("\r\n") : QStringLiteral("\n");
    const QString Text = Lines.join(Eol) + Eol;

    QSaveFile File(InPath);
    const QByteArray Bytes = Text.toUtf8();
    if(!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit())
    {
        Error = File.errorString();
        return false;
    }
    return true;
}

void ConfigIniFile::Index()
{
    Keys.clear();
    Sections.clear();

    QString Section;
    for(int Index = 0; Index < Lines.size(); ++Index)
    {
        const QString Line = Lines[Index].trimmed();
        if(Line.isEmpty() || Line.startsWith(QLatin1Char(';')) || Line.startsWith(QLatin1Char('#')))
        {
            continue;
        }
        if(Line.startsWith(QLatin1Char('[')))
        {
            const int Close = Line.indexOf(QLatin1Char(']'));
            if(Close > 0)
            {
                Section = Line.mid(1, Close - 1).trimmed();
                Sections.append({ Section, Index });
            }
            continue;
        }
        const int Eq = Line.indexOf(QLatin1Char('='));
        if(Eq <= 0)
        {
            continue;
        }
        KeyLine Key;
        Key.Line    = Index;
        Key.Section = Section;
        Key.Key     = Line.left(Eq).trimmed();
        Key.Value   = StripQuotes(Line.mid(Eq + 1).trimmed());
        Keys.append(Key);
    }
}

int ConfigIniFile::FindKey(const QString& InSection, const QString& InKey) const
{
    /* The last one wins, as it does when Core reads the file. */
    for(int Index = Keys.size() - 1; Index >= 0; --Index)
    {
        if(Same(Keys[Index].Section, InSection) && Same(Keys[Index].Key, InKey))
        {
            return Index;
        }
    }
    return -1;
}

QString ConfigIniFile::Get(const QString& InSection, const QString& InKey, bool* OutbFound) const
{
    const int Found = FindKey(InSection, InKey);
    if(OutbFound)
    {
        *OutbFound = Found >= 0;
    }
    return Found >= 0 ? Keys[Found].Value : QString();
}

QString ConfigIniFile::QuoteIfNeeded(const QString& InValue)
{
    /* Core trims values and strips one pair of quotes, so a value that would
     * lose either needs quoting to come back the same. */
    const bool bNeeds = InValue != InValue.trimmed()
                     || (InValue.startsWith(QLatin1Char('"')) && InValue.endsWith(QLatin1Char('"')) && InValue.size() >= 2);
    return bNeeds ? QLatin1Char('"') + InValue + QLatin1Char('"') : InValue;
}

void ConfigIniFile::Set(const QString& InSection, const QString& InKey, const QString& InValue)
{
    const QString Value = QuoteIfNeeded(InValue);

    const int Found = FindKey(InSection, InKey);
    if(Found >= 0)
    {
        /* Keep the line's own spelling and spacing up to the value. */
        QString&  Line = Lines[Keys[Found].Line];
        const int Eq   = Line.indexOf(QLatin1Char('='));
        const bool bSpaced = Eq + 1 < Line.size() && Line[Eq + 1] == QLatin1Char(' ');
        Line = Line.left(Eq + 1) + (bSpaced ? QStringLiteral(" ") : QString()) + Value;
        Index();
        return;
    }

    const QString NewLine = InKey + QLatin1Char('=') + Value;

    /* After the section's last key (or its header), so comments above the
     * next section stay put. The last header of a section wins. */
    int Header = -1;
    for(const QPair<QString, int>& Section : Sections)
    {
        if(Same(Section.first, InSection))
        {
            Header = Section.second;
        }
    }
    if(Header >= 0)
    {
        int After = Header;
        for(const KeyLine& Key : Keys)
        {
            if(Key.Line > Header && Same(Key.Section, InSection))
            {
                After = std::max(After, Key.Line);
            }
        }
        Lines.insert(After + 1, NewLine);
    }
    else
    {
        if(!Lines.isEmpty() && !Lines.back().trimmed().isEmpty())
        {
            Lines.append(QString());
        }
        Lines.append(QLatin1Char('[') + InSection + QLatin1Char(']'));
        Lines.append(NewLine);
    }
    Index();
}

QList<QPair<QString, QString>> ConfigIniFile::GetKeys() const
{
    QList<QPair<QString, QString>> Out;
    for(const KeyLine& Key : Keys)
    {
        Out.append({ Key.Section, Key.Key });
    }
    return Out;
}

#endif // WITH_EDITOR
