#if WITH_EDITOR

#include "Private/Editor/SettingsDialog.h"

#include <map>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"

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
}

SettingsDialog::SettingsDialog(QWidget* InParent)
    : QDialog(InParent)
{
    setWindowTitle(QStringLiteral("Editor Settings"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto* Layout = new QVBoxLayout(this);

    const auto AddGroup = [this, Layout](const QString& InTitle)
    {
        auto* Group = new QGroupBox(InTitle, this);
        auto* Form  = new QFormLayout(Group);
        Form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
        Layout->addWidget(Group);
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
                                         const QString& InTip)
    {
        auto* Check = new QCheckBox(this);
        Row R;
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

    QFormLayout* Rendering = AddGroup(QStringLiteral("Rendering"));
    /* OpenGL does not start yet and Metal is Apple-only (DefaultEngine.ini);
     * a file that sets either still shows as Other. */
    AddChoice(Rendering, "Editor.RHI", QStringLiteral("Render backend"),
              QStringLiteral("The viewport's renderer. --rhi on the command line overrides it"),
              { { "Vulkan", 2 }, { "D3D12", 4 }, { "Null (draws nothing)", 0 } },
              true);

    QFormLayout* Build = AddGroup(QStringLiteral("Build"));
    AddCheck(Build, "Editor.ReloadAfterCompile", QStringLiteral("Restart a running game after Compile"),
             QStringLiteral("When Compile Source succeeds during Play, stop the run and start it again on the new "
                            "game module. Off: the run keeps the old one until the next Play"));

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
    AddCheck(Import, "Editor.ImportTextureSRGB", QStringLiteral("Textures are sRGB"),
             QStringLiteral("Colour images are sRGB; turn off for masks and normal maps"));
    AddChoice(Import, "Editor.ImportTextureMaxSize", QStringLiteral("Largest texture"),
              QStringLiteral("An image bigger than this on either side is scaled down to fit"),
              { { "256", 256 }, { "512", 512 }, { "1024", 1024 }, { "2048", 2048 }, { "4096", 4096 },
                { "8192", 8192 }, { "16384", 16384 } },
              false);

    Layout->addStretch(1);

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

void SettingsDialog::Apply()
{
    bool bNeedsRestart = false;
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
                                 QStringLiteral("The new render backend takes effect when the editor restarts."));
    }
}

bool SettingsDialog::Save() const
{
    /* By section, in row order, so the file reads like the dialog. */
    std::vector<std::string>                              Sections;
    std::map<std::string, std::vector<std::string>>       Lines;
    for(const Row& R : Rows)
    {
        const EngineCore::ConfigVar* Var = FindVar(R.Path);
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
