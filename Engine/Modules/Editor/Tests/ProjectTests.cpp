/*
 * NimbleEditorProjectTests -- File > New Project's generator (ProjectTemplate.h),
 * from the real Engine/Templates/Game into a temporary folder; and
 * Engine/Binaries/NewProject.cmake, which must make the same project. And
 * example projects: Tests/Data/Examples (the Tiny example, packed by
 * Engine/Examples/pack_example.py) unpacked and renamed by NewProject.cmake
 * -DEXAMPLE=, as the wizard runs it.
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
#include "Private/Editor/Project/PluginTemplate.h"
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
    Expect(Game.contains(QStringLiteral("NIMBLE_REGISTER_GAME_MODE(AsteroidsGameMode, \"Asteroids\")")), "it registers the mode by the game's name");

    /* The world list, not C++, makes the start world a level. */
    const QString Ini = ReadText(Project.filePath(QStringLiteral("Config/DefaultEngine.ini")));
    Expect(Ini.contains(QStringLiteral("[World_Asteroids]\nFile=Content/Levels/Asteroids.nmworld\n"
                                       "StartupScript=Scripts/Main.lua")),
           "the config lists the start world and its startup script");
    const QString World = ReadText(Project.filePath(QStringLiteral("Content/Levels/Asteroids.nmworld")));
    Expect(World.contains(QStringLiteral("<Settings GameMode=\"Asteroids\" />")), "the start world names the mode");
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

/* File > New Plugin: every starter, made by the editor and by
 * Engine/Binaries/NewPlugin.cmake, must come out byte for byte the same. */
