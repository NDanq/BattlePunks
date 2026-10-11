#ifndef COLLAB_COLLABPROCESS_H
#define COLLAB_COLLABPROCESS_H

#include <cstdint>
#include <string>
#include <vector>

/*
 * Child processes for the server and the content worker, without Qt:
 * CreateProcessW on Windows, fork/exec on POSIX. Arguments are passed as a
 * list (quoted for Windows here), never through a shell.
 */
namespace Collab
{
    struct ProcessResult
    {
        bool        bStarted = false;
        int         ExitCode = -1;
        std::string Output;   // stdout and stderr together, as written
    };

    /* Runs InArgs[0] with the rest, in InWorkingDir (empty: this process's),
     * and waits. InEnvironment: "NAME=value" entries added to this process's. */
    ProcessResult RunProcess(const std::vector<std::string>& InArgs, const std::string& InWorkingDir = std::string(),
                             const std::vector<std::string>& InEnvironment = {});

    /* A running child, not waited on. */
    struct ChildProcess
    {
        uintptr_t Handle = 0;   // HANDLE / pid
        bool IsValid() const { return Handle != 0; }
    };

    /* Starts InArgs without waiting; its output goes where this process's does.
     * Children of both kinds end when this process does, however it ends (a
     * kill-on-close job on Windows, PR_SET_PDEATHSIG on Linux). On Linux that
     * is when the calling thread ends, so spawn from a thread that lives as
     * long as the child should. */
    ChildProcess SpawnProcess(const std::vector<std::string>& InArgs, const std::string& InWorkingDir = std::string());

    bool IsProcessRunning(const ChildProcess& InChild);

    /* Ends it, waits for it, and forgets it. */
    void KillProcess(ChildProcess& InOutChild);

    /* This executable's folder, for finding its siblings. */
    std::string GetExecutableDir();
}

#endif //COLLAB_COLLABPROCESS_H
