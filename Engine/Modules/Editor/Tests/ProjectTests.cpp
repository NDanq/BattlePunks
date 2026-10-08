/*
 * NimbleEditorProjectTests -- File > New Project's generator (ProjectTemplate.h),
 * from the real Engine/Templates/Game into a temporary folder; and
 * Engine/Binaries/NewProject.cmake, which must make the same project.
 */

#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/Project/ProjectTemplate.h"

using namespace EditorProject;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static QString ReadText(const QString& InPath)
{
    QFile File(InPath);
    return File.open(QIODevice::ReadOnly) ? QString::fromUtf8(File.readAll()) : QString();
}

static void TestNames()
{
    Expect(ValidateName(QStringLiteral("Asteroids")).isEmpty(), "a plain name is valid");
    Expect(ValidateName(QStringLiteral("My_Game2")).isEmpty(), "digits and _ after the first letter are valid");
    Expect(!ValidateName(QString()).isEmpty(), "an empty name is not");
    Expect(!ValidateName(QStringLiteral("2Fast")).isEmpty(), "a leading digit is not");
    Expect(!ValidateName(QStringLiteral("My Game")).isEmpty(), "a space is not");
    Expect(!ValidateName(QStringLiteral("Über")).isEmpty(), "a non-ASCII letter is not");
    Expect(!ValidateName(QStringLiteral("engine")).isEmpty(), "an engine name is not, in any case");
}

static void TestEngineDir()
{
    Expect(EngineDirForCMake(QStringLiteral("/Repo/Games/Asteroids"), QStringLiteral("/Repo/Engine"))
               == QStringLiteral("${CMAKE_CURRENT_SOURCE_DIR}/../../Engine"),
           "a project in Games/ reaches the engine relatively, as Sandbox does");
}