static void TestPluginTemplates(const QDir& InRoot)
{
    std::printf("[plugin templates]\n");
    const QString Template = EditorPlugins::GetPluginTemplateDir();
    const QString Engine   = GetEngineDir();
    const QString Version  = EditorPlugins::ReadEngineVersion(Engine);
    Expect(!Template.isEmpty(), "the plugin template is found");
    Expect(!Version.isEmpty(), "the engine's version is read");
    const QStringList Starters = EditorPlugins::GetStarters(Template);
    Expect(Starters.contains(QStringLiteral("Empty")) && Starters.contains(QStringLiteral("Component"))
           && Starters.contains(QStringLiteral("RenderPass")) && !Starters.contains(QStringLiteral("Common")), "the three starters, not Common");

    struct Case { const char* Name; const char* Starter; bool bAll; };
    const Case Cases[] = { { "PlainEmpty", "Empty", false }, { "FullEmpty", "Empty", true },
                           { "PlainComponent", "Component", false }, { "FullComponent", "Component", true },
                           { "FullRenderPass", "RenderPass", true } };
    const QString EditorParent = InRoot.filePath(QStringLiteral("PluginsEditor"));
    const QString ScriptParent = InRoot.filePath(QStringLiteral("PluginsScript"));
    for(const Case& Each : Cases)
    {
        EditorPlugins::NewPluginOptions Options;
        Options.Name          = QString::fromLatin1(Each.Name);
        Options.Starter       = QString::fromLatin1(Each.Starter);
        Options.Category      = Each.bAll ? QStringLiteral("World Building") : QString();
        Options.Description   = Each.bAll ? QStringLiteral("Line one\nline two") : QString();
        Options.EngineVersion = Version;
        Options.bTest = Options.bEditor = Options.bContent = Each.bAll;
        QString PluginDir, Error;
        QStringList Files;
        const bool bMade = EditorPlugins::CreatePlugin(Template, EditorParent, Options, {}, PluginDir, Error, &Files);
        if(!bMade) { std::printf("  %s\n", qPrintable(Error)); }
        Expect(bMade, "the editor makes the plugin");

        QStringList Args = { QStringLiteral("-DNAME=") + Options.Name, QStringLiteral("-DLOCATION=") + ScriptParent,
                             QStringLiteral("-DSTARTER=") + Options.Starter };
        Args << (Each.bAll ? QStringList{ QStringLiteral("-DTEST=ON"), QStringLiteral("-DEDITOR=ON"), QStringLiteral("-DCONTENT=ON"),
                                          QStringLiteral("-DCATEGORY=World Building"), QStringLiteral("-DDESCRIPTION=Line one\nline two") }
                           : QStringList{ QStringLiteral("-DTEST=OFF") });
        Args << QStringLiteral("-P") << QStringLiteral(NIMBLEEDITOR_NEW_PLUGIN_SCRIPT);
        QProcess Script;
        Script.setProcessChannelMode(QProcess::MergedChannels);
        Script.start(QStringLiteral(NIMBLEEDITOR_CMAKE_COMMAND), Args);
        const bool bRan = Script.waitForFinished(60000) && Script.exitStatus() == QProcess::NormalExit && Script.exitCode() == 0;
        if(!bRan) { std::printf("%s\n", Script.readAll().constData()); }
        Expect(bRan, "NewPlugin.cmake makes it too");

        const QString Mine   = QDir(EditorParent).filePath(Options.Name);
        const QString Theirs = QDir(ScriptParent).filePath(Options.Name);
        const QStringList List = ListFiles(Mine);
        Expect(!List.isEmpty() && List == ListFiles(Theirs), "the same files");
        bool bSame = true;
        for(const QString& File : List)
        {
            if(ReadBytes(QDir(Mine).filePath(File)) != ReadBytes(QDir(Theirs).filePath(File)))
            {
                bSame = false;
                std::printf("  differs: %s/%s\n", Each.Name, qPrintable(File));
            }
        }
        Expect(bSame, "byte for byte");
        Expect(!ReadText(QDir(Mine).filePath(Options.Name + QStringLiteral(".nmplugin"))).contains(QLatin1Char('@')),
               "no token or marker is left in the descriptor");
        Expect(List.contains(QStringLiteral("Tests/") + Options.Name + QStringLiteral("Tests.cpp")) == Each.bAll, "Tests/ only with a test");
        Expect(List.contains(QStringLiteral("Source/") + Options.Name + QStringLiteral("Editor/Private/") + Options.Name
                             + QStringLiteral("Editor/") + Options.Name + QStringLiteral("EditorPlugin.cpp")) == Each.bAll,
               "the Editor module only when asked");
    }

    QString PluginDir, Error;
    EditorPlugins::NewPluginOptions Bad;
    Bad.EngineVersion = Version;
    Bad.Name = QStringLiteral("Q");
    Expect(!EditorPlugins::CreatePlugin(Template, EditorParent, Bad, {}, PluginDir, Error), "a one-letter name is refused");
    Bad.Name = QStringLiteral("Has Space");
    Expect(!EditorPlugins::CreatePlugin(Template, EditorParent, Bad, {}, PluginDir, Error), "a space is refused");
    Bad.Name = QStringLiteral("audio");
    Expect(!EditorPlugins::CreatePlugin(Template, EditorParent, Bad, EditorPlugins::GetReservedNames(Engine), PluginDir, Error),
           "an engine module's name is refused, in any case");
    Bad.Name = QStringLiteral("FullComponent");
    Expect(!EditorPlugins::CreatePlugin(Template, EditorParent, Bad, {}, PluginDir, Error) && Error.contains(QStringLiteral("exists")),
           "an existing folder is refused");
    Bad.Name = QStringLiteral("Fine");
    Bad.Category = QStringLiteral("Bad/Category");
    Expect(!EditorPlugins::CreatePlugin(Template, EditorParent, Bad, {}, PluginDir, Error), "a category with symbols is refused");
    Expect(!QFileInfo::exists(QDir(EditorParent).filePath(QStringLiteral("Fine"))), "and nothing is left behind");

    /* SetPluginEnabled: the line, the rest of the file as it was. */
    const QString Config = InRoot.filePath(QStringLiteral("Plugins.ini"));
    {
        QFile Out(Config);
        Out.open(QIODevice::WriteOnly);
        Out.write("; keep me\n[Game]\nModule=X\n");
    }
    Expect(EditorPlugins::SetPluginEnabled(Config, QStringLiteral("Weather"), true, Error), "a plugin is enabled");
    const QString Text = ReadText(Config);
    Expect(Text.startsWith(QStringLiteral("; keep me\n[Game]\nModule=X\n")) && Text.contains(QStringLiteral("[Plugins]"))
           && Text.contains(QStringLiteral("Weather=true")), "as [Plugins] Weather=true, the file kept");
}

