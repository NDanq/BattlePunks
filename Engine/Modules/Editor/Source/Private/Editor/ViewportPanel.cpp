#if WITH_EDITOR

#include "Public/Editor/ViewportPanel.h"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QToolBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/Editor/EditorCamera.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportWidget.h"
#include "Private/Editor/SettingsDialog.h"

namespace
{
    /* Speed is 2^(step - SpeedStepOne): 1/8x .. 8x, 1x in the middle. */
    constexpr int SpeedStepMin = 0;
    constexpr int SpeedStepMax = 6;
    constexpr int SpeedStepOne = 3;

    /* How long after the last change the view is written to EditorSettings.ini. */
    constexpr int SaveDelayMs = 750;
}

/*
 * The view as the user left it: read when a viewport opens, written back on
 * every change (SettingsDialog saves them to EditorSettings.ini with the
 * Editor Settings, though no page shows them). Show > Lighting is [Rendering]
 * BakedLighting, saved the same way.
 */
NIMBLE_CONFIG_INT (GEditorViewGizmoMode,    "Editor", "ViewGizmoMode",    int(TransformGizmo::EMode::Translate), nullptr);   // 0 Select, 1 Move, 2 Rotate, 3 Scale
NIMBLE_CONFIG_INT (GEditorViewGizmoSpace,   "Editor", "ViewGizmoSpace",   int(TransformGizmo::ESpace::World), nullptr);      // 0 World, 1 Local
NIMBLE_CONFIG_BOOL(GEditorViewGizmoSnap,    "Editor", "ViewGizmoSnap",    false, nullptr);
NIMBLE_CONFIG_INT (GEditorViewMode,         "Editor", "ViewMode",         0, nullptr);       // 0 Lit, 1 Unlit (a render target is not kept)
NIMBLE_CONFIG_BOOL(GEditorViewAllBounds,    "Editor", "ViewAllBounds",    false, nullptr);
NIMBLE_CONFIG_BOOL(GEditorViewLightGizmos,  "Editor", "ViewLightGizmos",  true, nullptr);
NIMBLE_CONFIG_INT (GEditorViewCameraSpeed,  "Editor", "ViewCameraSpeed",  3, nullptr);       // 0 (1/8x) .. 6 (8x)

namespace
{

    struct PresetButton
    {
        const char*           Label;
        const char*           Tip;
        EditorCamera::EPreset Preset;
    };

    struct GizmoModeButton
    {
        const char*           Label;
        const char*           Tip;
        TransformGizmo::EMode Mode;
    };

    /* In EMode order: the mode combo's index is the mode. */
    const GizmoModeButton GizmoModes[] =
    {
        { "Select", "No transform handles -- click to select (1)",       TransformGizmo::EMode::None      },
        { "Move",   "Move handles: axes, planes, screen centre (2)",     TransformGizmo::EMode::Translate },
        { "Rotate", "Rotate rings: one per axis, the outer about the view (3)", TransformGizmo::EMode::Rotate },
        { "Scale",  "Scale handles: per axis, the centre for all three (4)",    TransformGizmo::EMode::Scale  },
    };

    /* A toolbar combo box that leaves keyboard focus where it was. */
    QComboBox* MakeCombo(QToolBar* InToolbar, const QString& InTip)
    {
        auto* Combo = new QComboBox(InToolbar);
        Combo->setFocusPolicy(Qt::NoFocus);
        Combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        Combo->setToolTip(InTip);
        InToolbar->addWidget(Combo);
        return Combo;
    }

    const PresetButton Presets[] =
    {
        { "Persp",  "Perspective -- above and to the side",    EditorCamera::EPreset::Perspective },
        { "Front",  "Front -- looking along +Z (the game view)", EditorCamera::EPreset::Front       },
        { "Back",   "Back -- looking along -Z",                 EditorCamera::EPreset::Back        },
        { "Top",    "Top -- from above (+Y), looking along -Y",    EditorCamera::EPreset::Top     },
        { "Bottom", "Bottom -- from below (-Y), looking along +Y", EditorCamera::EPreset::Bottom   },
        { "Left",   "Left -- from -X, looking along +X",        EditorCamera::EPreset::Left        },
        { "Right",  "Right -- from +X, looking along -X",       EditorCamera::EPreset::Right       },
    };
}

