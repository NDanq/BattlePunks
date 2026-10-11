#if WITH_EDITOR

#include "Private/Editor/SettingsDialog.h"

#include <cstring>
#include <map>
#include <string>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace
{
    EngineCore::ConfigVar* FindVar(const char* InPath)
    {
        return EngineCore::ConfigFindVarByPath(InPath);
    }

    QString VarText(const EngineCore::ConfigVar& InVar)
    {
        return QString::fromStdString(InVar.ToString());
    }

    std::string ToStd(const QString& InText)
    {
        return std::string(InText.toUtf8().constData());
    }

    struct Choice
    {
        const char* Text;
        int         Value;
    };

    /* Saved with the settings but set in the viewport -- its toolbar and Show
     * menu (ViewportPanel.cpp) -- not on a page here. */
    const char* const ViewStatePaths[] =
    {
        "Editor.ViewGizmoMode", "Editor.ViewGizmoSpace", "Editor.ViewGizmoSnap", "Editor.ViewMode",
        "Editor.ViewAllBounds", "Editor.ViewLightGizmos", "Editor.ViewCameraSpeed", "Rendering.BakedLighting",
    };
}

SettingsDialog::SettingsDialog(QWidget* InParent)
    : QDialog(InParent)
{
    setWindowTitle(QStringLiteral("Editor Settings"));
    setAttribute(Qt::WA_DeleteOnClose);

    resize(640, 420);
    auto* Layout = new QVBoxLayout(this);
    auto* Body   = new QHBoxLayout();
    Layout->addLayout(Body, 1);

    /* Pages, as Project Settings has: a list of sections, one form each. */
    auto* Pages = new QListWidget(this);
    PageList = Pages;
    Pages->setFixedWidth(150);
    Body->addWidget(Pages);
    auto* Stack = new QStackedWidget(this);
    Body->addWidget(Stack, 1);
    connect(Pages, &QListWidget::currentRowChanged, Stack, &QStackedWidget::setCurrentIndex);

    const auto AddGroup = [this, Pages, Stack](const QString& InTitle)
    {
        auto* Scroll = new QScrollArea(Stack);
        Scroll->setWidgetResizable(true);
        Scroll->setFrameShape(QFrame::NoFrame);

        auto* Page   = new QWidget(Scroll);
        auto* Column = new QVBoxLayout(Page);
        Column->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(InTitle.toHtmlEscaped()), Page));
        auto* Form = new QFormLayout();
        Form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        Column->addLayout(Form);
        Column->addStretch(1);

        Scroll->setWidget(Page);
        Stack->addWidget(Scroll);
        Pages->addItem(InTitle);
        return Form;
    };

    /* Each Add* makes the widget for one var and its row. A var that is not
     * registered (a typo here, or a module not loaded) gets no row. */
    const auto AddRow = [this](QFormLayout* InForm, const char* InPath, const QString& InLabel,
                               const QString& InTip, QWidget* InField, Row InRow)
    {
        if(!FindVar(InPath))
        {
            CORE_LOG_WARN(std::string("[Editor] settings: no config var ") + InPath)
            delete InField;
            return;
        }
        InField->setToolTip(InTip + QStringLiteral("\n\n") + QString::fromLatin1(InPath));
        InRow.Path = InPath;
        InForm->addRow(InLabel + (InRow.bRestart ? QStringLiteral(" *") : QString()), InField);
        Rows.push_back(std::move(InRow));
    };

    const auto AddFloat = [this, AddRow](QFormLayout* InForm, const char* InPath, const QString& InLabel,
                                         const QString& InTip, const double InMin, const double InMax,
                                         const int InDecimals, const QString& InSuffix)
    {
        auto* Spin = new QDoubleSpinBox(this);
        Spin->setRange(InMin, InMax);
        Spin->setDecimals(InDecimals);
        Spin->setSuffix(InSuffix);
        Spin->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
        Row R;
        R.Show = [Spin](const QString& InText) { Spin->setValue(InText.toDouble()); };
        R.Read = [Spin]() { return QString::number(Spin->value(), 'g', 6); };
        AddRow(InForm, InPath, InLabel, InTip, Spin, std::move(R));
    };

    const auto AddCheck = [this, AddRow](QFormLayout* InForm, const char* InPath, const QString& InLabel,
                                         const QString& InTip, const bool bInRestart = false)
    {
        auto* Check = new QCheckBox(this);
        Row R;
        R.bRestart = bInRestart;
        R.Show = [Check](const QString& InText) { Check->setChecked(InText == QStringLiteral("true")); };
        R.Read = [Check]() { return Check->isChecked() ? QStringLiteral("true") : QStringLiteral("false"); };
        AddRow(InForm, InPath, InLabel, InTip, Check, std::move(R));
    };

    const auto AddChoice = [this, AddRow](QFormLayout* InForm, const char* InPath, const QString& InLabel,
                                          const QString& InTip, std::initializer_list<Choice> InChoices,
                                          const bool bInRestart)
    {
        auto* Combo = new QComboBox(this);
        for(const Choice& C : InChoices)
        {
            Combo->addItem(QString::fromLatin1(C.Text), C.Value);
        }
        Row R;
        R.bRestart = bInRestart;
        R.Show = [Combo](const QString& InText)
        {
            /* A value set by hand in an .ini that the list does not offer
             * still shows, rather than being silently replaced. */
            const int Value = InText.toInt();
            int Index = Combo->findData(Value);
            if(Index < 0)
            {
                Combo->addItem(QStringLiteral("Other (%1)").arg(Value), Value);
                Index = Combo->count() - 1;
            }
            Combo->setCurrentIndex(Index);
        };
        R.Read = [Combo]() { return QString::number(Combo->currentData().toInt()); };
        AddRow(InForm, InPath, InLabel, InTip, Combo, std::move(R));
    };

    /* A colour as "#rrggbb": a swatch button that opens a colour picker. */
    const auto AddColor = [this, AddRow](QFormLayout* InForm, const char* InPath, const QString& InLabel,
                                         const QString& InTip)
    {
        auto* Swatch = new QPushButton(this);
        Swatch->setFixedWidth(96);
        const auto Paint = [Swatch](const QColor& InColor)
        {
            Swatch->setProperty("Color", InColor.name());
            Swatch->setText(InColor.name());
            Swatch->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; }")
                                      .arg(InColor.name(), InColor.lightness() > 128 ? QStringLiteral("black") : QStringLiteral("white")));
        };
        connect(Swatch, &QPushButton::clicked, this, [this, Swatch, Paint, InLabel]()
        {
            const QColor Picked = QColorDialog::getColor(QColor(Swatch->property("Color").toString()), this, InLabel);
            if(Picked.isValid()) { Paint(Picked); }
        });
        Row R;
        R.Show = [Paint](const QString& InText) { const QColor Color(InText); Paint(Color.isValid() ? Color : QColor(Qt::black)); };
        R.Read = [Swatch]() { return Swatch->property("Color").toString(); };
        AddRow(InForm, InPath, InLabel, InTip, Swatch, std::move(R));
    };

    const auto AddText = [this, AddRow](QFormLayout* InForm, const char* InPath, const QString& InLabel,
                                        const QString& InTip)
    {
        auto* Edit = new QLineEdit(this);
        Edit->setMinimumWidth(200);
        Row R;
        R.Show = [Edit](const QString& InText) { Edit->setText(InText); };
        R.Read = [Edit]() { return Edit->text().trimmed(); };
        AddRow(InForm, InPath, InLabel, InTip, Edit, std::move(R));
    };

    const auto AddInt = [this, AddRow](QFormLayout* InForm, const char* InPath, const QString& InLabel,
                                       const QString& InTip, const int InMin, const int InMax)
    {
        auto* Spin = new QSpinBox(this);
        Spin->setRange(InMin, InMax);
        Row R;
        R.Show = [Spin](const QString& InText) { Spin->setValue(InText.toInt()); };
        R.Read = [Spin]() { return QString::number(Spin->value()); };
        AddRow(InForm, InPath, InLabel, InTip, Spin, std::move(R));
    };

    // === The settings ========================================================

    QFormLayout* Appearance = AddGroup(QStringLiteral("Appearance"));
    AddChoice(Appearance, "Editor.Theme", QStringLiteral("Theme"),
              QStringLiteral("Light or dark UI, or follow the system's app mode. Applies at once"),
              { { "System", 0 }, { "Light", 1 }, { "Dark", 2 } },
              false);

    QFormLayout* Viewport = AddGroup(QStringLiteral("Viewport"));
    AddFloat(Viewport, "Editor.SnapTranslate", QStringLiteral("Move snap"),
             QStringLiteral("Step for snapped Move drags: a world grid in World space, a step along the handle in Local"),
             0.001, 100000.0, 3, QString());
    AddFloat(Viewport, "Editor.SnapRotate", QStringLiteral("Rotate snap"),
             QStringLiteral("Step for snapped Rotate drags"),
             0.01, 360.0, 2, QStringLiteral(" deg"));
    AddFloat(Viewport, "Editor.SnapScale", QStringLiteral("Scale snap"),
             QStringLiteral("Step for snapped Scale drags"),
             0.001, 100.0, 3, QString());
    AddCheck(Viewport, "Editor.ViewportGradient", QStringLiteral("Background gradient"),
             QStringLiteral("The viewport background as a vertical gradient, top colour to bottom; off, the top colour alone"));
    AddColor(Viewport, "Editor.ViewportTopColor", QStringLiteral("Background top"),
             QStringLiteral("The background at the top of the viewport (all of it, with the gradient off)"));
    AddColor(Viewport, "Editor.ViewportBottomColor", QStringLiteral("Background bottom"),
             QStringLiteral("The background at the bottom of the viewport"));

    QFormLayout* Rendering = AddGroup(QStringLiteral("Rendering"));
    /* OpenGL does not start yet and Metal is Apple-only (DefaultEngine.ini);
     * a file that sets either still shows as Other. */
    AddChoice(Rendering, "Editor.RHI", QStringLiteral("Render backend"),
              QStringLiteral("The viewport's renderer. --rhi on the command line overrides it"),
              { { "Vulkan", 2 }, { "D3D12", 4 }, { "Null (draws nothing)", 0 } },
              true);

    /* GDI's (GDI.h): what capture tools (RenderDoc, PIX) are given. */
    QFormLayout* Shaders = AddGroup(QStringLiteral("Shaders"));
    AddCheck(Shaders, "Shaders.ShaderDebug", QStringLiteral("ShaderDebug"),
             QStringLiteral("Draw events and debug names: each draw a named region and each GPU object (mesh, texture, "
                            "buffer, shader, render target) named, as RenderDoc and PIX show them. Draw events follow at "
                            "once; objects are named as they are made, so a restart names those made before"));
    AddCheck(Shaders, "Shaders.ShaderPDB", QStringLiteral("ShaderPDB"),
             QStringLiteral("Shader PDBs: the shader cook also writes each shader's debug information -- SPIR-V with its "
                            "source for Vulkan, fxc PDBs for D3D12 -- to %1/<Target>, for the engine's shaders and "
                            "materials and the project's, and for a material as it is saved. Under RenderDoc a Vulkan "
                            "shader then shows its source; for D3D12, add the folder to RenderDoc's shader search paths. "
                            "Shaders loaded before the files are written find them after a restart")
                 .arg(QDir::toNativeSeparators(EditorGraphs::GetShaderPDBRoot())),
             true);

    QFormLayout* Build = AddGroup(QStringLiteral("Build"));
    AddCheck(Build, "Editor.ReloadAfterCompile", QStringLiteral("Restart a running game after Compile"),
             QStringLiteral("When Compile Source succeeds during Play, stop the run and start it again on the new "
                            "game module. Off: the run keeps the old one until the next Play"));
    AddCheck(Build, "Editor.BuildGameExecutables", QStringLiteral("Also build the game's client and server"),
             QStringLiteral("Compile Source builds the game's launcher and, with networking, its dedicated server "
                            "beside the module -- what Play's client windows and dedicated server run"));