static void TestCreate(const QDir& InRoot)
{
    const QString Template = GetGameTemplateDir();
    const QString Engine   = GetEngineDir();
    Expect(!Template.isEmpty() && QFileInfo(Template).isDir(), "the template is found");
    Expect(!Engine.isEmpty() && QFileInfo(QDir(Engine).filePath(QStringLiteral("CMakeLists.txt"))).isFile(), "the engine is found");

    QString     ProjectDir, Error;
    QStringList Files;
    Expect(CreateProject(Template, Engine, InRoot.path(), QStringLiteral("Asteroids"), ProjectDir, Error, &Files),
           "a project is created");
    Expect(Error.isEmpty(), "with no error");
    const QDir Project(ProjectDir);
    Expect(Project.dirName() == QStringLiteral("Asteroids"), "in <parent>/<Name>");

    for(const char* Expected : { "CMakeLists.txt", "CMakePresets.json", ".gitignore", "README.md",
                                 "AsteroidsGame.cpp", "Config/DefaultEngine.ini", "Scripts/Main.lua", "Docs/GettingStarted.md",
                                 "Content/Materials/.gitkeep", "Content/Prefabs/.gitkeep", "Content/Levels/Asteroids.nmworld" })
    {
        const bool bThere = QFileInfo(Project.filePath(QString::fromLatin1(Expected))).isFile();
        Expect(bThere && Files.contains(QString::fromLatin1(Expected)), Expected);
    }

    /* No token survives, in any file or file name. */
    bool bClean = true;
    QDirIterator It(ProjectDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while(It.hasNext())
    {
        const QString Path = It.next();
        const QString Text = ReadText(Path);
        if(Path.contains(QStringLiteral("__PROJECT__")) || Text.contains(QStringLiteral("@PROJECT_NAME@"))
           || Text.contains(QStringLiteral("@ENGINE_DIR@")))
        {
            bClean = false;
            std::printf("  token left in %s\n", qPrintable(Path));
        }
    }
    Expect(bClean, "every token is replaced");

    const QString Game = ReadText(Project.filePath(QStringLiteral("AsteroidsGame.cpp")));
    Expect(Game.contains(QStringLiteral("class AsteroidsGameMode : public NimbleEngine::IGameMode")), "the game mode is named for the game");
    Expect(Game.contains(QStringLiteral("class AsteroidsGameState : public NimbleEngine::GameState")), "and the game state");
    Expect(Game.contains(QStringLiteral("NIMBLE_IMPLEMENT_GAME_MODULE(AsteroidsGameModule, \"AsteroidsGame\")")), "the module is AsteroidsGame");
    Expect(Game.contains(QStringLiteral("RegisterGameMode<AsteroidsGameMode>(\"Asteroids\")")), "it registers the mode by the game's name");

    /* The world list, not C++, makes the start world a level. */
    const QString Ini = ReadText(Project.filePath(QStringLiteral("Config/DefaultEngine.ini")));
    Expect(Ini.contains(QStringLiteral("[World_Asteroids]\nFile=Content/Levels/Asteroids.nmworld\nGameMode=Asteroids\n"
                                       "StartupScript=Scripts/Main.lua")),
           "the config lists the start world, naming the mode and its startup script");
    Expect(!Ini.contains(QStringLiteral("[Scripting]")), "there is no project-wide startup script");

    const QString Config = ReadText(Project.filePath(QStringLiteral("Config/DefaultEngine.ini")));
    Expect(Config.contains(QStringLiteral("Module=AsteroidsGame")) && Config.contains(QStringLiteral("StartLevel=Asteroids")),
           "the config names the module and the start level");

    /* The engine dir CMake will read resolves to the real engine. */
    const QString Lists = ReadText(Project.filePath(QStringLiteral("CMakeLists.txt")));
    const QString Prefix = QStringLiteral("set(GAME_ENGINE_DIR \"");
    const int     Start  = Lists.indexOf(Prefix) + Prefix.size();
    QString EngineDir = Lists.mid(Start, Lists.indexOf(QLatin1Char('"'), Start) - Start);
    EngineDir.replace(QStringLiteral("${CMAKE_CURRENT_SOURCE_DIR}"), ProjectDir);
    Expect(QFileInfo(QDir(EngineDir).filePath(QStringLiteral("CMakeLists.txt"))).canonicalFilePath()
               == QFileInfo(QDir(Engine).filePath(QStringLiteral("CMakeLists.txt"))).canonicalFilePath(),
           "GAME_ENGINE_DIR is the engine, from a project outside Games/");

    /* A second time: refused, and the first is left alone. */
    QString Again;
    Expect(!CreateProject(Template, Engine, InRoot.path(), QStringLiteral("Asteroids"), Again, Error) && !Error.isEmpty(),
           "an existing folder is refused");
    Expect(QFileInfo(Project.filePath(QStringLiteral("AsteroidsGame.cpp"))).isFile(), "and kept");

    Expect(!CreateProject(Template, Engine, InRoot.path(), QStringLiteral("Bad Name"), Again, Error), "an invalid name is refused");
    Expect(!QFileInfo::exists(InRoot.filePath(QStringLiteral("Bad Name"))), "and nothing is written");

    Expect(!CreateProject(InRoot.filePath(QStringLiteral("NoTemplate")), Engine, InRoot.path(), QStringLiteral("Other"), Again, Error),
           "a missing template is refused");
    Expect(!QFileInfo::exists(InRoot.filePath(QStringLiteral("Other"))), "and nothing is written");
}

/* Every file below InDir, relative, sorted. */
static QStringList ListFiles(const QString& InDir)
{
    QStringList Files;
    QDirIterator It(InDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while(It.hasNext()) { Files << QDir(InDir).relativeFilePath(It.next()); }
    Files.sort();
    return Files;
}

static QByteArray ReadBytes(const QString& InPath)
{
    QFile File(InPath);
    return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
}

static void TestScriptMatchesEditor(const QDir& InRoot)
{
    const QString EditorDir = InRoot.filePath(QStringLiteral("Asteroids"));   // TestCreate's
    const QString ScriptParent = InRoot.filePath(QStringLiteral("Script"));

    QProcess Script;
    Script.setProcessChannelMode(QProcess::MergedChannels);
    Script.start(QStringLiteral(NIMBLEEDITOR_CMAKE_COMMAND),
                 { QStringLiteral("-DNAME=Asteroids"), QStringLiteral("-DLOCATION=") + ScriptParent,
                   QStringLiteral("-P"), QStringLiteral(NIMBLEEDITOR_NEW_PROJECT_SCRIPT) });
    const bool bRan = Script.waitForFinished(60000) && Script.exitStatus() == QProcess::NormalExit && Script.exitCode() == 0;
    if(!bRan) { std::printf("%s\n", Script.readAll().constData()); }
    Expect(bRan, "NewProject.cmake runs");

    /* One level deeper than the editor's copy, so @ENGINE_DIR@ differs by
     * exactly one "../"; everything else must be identical. */
    const QString ScriptDir = QDir(ScriptParent).filePath(QStringLiteral("Asteroids"));
    const QStringList Files = ListFiles(EditorDir);
    Expect(!Files.isEmpty() && Files == ListFiles(ScriptDir), "the script writes the same files as the editor");

    bool bSame = true;
    for(const QString& File : Files)
    {
        QByteArray Mine = ReadBytes(QDir(ScriptDir).filePath(File));
        Mine.replace("${CMAKE_CURRENT_SOURCE_DIR}/../", "${CMAKE_CURRENT_SOURCE_DIR}/");
        if(Mine != ReadBytes(QDir(EditorDir).filePath(File)))
        {
            bSame = false;
            std::printf("  differs: %s\n", qPrintable(File));
        }
    }
    Expect(bSame, "with the same contents");

    Script.start(QStringLiteral(NIMBLEEDITOR_CMAKE_COMMAND),
                 { QStringLiteral("-DNAME=Bad Name"), QStringLiteral("-DLOCATION=") + ScriptParent,
                   QStringLiteral("-P"), QStringLiteral(NIMBLEEDITOR_NEW_PROJECT_SCRIPT) });
    Expect(Script.waitForFinished(60000) && Script.exitCode() != 0, "the script refuses an invalid name");
    Expect(ListFiles(ScriptParent).size() == Files.size(), "and writes nothing");
}

/* Project Settings > Worlds rewrites the [World_*] sections as a list. */
static void TestWorldSections(const QDir& InRoot)
{
    std::printf("[world sections]\n");
    const QString Path = InRoot.filePath(QStringLiteral("Worlds.ini"));
    {
        QFile Out(Path);
        Out.open(QIODevice::WriteOnly);
        Out.write("[Game]\nStartLevel=A\n\n; the worlds\n[World_A]\nFile=Content/Levels/A.nmworld\nGameMode=Arena\n\n"
                  "[World_B]\nFile=Content/Levels/B.nmworld\nCook=false\n\n; keep me\n[Scripting]\nStartupScript=Main.lua\n");
    }

    ConfigIniFile File;
    Expect(File.Load(Path), "the file loads");
    const QList<ConfigIniFile::Section> Read = File.GetSections(QStringLiteral("world_"));
    Expect(Read.size() == 2 && Read[0].Name == QStringLiteral("World_A") && Read[1].Name == QStringLiteral("World_B"),
           "the world sections are read in order");
    Expect(Read.size() == 2 && Read[0].Keys.size() == 2 && Read[0].Keys[1].second == QStringLiteral("Arena"),
           "with their keys");

    ConfigIniFile::Section C;
    C.Name = QStringLiteral("World_C");
    C.Keys = { { QStringLiteral("File"), QStringLiteral("Content/Levels/C.nmworld") }, { QStringLiteral("GameMode"), QString() } };
    QList<ConfigIniFile::Section> Next = { C, Read[0] };
    File.ReplaceSections(QStringLiteral("World_"), Next);
    Expect(File.Save(Path), "the file saves");

    ConfigIniFile Again;
    Again.Load(Path);
    const QList<ConfigIniFile::Section> Written = Again.GetSections(QStringLiteral("World_"));
    Expect(Written.size() == 2 && Written[0].Name == QStringLiteral("World_C") && Written[1].Name == QStringLiteral("World_A"),
           "the list is replaced, in its new order, B gone");
    bool bFound = false;
    Expect(Again.Get(QStringLiteral("World_C"), QStringLiteral("GameMode"), &bFound).isEmpty() && bFound,
           "an empty value is written, not dropped");
    Expect(Again.Get(QStringLiteral("Scripting"), QStringLiteral("StartupScript")) == QStringLiteral("Main.lua")
           && Again.Get(QStringLiteral("Game"), QStringLiteral("StartLevel")) == QStringLiteral("A"),
           "other sections are untouched");
    const QString Text = ReadText(Path);
    Expect(Text.contains(QStringLiteral("; the worlds\n[World_C]")), "written where the old list was, its comment above it kept");
    Expect(Text.contains(QStringLiteral("; keep me\n[Scripting]")), "the next section's comment is kept");
}

int main(int argc, char** argv)
{
    QCoreApplication App(argc, argv);
    std::printf("Editor project tests\n");

    QTemporaryDir Temp;
    const QDir Root(Temp.path());

    TestNames();
    TestEngineDir();
    TestCreate(Root);
    TestScriptMatchesEditor(Root);
    TestWorldSections(Root);

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