ViewportPanel::ViewportPanel(EditorContext& InContext, EditorDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Context(InContext)
    , Document(InDocument)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->setSpacing(0);

    Toolbar = new QToolBar(this);
    Toolbar->setIconSize(QSize(16, 16));
    Toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    Layout->addWidget(Toolbar);

    Viewport = new ViewportWidget(InContext, InDocument, this);
    Layout->addWidget(Viewport, 1);

    /* The view as it was left, before the toolbar reads it back. */
    SaveTimer = new QTimer(this);
    SaveTimer->setSingleShot(true);
    SaveTimer->setInterval(SaveDelayMs);
    connect(SaveTimer, &QTimer::timeout, this, []() { SettingsDialog::SaveUserSettings(); });
    Viewport->SetGizmoMode(TransformGizmo::EMode(std::clamp(GEditorViewGizmoMode.GetInt(), 0, int(TransformGizmo::EMode::Scale))));
    Viewport->SetGizmoSpace(TransformGizmo::ESpace(std::clamp(GEditorViewGizmoSpace.GetInt(), 0, int(TransformGizmo::ESpace::Local))));
    Viewport->SetGizmoSnap(GEditorViewGizmoSnap.GetBool());
    Viewport->SetShowAllBounds(GEditorViewAllBounds.GetBool());
    Viewport->SetShowLightGizmos(GEditorViewLightGizmos.GetBool());
    if(Context.GetViewMode() != RenderCore::ERenderViewMode::RenderTarget)
    {
        Context.SetViewMode(GEditorViewMode.GetInt() == 1 ? RenderCore::ERenderViewMode::Unlit : RenderCore::ERenderViewMode::Lit);
    }

    // --- Transform handles ---------------------------------------------------
    /* No shortcuts here: the keys are the viewport's own (see ViewportWidget),
     * so typing Q W E R in a text field never switches modes. */
    ModeCombo = MakeCombo(Toolbar, QStringLiteral("Transform handles (1 / 2 / 3 / 4)"));
    for(const GizmoModeButton& Button : GizmoModes)
    {
        ModeCombo->addItem(QString::fromLatin1(Button.Label));
        ModeCombo->setItemData(ModeCombo->count() - 1, QString::fromLatin1(Button.Tip), Qt::ToolTipRole);
    }
    ModeCombo->setCurrentIndex(int(Viewport->GetGizmoMode()));
    connect(ModeCombo, &QComboBox::currentIndexChanged, this, [this](const int InIndex)
    {
        Viewport->SetGizmoMode(TransformGizmo::EMode(InIndex));
        Remember(GEditorViewGizmoMode.SetInt(InIndex));
    });
    connect(Viewport, &ViewportWidget::GizmoModeChanged, ModeCombo, [this](const TransformGizmo::EMode InMode)
    {
        ModeCombo->setCurrentIndex(int(InMode));
    });

    SpaceCombo = MakeCombo(Toolbar, QStringLiteral("Move and rotate along the world's axes or the entity's own (X). "
                                                   "Scale always uses the entity's axes."));
    SpaceCombo->addItems({ QStringLiteral("World"), QStringLiteral("Local") });
    SpaceCombo->setCurrentIndex(int(Viewport->GetGizmoSpace()));
    connect(SpaceCombo, &QComboBox::currentIndexChanged, this, [this](const int InIndex)
    {
        Viewport->SetGizmoSpace(TransformGizmo::ESpace(InIndex));
        Remember(GEditorViewGizmoSpace.SetInt(InIndex));
    });
    connect(Viewport, &ViewportWidget::GizmoSpaceChanged, SpaceCombo, [this](const TransformGizmo::ESpace InSpace)
    {
        SpaceCombo->setCurrentIndex(int(InSpace));
    });

    GizmoSnapAction = Toolbar->addAction(QStringLiteral("Snap"));
    GizmoSnapAction->setCheckable(true);
    GizmoSnapAction->setToolTip(QStringLiteral("Snap handle drags to [Editor] SnapTranslate / SnapRotate / SnapScale "
                                               "(Ctrl during a drag inverts this)"));
    GizmoSnapAction->setChecked(Viewport->IsGizmoSnapEnabled());
    connect(GizmoSnapAction, &QAction::toggled, this, [this](const bool bInOn)
    {
        Viewport->SetGizmoSnap(bInOn);
        Remember(GEditorViewGizmoSnap.SetBool(bInOn));
    });
    connect(Viewport, &ViewportWidget::GizmoSnapChanged, GizmoSnapAction, &QAction::setChecked);

    Toolbar->addSeparator();

    // --- View presets --------------------------------------------------------
    /* `activated`, so re-picking the shown view snaps back to it. */
    ViewCombo = MakeCombo(Toolbar, QStringLiteral("View preset -- orbits about the selection to that axis"));
    for(const PresetButton& Button : Presets)
    {
        ViewCombo->addItem(QString::fromLatin1(Button.Label), int(Button.Preset));
        ViewCombo->setItemData(ViewCombo->count() - 1, QString::fromLatin1(Button.Tip), Qt::ToolTipRole);
    }
    connect(ViewCombo, &QComboBox::activated, this, [this](const int InIndex)
    {
        Viewport->ApplyPreset(EditorCamera::EPreset(ViewCombo->itemData(InIndex).toInt()));
    });

    // --- Framing -------------------------------------------------------------
    /* Frame (selection) with Frame All on its menu; shared with the Edit menu. */
    FrameSelectionAction = new QAction(QStringLiteral("Frame Selected"), this);
    FrameSelectionAction->setToolTip(QStringLiteral("Fit the selection's bounds in view (F)"));
    connect(FrameSelectionAction, &QAction::triggered, this, [this]() { Viewport->FrameSelection(); });

    FrameAllAction = new QAction(QStringLiteral("Frame All"), this);
    FrameAllAction->setToolTip(QStringLiteral("Fit every entity in view (Home)"));
    connect(FrameAllAction, &QAction::triggered, this, [this]() { Viewport->FrameAll(); });

    auto* FrameMenu = new QMenu(this);
    FrameMenu->addAction(FrameSelectionAction);
    FrameMenu->addAction(FrameAllAction);

    auto* FrameButton = new QToolButton(Toolbar);
    FrameButton->setText(QStringLiteral("Frame"));
    FrameButton->setToolTip(QStringLiteral("Fit the selection in view (F); the arrow has Frame All (Home)"));
    FrameButton->setFocusPolicy(Qt::NoFocus);
    FrameButton->setPopupMode(QToolButton::MenuButtonPopup);
    FrameButton->setMenu(FrameMenu);
    connect(FrameButton, &QToolButton::clicked, FrameSelectionAction, &QAction::trigger);
    Toolbar->addWidget(FrameButton);

    Toolbar->addSeparator();

    // --- Show: the view mode and the editor's gizmos ---------------------------
    auto* ShowMenu = new QMenu(this);

    QMenu* ModeMenu = ShowMenu->addMenu(QStringLiteral("View Mode"));
    auto* Modes = new QActionGroup(ModeMenu);
    LitModeAction = ModeMenu->addAction(QStringLiteral("Lit"));
    LitModeAction->setToolTip(QStringLiteral("The scene as it is lit"));
    UnlitModeAction = ModeMenu->addAction(QStringLiteral("Unlit"));
    UnlitModeAction->setToolTip(QStringLiteral("Base colour and emissive as they are: no light, baked or live"));
    TargetModeAction = ModeMenu->addAction(QStringLiteral("Render Target..."));
    TargetModeAction->setToolTip(QStringLiteral("One of the render targets the frame drew, over the view: G-buffer, shadow atlases, ..."));
    for(QAction* Each : { LitModeAction, UnlitModeAction, TargetModeAction })
    {
        Each->setCheckable(true);
        Modes->addAction(Each);
    }
    connect(LitModeAction, &QAction::triggered, this, [this]()
    {
        Context.SetViewMode(RenderCore::ERenderViewMode::Lit);
        UpdateViewModeUI();
    });
    connect(UnlitModeAction, &QAction::triggered, this, [this]()
    {
        Context.SetViewMode(RenderCore::ERenderViewMode::Unlit);
        UpdateViewModeUI();
    });
    connect(TargetModeAction, &QAction::triggered, this, [this]()
    {
        PickRenderTarget();
        UpdateViewModeUI();   // a cancel puts the check back where it was
    });

    QMenu* LightingMenu = ShowMenu->addMenu(QStringLiteral("Lighting"));
    auto* Lighting = new QActionGroup(LightingMenu);
    BakedAction = LightingMenu->addAction(QStringLiteral("Baked"));
    BakedAction->setToolTip(QStringLiteral("Static entities and lights as Build > Bake Lightmaps baked them; dynamic ones lit by the probes"));
    LiveAction = LightingMenu->addAction(QStringLiteral("Live"));
    LiveAction->setToolTip(QStringLiteral("Every light lit live, as if there were no bake"));
    for(QAction* Each : { BakedAction, LiveAction })
    {
        Each->setCheckable(true);
        Lighting->addAction(Each);
    }
    {
        const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Rendering", "BakedLighting");
        (Var && !Var->GetBool() ? LiveAction : BakedAction)->setChecked(true);
    }
    connect(BakedAction, &QAction::triggered, this, [this]() { SetBakedLighting(true); });
    connect(LiveAction, &QAction::triggered, this, [this]() { SetBakedLighting(false); });

    QMenu* GizmoMenu = ShowMenu->addMenu(QStringLiteral("Editor Gizmos"));
    ShowAllBoundsAction = GizmoMenu->addAction(QStringLiteral("All Bounds"));
    ShowAllBoundsAction->setCheckable(true);
    ShowAllBoundsAction->setChecked(Viewport->IsShowingAllBounds());
    ShowAllBoundsAction->setToolTip(QStringLiteral("Draw every entity's bounds, not only the selection's"));
    connect(ShowAllBoundsAction, &QAction::toggled, this, [this](const bool bInOn)
    {
        Viewport->SetShowAllBounds(bInOn);
        Remember(GEditorViewAllBounds.SetBool(bInOn));
    });

    ShowLightGizmosAction = GizmoMenu->addAction(QStringLiteral("Light Gizmos"));
    ShowLightGizmosAction->setCheckable(true);
    ShowLightGizmosAction->setChecked(Viewport->IsShowingLightGizmos());
    ShowLightGizmosAction->setToolTip(QStringLiteral("Draw every light's reach, not only the selected light's"));
    connect(ShowLightGizmosAction, &QAction::toggled, this, [this](const bool bInOn)
    {
        Viewport->SetShowLightGizmos(bInOn);
        Remember(GEditorViewLightGizmos.SetBool(bInOn));
    });

    ShowButton = new QToolButton(Toolbar);
    ShowButton->setFocusPolicy(Qt::NoFocus);
    ShowButton->setPopupMode(QToolButton::InstantPopup);
    ShowButton->setMenu(ShowMenu);
    Toolbar->addWidget(ShowButton);
    UpdateViewModeUI();

    Toolbar->addSeparator();

    // --- Camera speed --------------------------------------------------------
    SpeedCombo = MakeCombo(Toolbar, QStringLiteral("Camera speed: fly, pan and dolly"));
    for(int Step = SpeedStepMin; Step <= SpeedStepMax; ++Step)
    {
        const int Power = Step - SpeedStepOne;
        SpeedCombo->addItem(Power < 0 ? QStringLiteral("Speed 1/%1x").arg(1 << -Power)
                                      : QStringLiteral("Speed %1x").arg(1 << Power));
    }
    const int Speed = std::clamp(GEditorViewCameraSpeed.GetInt(), SpeedStepMin, SpeedStepMax);
    SpeedCombo->setCurrentIndex(Speed);
    SetSpeedStep(Speed);
    connect(SpeedCombo, &QComboBox::currentIndexChanged, this, [this](const int InStep)
    {
        SetSpeedStep(InStep);
        Remember(GEditorViewCameraSpeed.SetInt(InStep));
    });
}