#if WITH_NETCORE
    /* Play-in-Editor over the network (PlaySession.h's PlayNetSettings). */
    QFormLayout* Play = AddGroup(QStringLiteral("Play"));
    AddChoice(Play, "Editor.PlayNetMode", QStringLiteral("Network mode"),
              QStringLiteral("Standalone: no network. Listen Server: the viewport hosts and is a player too. "
                             "Client: the viewport joins a server (started here, below, or anywhere)"),
              { { "Standalone", 0 }, { "Listen Server", 1 }, { "Client", 2 } },
              false);
    AddText(Play, "Editor.PlayServerAddress", QStringLiteral("Server address"),
            QStringLiteral("Client: the server to join. 127.0.0.1 is this machine"));
    AddInt(Play, "Editor.PlayPort", QStringLiteral("Port"),
           QStringLiteral("Listen Server: the port the viewport hosts on. Client: the server's port"), 1, 65535);
    AddCheck(Play, "Editor.PlayLaunchServer", QStringLiteral("Start a dedicated server"),
             QStringLiteral("Client: run the game's dedicated server on this machine first, on the port above, "
                            "and stop it with the run (needs the server built: Build > Compile Source)"));
    AddInt(Play, "Editor.PlayClientWindows", QStringLiteral("Client windows at Play"),
           QStringLiteral("More players: game windows that join the run when it starts. Play > New Client Window "
                          "(Shift+F6) adds one at any time"), 0, 8);
    AddText(Play, "Editor.PlayClientArgs", QStringLiteral("Client window arguments"),
            QStringLiteral("More command line for each client window, as the game's launcher takes it: "
                           "-Section.Key=Value overrides, e.g. -Display.RHI=4"));
