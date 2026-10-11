#include "Private/Editor/SourceControl/GitStatus.h"

#include <cctype>
#include <cstdlib>

namespace
{
    /* The next NUL-terminated record from InOffset; false at the end. */
    bool NextRecord(const std::string& InText, size_t& InOutOffset, std::string& OutRecord)
    {
        if(InOutOffset >= InText.size())
        {
            return false;
        }
        const size_t End = InText.find('\0', InOutOffset);
        if(End == std::string::npos)
        {
            OutRecord   = InText.substr(InOutOffset);
            InOutOffset = InText.size();
        }
        else
        {
            OutRecord   = InText.substr(InOutOffset, End - InOutOffset);
            InOutOffset = End + 1;
        }
        return true;
    }

    /* InCount space-separated fields, then the rest (a path, which may hold
     * spaces) in OutRest. */
    bool SplitFields(const std::string& InRecord, const size_t InCount, std::vector<std::string>& OutFields, std::string& OutRest)
    {
        OutFields.clear();
        size_t Start = 0;
        for(size_t Field = 0; Field < InCount; ++Field)
        {
            const size_t Space = InRecord.find(' ', Start);
            if(Space == std::string::npos)
            {
                return false;
            }
            OutFields.push_back(InRecord.substr(Start, Space - Start));
            Start = Space + 1;
        }
        OutRest = InRecord.substr(Start);
        return !OutRest.empty();
    }

    bool SetXY(const std::string& InXY, EditorGit::FileEntry& OutEntry)
    {
        if(InXY.size() != 2)
        {
            return false;
        }
        OutEntry.Index    = InXY[0];
        OutEntry.Worktree = InXY[1];
        return true;
    }

    bool Fail(std::string* OutError, const std::string& InWhy)
    {
        if(OutError)
        {
            *OutError = InWhy;
        }
        return false;
    }

    bool StartsWith(const std::string& InText, const char* InPrefix)
    {
        return InText.rfind(InPrefix, 0) == 0;
    }

    std::string Lower(std::string InText)
    {
        for(char& Each : InText)
        {
            Each = char(std::tolower(static_cast<unsigned char>(Each)));
        }
        return InText;
    }
}

namespace EditorGit
{
    bool FileEntry::IsStaged() const
    {
        return Kind == EEntryKind::Ordinary || Kind == EEntryKind::Renamed ? Index != '.' : false;
    }

    bool FileEntry::IsUnstaged() const
    {
        switch(Kind)
        {
            case EEntryKind::Ordinary:
            case EEntryKind::Renamed:   return Worktree != '.';
            case EEntryKind::Untracked: return true;
            case EEntryKind::Unmerged:  return true;
            case EEntryKind::Ignored:   return false;
        }
        return false;
    }

    char FileEntry::DisplayStatus() const
    {
        switch(Kind)
        {
            case EEntryKind::Untracked: return '?';
            case EEntryKind::Ignored:   return '!';
            case EEntryKind::Unmerged:  return 'U';
            default: break;
        }
        return Index != '.' ? Index : Worktree;
    }