ViewportPanel::~ViewportPanel()
{
    /* A change still waiting for its save is written now. */
    if(SaveTimer && SaveTimer->isActive())
    {
        SaveTimer->stop();
        SettingsDialog::SaveUserSettings();
    }
}

void ViewportPanel::Remember(const bool bInChanged)
{
    if(bInChanged)
    {
        SaveTimer->start();
    }
}

void ViewportPanel::SetSpeedStep(const int InStep)
{
    Viewport->SetSpeedScale(std::pow(2.0f, float(InStep - SpeedStepOne)));
}

bool ViewportPanel::PickRenderTarget()
{
    const QStringList Targets = Context.GetViewableTargets();
    if(Targets.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Render Target"),
                                 QStringLiteral("No render targets yet: the viewport has not drawn a frame."));
        return false;
    }
    const int Current = std::max(0, int(Targets.indexOf(Context.GetViewTarget())));
    bool bOk = false;
    const QString Chosen = QInputDialog::getItem(this, QStringLiteral("Render Target"),
                                                 QStringLiteral("Show which render target in the viewport:"),
                                                 Targets, Current, /*bEditable*/ false, &bOk);
    if(!bOk || Chosen.isEmpty())
    {
        return false;
    }
    Context.SetViewMode(RenderCore::ERenderViewMode::RenderTarget, Chosen);
    return true;
}