#endif

    /* What the Import Wizard starts with; it saves back what was last used. */
    QFormLayout* Import = AddGroup(QStringLiteral("Import"));
    AddFloat(Import, "Editor.ImportMeshScale", QStringLiteral("Mesh scale"),
             QStringLiteral("Multiplies an imported mesh's positions. glTF is in metres: 100 makes a metre 100 units"),
             0.0001, 100000.0, 4, QString());
    AddCheck(Import, "Editor.ImportConvertCoordinates", QStringLiteral("Convert glTF axes"),
             QStringLiteral("Convert a glTF's right-handed axes to the engine's left-handed ones (z flips), facing the default camera"));
    AddCheck(Import, "Editor.ImportGenerateNormals", QStringLiteral("Generate missing normals"),
             QStringLiteral("Smooth normals for a primitive that has none"));
    AddCheck(Import, "Editor.ImportCombineMeshes", QStringLiteral("Combine meshes"),
             QStringLiteral("One .nmmesh for a whole glTF scene, or one per glTF mesh"));
    AddCheck(Import, "Editor.ImportMeshTextures", QStringLiteral("Import a mesh's textures"),
             QStringLiteral("Import the images a glTF refers to as .nmtexture beside the mesh"));
    AddCheck(Import, "Editor.ImportMaterialColors", QStringLiteral("Bake material colours"),
             QStringLiteral("An untextured glTF material's base colour baked into the mesh's vertex colours, so it draws in its colours with DefaultLit"));
    AddCheck(Import, "Editor.ImportTextureSRGB", QStringLiteral("Textures are sRGB"),
             QStringLiteral("Colour images are sRGB; turn off for masks and normal maps"));
    AddChoice(Import, "Editor.ImportTextureMaxSize", QStringLiteral("Largest texture"),
              QStringLiteral("An image bigger than this on either side is scaled down to fit"),
              { { "256", 256 }, { "512", 512 }, { "1024", 1024 }, { "2048", 2048 }, { "4096", 4096 },
                { "8192", 8192 }, { "16384", 16384 } },
              false);
    AddChoice(Import, "Editor.ImportTextureMipGen", QStringLiteral("Texture mipmaps"),
              QStringLiteral("The mip chain baked into an imported texture; the Texture Viewer changes it later"),
              { { "None", 0 }, { "Box", 1 }, { "Kaiser", 2 } },
              false);

    Pages->setCurrentRow(0);

    auto* Note = new QLabel(QStringLiteral("* Takes effect when the editor restarts.\nSaved to %1")
                            .arg(QDir::toNativeSeparators(GetUserSettingsPath())), this);
    Note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Note->setWordWrap(true);
    Layout->addWidget(Note);

    Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply
                                 | QDialogButtonBox::RestoreDefaults, this);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this]()
    {
        Apply();
        accept();
    });
    connect(Buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::Apply);
    connect(Buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, &SettingsDialog::RestoreDefaults);
    Layout->addWidget(Buttons);

    for(const Row& R : Rows)
    {
        R.Show(VarText(*FindVar(R.Path)));
    }
}