/* `cmake <InArgs>`; true if it exits 0. Its output is printed when InExpectSuccess is not met. */
static bool RunCMake(const QStringList& InArgs, const bool bInExpectSuccess = true)
{
    QProcess Process;
    Process.setProcessChannelMode(QProcess::MergedChannels);
    Process.start(QStringLiteral(NIMBLEEDITOR_CMAKE_COMMAND), InArgs);
    const bool bOk = Process.waitForFinished(120000) && Process.exitStatus() == QProcess::NormalExit && Process.exitCode() == 0;
    if(bOk != bInExpectSuccess) { std::printf("%s\n", Process.readAll().constData()); }
    return bOk;
}

static bool CopyTree(const QString& InFrom, const QString& InTo)
{
    for(const QString& File : ListFiles(InFrom))
    {
        const QString Target = QDir(InTo).filePath(File);
        QDir().mkpath(QFileInfo(Target).absolutePath());
        if(!QFile::copy(QDir(InFrom).filePath(File), Target)) { return false; }
        QFile::setPermissions(Target, QFile::permissions(QDir(InFrom).filePath(File)));
    }
    return true;
}

static void TestExamples(const QDir& InRoot)
{
    std::printf("[examples]\n");
    const QString Fixture = QStringLiteral(NIMBLEEDITOR_EXAMPLES_FIXTURE);
    const QString Source  = QStringLiteral(NIMBLEEDITOR_EXAMPLE_PROJECT_FIXTURE);
    const QString Script  = QStringLiteral(NIMBLEEDITOR_NEW_PROJECT_SCRIPT);

    QString Error;
    const QList<ExampleInfo> Examples = ListExamples(Fixture, &Error);
    Expect(Error.isEmpty() && Examples.size() == 1, "the fixture index lists one example");
    if(Examples.size() != 1) { return; }
    const ExampleInfo& Tiny = Examples.front();
    Expect(Tiny.Name == QStringLiteral("Tiny") && Tiny.Title == QStringLiteral("Tiny Example") && !Tiny.Description.isEmpty(),
           "with its name, title and description");
    Expect(Tiny.Files == ListFiles(Source).size() && Tiny.PackedBytes > 0 && Tiny.UnpackedBytes > 0, "its file count and sizes");
    Expect(ListExamples(InRoot.filePath(QStringLiteral("NoExamples")), &Error).isEmpty() && !Error.isEmpty(),
           "a missing index lists nothing, with an error");

    int Index = 0, Count = 0;
    QString What;
    Expect(ParseExampleStep(QStringLiteral("-- NewProject: [2/4] Configuring (cmake --preset x)"), Index, Count, What)
           && Index == 2 && Count == 4 && What == QStringLiteral("Configuring (cmake --preset x)"), "a step line is parsed");
    Expect(!ParseExampleStep(QStringLiteral("-- Configuring done"), Index, Count, What), "any other line is not");
    Expect(GetExampleSteps(true).size() == 4 && GetExampleSteps(false) == QStringList{ QStringLiteral("unpack") },
           "the steps with and without a build");

    /* Under its own name: exactly the project it was packed from. */
    const QString Parent = InRoot.filePath(QStringLiteral("Examples"));
    Expect(RunCMake(ExampleScriptArgs(Script, Fixture, QStringLiteral("Tiny"), Parent, QStringLiteral("Tiny"), GetExampleSteps(false))),
           "NewProject.cmake unpacks the example");
    const QString Same = QDir(Parent).filePath(QStringLiteral("Tiny"));
    const QStringList Files = ListFiles(Source);
    Expect(!Files.isEmpty() && ListFiles(Same) == Files, "the files it was packed from");
    bool bSame = true;
    for(const QString& File : Files)
    {
        /* Outside Games/, the engine path is the one change. */
        if(File == QStringLiteral("CMakeLists.txt")) { continue; }
        if(ReadBytes(QDir(Same).filePath(File)) != ReadBytes(QDir(Source).filePath(File)))
        {
            bSame = false;
            std::printf("  differs: %s\n", qPrintable(File));
        }
    }
    Expect(bSame, "byte for byte");
#if !defined(_WIN32)
    Expect(QFileInfo(QDir(Same).filePath(QStringLiteral("Tools/run.sh"))).isExecutable(), "a script stays executable");
#endif

    /* Renamed: the module, classes, guards and settings, not the assets. */
    Expect(RunCMake(ExampleScriptArgs(Script, Fixture, QStringLiteral("Tiny"), Parent, QStringLiteral("Hello"), GetExampleSteps(false))),
           "and unpacks it under another name");
    const QDir Hello(QDir(Parent).filePath(QStringLiteral("Hello")));
    Expect(Hello.exists(QStringLiteral("HelloGame.cpp")) && !Hello.exists(QStringLiteral("TinyGame.cpp")), "source files are renamed");
    Expect(Hello.exists(QStringLiteral("Source/HelloAI/HelloBrain.h")) && !Hello.exists(QStringLiteral("Source/TinyAI")),
           "and folders, the old ones removed");
    Expect(Hello.exists(QStringLiteral("Content/Textures/Tiny_BaseColor.nmtexture")) && Hello.exists(QStringLiteral("Content/Levels/Tiny.nmworld")),
           "assets keep their names");
    Expect(ReadBytes(Hello.filePath(QStringLiteral("Content/Textures/Tiny_BaseColor.nmtexture")))
               == ReadBytes(QDir(Source).filePath(QStringLiteral("Content/Textures/Tiny_BaseColor.nmtexture"))),
           "and a binary asset is untouched");
    const QString Brain = ReadText(Hello.filePath(QStringLiteral("Source/HelloAI/HelloBrain.h")));
    Expect(Brain.contains(QStringLiteral("#ifndef HELLO_HELLOBRAIN_H")) && Brain.contains(QStringLiteral("[Hello]")) && !Brain.contains(QStringLiteral("Tiny"))
           && !Brain.contains(QStringLiteral("TINY")), "the name is replaced in text, capitals too");
    Expect(ReadText(Hello.filePath(QStringLiteral("Config/DefaultEngine.ini"))).contains(QStringLiteral("Module=HelloGame\n")), "the module setting");
    Expect(ReadText(Hello.filePath(QStringLiteral("Content/Levels/Tiny.nmworld"))).contains(QStringLiteral("Name=\"Hello\"")), "and text assets");
#if !defined(_WIN32)
    Expect(QFileInfo(Hello.filePath(QStringLiteral("Tools/run.sh"))).isExecutable(), "a rewritten script stays executable");
#endif

    /* Outside Games/, CMakeLists finds the real engine. */
    const QString Lists = ReadText(Hello.filePath(QStringLiteral("CMakeLists.txt")));
    const QString Prefix = QStringLiteral("set(GAME_ENGINE_DIR \"");
    const int     Start  = Lists.indexOf(Prefix) + Prefix.size();
    QString EngineDir = Lists.mid(Start, Lists.indexOf(QLatin1Char('"'), Start) - Start);
    EngineDir.replace(QStringLiteral("${CMAKE_CURRENT_SOURCE_DIR}"), Hello.path());
    Expect(QFileInfo(QDir(EngineDir).filePath(QStringLiteral("CMakeLists.txt"))).canonicalFilePath()
               == QFileInfo(QDir(GetEngineDir()).filePath(QStringLiteral("CMakeLists.txt"))).canonicalFilePath(),
           "GAME_ENGINE_DIR is the engine, from an example outside Games/");

    /* Refusals leave nothing behind. */
    Expect(!RunCMake(ExampleScriptArgs(Script, Fixture, QStringLiteral("Tiny"), Parent, QStringLiteral("Hello"), GetExampleSteps(false)), false),
           "an existing folder is refused");
    const QString Damaged = InRoot.filePath(QStringLiteral("Damaged"));
    Expect(CopyTree(Fixture, Damaged), "the fixture is copied");
    {
        QFile Part(QDir(Damaged).filePath(QStringLiteral("Tiny.Content.1.tar.zst")));
        Expect(Part.open(QIODevice::Append) && Part.write("x") == 1, "an archive is damaged");
    }
    Expect(!RunCMake(ExampleScriptArgs(Script, Damaged, QStringLiteral("Tiny"), Parent, QStringLiteral("Other"), GetExampleSteps(false)), false),
           "a damaged archive is refused");
    Expect(!RunCMake(ExampleScriptArgs(Script, Fixture, QStringLiteral("Nope"), Parent, QStringLiteral("Other"), GetExampleSteps(false)), false),
           "an unknown example is refused");
    Expect(!RunCMake(ExampleScriptArgs(Script, Fixture, QStringLiteral("Tiny"), Parent, QStringLiteral("Other"), { QStringLiteral("cook") }), false),
           "a step after unpack needs the project");
    Expect(ListFiles(Parent).size() == 2 * Files.size(), "and none of them writes anything");

    /* The packer: the fixture again, with a build's leftovers, packs to the
     * very bytes committed -- reproducible, and the leftovers left out. */
#ifdef NIMBLEEDITOR_PYTHON
    QProcess Probe;
    Probe.start(QStringLiteral(NIMBLEEDITOR_PYTHON), { QStringLiteral("-c"), QStringLiteral("import compression.zstd") });
    if(!Probe.waitForFinished(30000) || Probe.exitCode() != 0)
    {
        std::printf("  (packer check skipped: %s has no compression.zstd)\n", NIMBLEEDITOR_PYTHON);
        return;
    }
    const QString Copy = InRoot.filePath(QStringLiteral("PackSource/Tiny"));
    Expect(CopyTree(Source, Copy), "the fixture project is copied");
    for(const char* Junk : { "Saved/Logs/run.log", "Build/editor/x.o", "Binaries/Linux/Tiny", "Tools/__pycache__/x.pyc", "CMakeUserPresets.json" })
    {
        const QString Path = QDir(Copy).filePath(QString::fromLatin1(Junk));
        QDir().mkpath(QFileInfo(Path).absolutePath());
        QFile Out(Path);
        Expect(Out.open(QIODevice::WriteOnly) && Out.write("junk") == 4, "a leftover is written");
    }
    const QString Out = InRoot.filePath(QStringLiteral("Packed"));
    QProcess Pack;
    Pack.setProcessChannelMode(QProcess::MergedChannels);
    Pack.start(QStringLiteral(NIMBLEEDITOR_PYTHON), { QStringLiteral("-I"), QStringLiteral(NIMBLEEDITOR_PACK_SCRIPT), Copy,
                                                      QStringLiteral("--out"), Out });
    const bool bPacked = Pack.waitForFinished(120000) && Pack.exitCode() == 0;
    if(!bPacked) { std::printf("%s\n", Pack.readAll().constData()); }
    Expect(bPacked, "pack_example.py packs it");
    for(const char* Part : { "Tiny.Source.tar.zst", "Tiny.Content.1.tar.zst" })
    {
        const QByteArray Packed = ReadBytes(QDir(Out).filePath(QString::fromLatin1(Part)));
        Expect(!Packed.isEmpty() && Packed == ReadBytes(QDir(Fixture).filePath(QString::fromLatin1(Part))),
               "into the committed archive, byte for byte");
    }
#endif
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
    TestPluginTemplates(Root);
    TestExamples(Root);
    TestWorldSections(Root);

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