void ViewportPanel::UpdateViewModeUI()
{
    const RenderCore::ERenderViewMode Mode = Context.GetViewMode();
    if(Mode != RenderCore::ERenderViewMode::RenderTarget)
    {
        Remember(GEditorViewMode.SetInt(Mode == RenderCore::ERenderViewMode::Unlit ? 1 : 0));
    }
    LitModeAction->setChecked(Mode == RenderCore::ERenderViewMode::Lit);
    UnlitModeAction->setChecked(Mode == RenderCore::ERenderViewMode::Unlit);
    TargetModeAction->setChecked(Mode == RenderCore::ERenderViewMode::RenderTarget);

    QString Text = QStringLiteral("Show");
    if(Mode == RenderCore::ERenderViewMode::Unlit)        { Text = QStringLiteral("Show: Unlit"); }
    if(Mode == RenderCore::ERenderViewMode::RenderTarget) { Text = QStringLiteral("Show: %1").arg(Context.GetViewTarget()); }
    ShowButton->setText(Text);
    ShowButton->setToolTip(QStringLiteral("View mode (Lit, Unlit, a render target) and the editor's gizmos"));
}

void ViewportPanel::SetBakedLighting(const bool bInBaked)
{
    if(EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Rendering", "BakedLighting")) { Remember(Var->SetBool(bInBaked)); }
    (bInBaked ? BakedAction : LiveAction)->setChecked(true);
    const GameCore::WorldLightingSettings& Lighting = Document.GetWorldSettings().Lighting;
    emit StatusMessage(!bInBaked ? QStringLiteral("Lighting: Live -- every light lit live")
                       : Lighting.bBakedLighting && !Lighting.BakedData.empty()
                         ? QStringLiteral("Lighting: Baked")
                         : QStringLiteral("Lighting: Baked -- but this world has no bake yet (Build > Bake Lightmaps)"));
}

#endif // WITH_EDITOR
