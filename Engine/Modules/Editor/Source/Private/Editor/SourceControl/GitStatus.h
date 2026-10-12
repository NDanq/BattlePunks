#ifndef NIMBLEEDITOR_GITSTATUS_H
#define NIMBLEEDITOR_GITSTATUS_H

#include <cstdint>
#include <string>
#include <vector>

/*
 * `git status --porcelain=v2 --branch -z`, parsed, and the Source Control
 * dock's path groups. Plain C++, no Qt: NimbleEditorSourceControlTests runs
 * it on fixtures.
 *
 * Written from the documented format:
 * Derived from: git status, porcelain format v2 (spec) https://git-scm.com/docs/git-status#_porcelain_format_version_2
 *   @ git 2.x -- the record layout only; no git code.
 */
namespace EditorGit
{
    struct BranchInfo
    {
        std::string Oid;        // empty before the first commit
        std::string Head;       // the branch; empty when detached
        std::string Upstream;   // empty without one
        int         Ahead  = 0;
        int         Behind = 0;
        bool        bInitial     = false;   // "(initial)": no commit yet
        bool        bDetached    = false;   // "(detached)"
        bool        bHasUpstream = false;
        bool        bHasAheadBehind = false;   // "# branch.ab" was given (the upstream exists)
    };

    enum class EEntryKind : uint8_t
    {
        Ordinary,   // "1": changed
        Renamed,    // "2": renamed or copied
        Unmerged,   // "u": a conflict
        Untracked,  // "?"
        Ignored     // "!"
    };

    struct FileEntry
    {
        EEntryKind  Kind = EEntryKind::Ordinary;
        char        Index    = '.';   // X: the staged change ('.' none; M A D R C T U)
        char        Worktree = '.';   // Y: the unstaged change
        std::string Path;             // repository-relative, '/' separated
        std::string OrigPath;         // Renamed: where it came from
        bool        bSubmodule = false;

        bool IsStaged() const;        // something in the index
        bool IsUnstaged() const;      // something in the working tree, or untracked
        bool IsConflicted() const { return Kind == EEntryKind::Unmerged; }

        /* One letter for the list: the staged change if any, else the
         * working tree's; '?' untracked, 'U' conflicted. */
        char DisplayStatus() const;
    };

    struct Status
    {
        BranchInfo             Branch;
        std::vector<FileEntry> Entries;
    };

    /* InOutput: the command's stdout, NUL-terminated records. False, with
     * OutError, on a record that isn't in the format. */
    bool ParseStatusV2(const std::string& InOutput, Status& OutStatus, std::string* OutError = nullptr);

    /* The dock's groups, by where a path lives. */
    enum class EPathGroup : uint8_t
    {
        Content,      // <Game>/Content/**
        Scripts,      // <Game>/Scripts/**
        GameModule,   // <Game>/Source/**, <Game>/CMakeLists.txt, <Game>/CMakePresets.json
        Config,       // <Game>/Config/**
        Engine,       // Engine/**: not supported during a session
        Other,        // anything else
        Count
    };

    const char* GetGroupName(EPathGroup InGroup);

    /* InPath and InGameDir repository-relative, '/' separated; InGameDir is
     * the game's folder ("Games/WorldsLab"), or empty when the game is the
     * repository's root. Case-insensitive, as on Windows. */
    EPathGroup ClassifyPath(const std::string& InPath, const std::string& InGameDir);
}

#endif //NIMBLEEDITOR_GITSTATUS_H