QString SettingsDialog::GetUserSettingsPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
           .filePath(QStringLiteral("EditorSettings.ini"));
}

void SettingsDialog::LoadUserSettings()
{
    /* Read here rather than by LoadConfigFile: the path is under the user's
     * profile, which may not be ASCII, and Qt already knows how to open it. */
    QFile File(GetUserSettingsPath());
    if(!File.exists())
    {
        return;
    }
    if(!File.open(QIODevice::ReadOnly))
    {
        CORE_LOG_WARN("[Editor] could not read " + ToStd(File.fileName()))
        return;
    }
    EngineCore::ConfigLoadStats Stats;
    EngineCore::LoadConfigText(File.readAll().toStdString(), &Stats);
    CORE_LOG("[Editor] user settings: " + std::to_string(Stats.KeysApplied) + " from " + ToStd(File.fileName()))
}

void SettingsDialog::ShowPage(const QString& InTitle)
{
    const QList<QListWidgetItem*> Found = PageList ? PageList->findItems(InTitle, Qt::MatchExactly) : QList<QListWidgetItem*>();
    if(!Found.isEmpty()) { PageList->setCurrentItem(Found.first()); }
}

void SettingsDialog::Apply()
{
    bool bNeedsRestart = false;
    bool bCookPDBs     = false;
    for(const Row& R : Rows)
    {
        EngineCore::ConfigVar* Var = FindVar(R.Path);
        if(!Var)
        {
            continue;
        }
        bool bChanged = false;
        if(!Var->SetFromString(ToStd(R.Read()), &bChanged))
        {
            CORE_LOG_WARN(std::string("[Editor] settings: '") + ToStd(R.Read()) + "' is not a value for " + R.Path)
            continue;
        }
        if(bChanged)
        {
            CORE_LOG(std::string("[Editor] settings: ") + R.Path + " = " + Var->ToString())
            bNeedsRestart |= R.bRestart;
            bCookPDBs     |= std::strcmp(R.Path, "Shaders.ShaderPDB") == 0 && Var->ToString() == "true";
        }
        R.Show(VarText(*Var));   // as parsed: 0.1000001 shows as the var holds it
    }

    if(!Save())
    {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("The settings apply to this session, but could not be saved to\n%1")
                             .arg(QDir::toNativeSeparators(GetUserSettingsPath())));
    }
    else if(bNeedsRestart)
    {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("Settings marked * take full effect when the editor restarts."));
    }

    if(bCookPDBs)
    {
        EditorGraphs::CookShaderPDBs();
    }
}