    bool ParseStatusV2(const std::string& InOutput, Status& OutStatus, std::string* OutError)
    {
        OutStatus = Status();
        size_t      Offset = 0;
        std::string Record;
        std::vector<std::string> Fields;
        std::string Rest;

        while(NextRecord(InOutput, Offset, Record))
        {
            if(Record.empty())
            {
                continue;
            }

            if(Record[0] == '#')
            {
                /* "# branch.<name> <value>"; other headers (stash) are skipped. */
                if(StartsWith(Record, "# branch.oid "))
                {
                    const std::string Value = Record.substr(13);
                    OutStatus.Branch.bInitial = Value == "(initial)";
                    OutStatus.Branch.Oid      = OutStatus.Branch.bInitial ? std::string() : Value;
                }
                else if(StartsWith(Record, "# branch.head "))
                {
                    const std::string Value = Record.substr(14);
                    OutStatus.Branch.bDetached = Value == "(detached)";
                    OutStatus.Branch.Head      = OutStatus.Branch.bDetached ? std::string() : Value;
                }
                else if(StartsWith(Record, "# branch.upstream "))
                {
                    OutStatus.Branch.Upstream     = Record.substr(18);
                    OutStatus.Branch.bHasUpstream = !OutStatus.Branch.Upstream.empty();
                }
                else if(StartsWith(Record, "# branch.ab "))
                {
                    /* "+<ahead> -<behind>" */
                    const std::string Value = Record.substr(12);
                    const size_t Space = Value.find(' ');
                    if(Space == std::string::npos || Value.size() < 4 || Value[0] != '+' || Value[Space + 1] != '-')
                    {
                        return Fail(OutError, "bad branch.ab header: " + Record);
                    }
                    OutStatus.Branch.Ahead  = std::atoi(Value.c_str() + 1);
                    OutStatus.Branch.Behind = std::atoi(Value.c_str() + Space + 2);
                    OutStatus.Branch.bHasAheadBehind = true;
                }
                continue;
            }

            FileEntry Entry;
            switch(Record[0])
            {
                case '1':
                    /* 1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path> */
                    if(!SplitFields(Record, 8, Fields, Rest) || !SetXY(Fields[1], Entry))
                    {
                        return Fail(OutError, "bad changed entry: " + Record);
                    }
                    Entry.Kind       = EEntryKind::Ordinary;
                    Entry.bSubmodule = Fields[2].size() == 4 && Fields[2][0] == 'S';
                    Entry.Path       = Rest;
                    break;

                case '2':
                    /* 2 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <X><score> <path>, then <origPath> as its own record */
                    if(!SplitFields(Record, 9, Fields, Rest) || !SetXY(Fields[1], Entry))
                    {
                        return Fail(OutError, "bad renamed entry: " + Record);
                    }
                    Entry.Kind       = EEntryKind::Renamed;
                    Entry.bSubmodule = Fields[2].size() == 4 && Fields[2][0] == 'S';
                    Entry.Path       = Rest;
                    if(!NextRecord(InOutput, Offset, Entry.OrigPath) || Entry.OrigPath.empty())
                    {
                        return Fail(OutError, "renamed entry without its original path: " + Record);
                    }
                    break;

                case 'u':
                    /* u <XY> <sub> <m1> <m2> <m3> <mW> <h1> <h2> <h3> <path> */
                    if(!SplitFields(Record, 10, Fields, Rest) || !SetXY(Fields[1], Entry))
                    {
                        return Fail(OutError, "bad unmerged entry: " + Record);
                    }
                    Entry.Kind = EEntryKind::Unmerged;
                    Entry.Path = Rest;
                    break;

                case '?':
                case '!':
                    if(Record.size() < 3 || Record[1] != ' ')
                    {
                        return Fail(OutError, "bad untracked entry: " + Record);
                    }
                    Entry.Kind     = Record[0] == '?' ? EEntryKind::Untracked : EEntryKind::Ignored;
                    Entry.Index    = '.';
                    Entry.Worktree = Record[0];
                    Entry.Path     = Record.substr(2);
                    break;

                default:
                    return Fail(OutError, "unknown entry: " + Record);
            }
            OutStatus.Entries.push_back(std::move(Entry));
        }
        return true;
    }

    const char* GetGroupName(const EPathGroup InGroup)
    {
        switch(InGroup)
        {
            case EPathGroup::Content:    return "Content";
            case EPathGroup::Scripts:    return "Scripts";
            case EPathGroup::GameModule: return "GameModule source";
            case EPathGroup::Config:     return "Config";
            case EPathGroup::Engine:     return "Engine";
            case EPathGroup::Other:      return "Other";
            case EPathGroup::Count:      break;
        }
        return "";
    }

    EPathGroup ClassifyPath(const std::string& InPath, const std::string& InGameDir)
    {
        const std::string Path = Lower(InPath);
        if(StartsWith(Path, "engine/"))
        {
            return EPathGroup::Engine;
        }

        std::string Game = Lower(InGameDir);
        while(!Game.empty() && Game.back() == '/')
        {
            Game.pop_back();
        }
        if(!Game.empty())
        {
            Game += '/';
            if(!StartsWith(Path, Game.c_str()))
            {
                return EPathGroup::Other;
            }
        }
        const std::string Local = Path.substr(Game.size());

        if(StartsWith(Local, "content/"))  { return EPathGroup::Content; }
        if(StartsWith(Local, "scripts/"))  { return EPathGroup::Scripts; }
        if(StartsWith(Local, "config/"))   { return EPathGroup::Config; }
        if(StartsWith(Local, "source/") || Local == "cmakelists.txt" || Local == "cmakepresets.json")
        {
            return EPathGroup::GameModule;
        }
        return EPathGroup::Other;
    }
}