bool SettingsDialog::Save() const
{
    /* By section, in row order, so the file reads like the dialog; then the
     * viewport's view, which no page shows. */
    std::vector<std::string>                              Sections;
    std::map<std::string, std::vector<std::string>>       Lines;
    std::vector<const char*>                              Paths;
    for(const Row& R : Rows) { Paths.push_back(R.Path); }
    for(const char* Path : ViewStatePaths) { Paths.push_back(Path); }
    for(const char* Path : Paths)
    {
        const EngineCore::ConfigVar* Var = FindVar(Path);
        if(!Var)
        {
            continue;
        }
        const std::string Section = Var->GetSection();
        if(Lines.find(Section) == Lines.end())
        {
            Sections.push_back(Section);
        }
        Lines[Section].push_back(std::string(Var->GetName()) + "=" + Var->ToString());
    }

    std::string Text = "; Nimble Editor settings -- written by Edit > Editor Settings, loaded over\n"
                       "; Config/DefaultEngine.ini at startup. Delete this file to go back to the project's values.\n";
    for(const std::string& Section : Sections)
    {
        Text += "\n[" + Section + "]\n";
        for(const std::string& Line : Lines[Section])
        {
            Text += Line + "\n";
        }
    }

    const QString Path = GetUserSettingsPath();
    QDir().mkpath(QFileInfo(Path).absolutePath());
    QSaveFile File(Path);
    if(!File.open(QIODevice::WriteOnly | QIODevice::Text)
       || File.write(Text.data(), qint64(Text.size())) != qint64(Text.size())
       || !File.commit())
    {
        CORE_LOG_ERROR("[Editor] could not write " + ToStd(Path) + ": " + ToStd(File.errorString()))
        return false;
    }
    return true;
}

bool SettingsDialog::SaveUserSettings()
{
    /* The rows say which vars are the user's; a dialog never shown has them. */
    SettingsDialog Rows;
    return Rows.Save();
}

void SettingsDialog::RestoreDefaults()
{
    /* The widgets only: nothing changes until Apply or OK. */
    for(const Row& R : Rows)
    {
        if(const EngineCore::ConfigVar* Var = FindVar(R.Path))
        {
            R.Show(QString::fromStdString(Var->DefaultToString()));
        }
    }
}

#endif // WITH_EDITOR
