#if WITH_EDITOR

#include "Public/Editor/ViewportPanel.h"

#include <cmath>

#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QSlider>
#include <QToolBar>
#include <QVBoxLayout>

#include "Public/Editor/EditorCamera.h"
#include "Public/Editor/ViewportWidget.h"

namespace
{
    /* Speed is 2^(step - SpeedStepOne): 1/8x .. 8x, 1x in the middle. */
    constexpr int SpeedStepMin = 0;
    constexpr int SpeedStepMax = 6;
    constexpr int SpeedStepOne = 3;

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

    /* In EMode order: GizmoModeActions is indexed by it. */
    const GizmoModeButton GizmoModes[] =
    {
        { "Select", "No transform handles -- click to select (Q)",       TransformGizmo::EMode::None      },
        { "Move",   "Move handles: axes, planes, screen centre (W)",     TransformGizmo::EMode::Translate },
        { "Rotate", "Rotate rings: one per axis, the outer about the view (E)", TransformGizmo::EMode::Rotate },
        { "Scale",  "Scale handles: per axis, the centre for all three (R)",    TransformGizmo::EMode::Scale  },
    };

    const PresetButton Presets[] =
    {
        { "Persp",  "Perspective -- above and to the side",    EditorCamera::EPreset::Perspective },
        { "Front",  "Front -- looking along +Z (the game view)", EditorCamera::EPreset::Front       },
        { "Back",   "Back -- looking along -Z",                 EditorCamera::EPreset::Back        },
        { "Top",    "Top -- from screen-up (-Y), looking along +Y", EditorCamera::EPreset::Top     },
        { "Bottom", "Bottom -- from +Y, looking along -Y",      EditorCamera::EPreset::Bottom      },
        { "Left",   "Left -- from -X, looking along +X",        EditorCamera::EPreset::Left        },
        { "Right",  "Right -- from +X, looking along -X",       EditorCamera::EPreset::Right       },
    };
}

ViewportPanel::ViewportPanel(EditorContext& InContext, QWidget* InParent)
    : QWidget(InParent)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->setSpacing(0);

    Toolbar = new QToolBar(this);
    Toolbar->setIconSize(QSize(16, 16));
    Toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    Layout->addWidget(Toolbar);

    Viewport = new ViewportWidget(InContext, this);
    Layout->addWidget(Viewport, 1);

    // --- Transform handles ---------------------------------------------------
    /* No shortcuts here: the keys are the viewport's own (see ViewportWidget),
     * so typing Q W E R in a text field never switches modes. */
    auto* ModeGroup = new QActionGroup(this);
    ModeGroup->setExclusive(true);
    for(const GizmoModeButton& Button : GizmoModes)
    {
        QAction* Action = Toolbar->addAction(QString::fromLatin1(Button.Label));
        Action->setToolTip(QString::fromLatin1(Button.Tip));
        Action->setCheckable(true);
        ModeGroup->addAction(Action);
        const TransformGizmo::EMode Mode = Button.Mode;
        GizmoModeActions[int(Mode)] = Action;
        connect(Action, &QAction::triggered, this, [this, Mode]() { Viewport->SetGizmoMode(Mode); });
    }
    GizmoModeActions[int(Viewport->GetGizmoMode())]->setChecked(true);
    connect(Viewport, &ViewportWidget::GizmoModeChanged, this, [this](const TransformGizmo::EMode InMode)
    {
        GizmoModeActions[int(InMode)]->setChecked(true);
    });

    GizmoLocalAction = Toolbar->addAction(QStringLiteral("Local"));
    GizmoLocalAction->setCheckable(true);
    GizmoLocalAction->setToolTip(QStringLiteral("Move and rotate along the entity's own axes rather than the world's (X). "
                                                "Scale always uses the entity's axes."));
    GizmoLocalAction->setChecked(Viewport->GetGizmoSpace() == TransformGizmo::ESpace::Local);
    connect(GizmoLocalAction, &QAction::toggled, this, [this](const bool bInLocal)
    {
        Viewport->SetGizmoSpace(bInLocal ? TransformGizmo::ESpace::Local : TransformGizmo::ESpace::World);
    });
    connect(Viewport, &ViewportWidget::GizmoSpaceChanged, this, [this](const TransformGizmo::ESpace InSpace)
    {
        GizmoLocalAction->setChecked(InSpace == TransformGizmo::ESpace::Local);
    });

    GizmoSnapAction = Toolbar->addAction(QStringLiteral("Snap"));
    GizmoSnapAction->setCheckable(true);
    GizmoSnapAction->setToolTip(QStringLiteral("Snap handle drags to [Editor] SnapTranslate / SnapRotate / SnapScale "
                                               "(Ctrl during a drag inverts this)"));
    GizmoSnapAction->setChecked(Viewport->IsGizmoSnapEnabled());
    connect(GizmoSnapAction, &QAction::toggled, Viewport, &ViewportWidget::SetGizmoSnap);
    connect(Viewport, &ViewportWidget::GizmoSnapChanged, GizmoSnapAction, &QAction::setChecked);

    Toolbar->addSeparator();

    // --- View presets --------------------------------------------------------
    /* Each orbits about the selection (or the focus point) to its axis. */
    for(const PresetButton& Button : Presets)
    {
        QAction* Action = Toolbar->addAction(QString::fromLatin1(Button.Label));
        Action->setToolTip(QString::fromLatin1(Button.Tip));
        const EditorCamera::EPreset Preset = Button.Preset;
        connect(Action, &QAction::triggered, this, [this, Preset]() { Viewport->ApplyPreset(Preset); });
    }

    Toolbar->addSeparator();

    // --- Framing -------------------------------------------------------------
    FrameSelectionAction = Toolbar->addAction(QStringLiteral("Frame Selected"));
    FrameSelectionAction->setToolTip(QStringLiteral("Fit the selection's bounds in view (F)"));
    connect(FrameSelectionAction, &QAction::triggered, this, [this]() { Viewport->FrameSelection(); });

    FrameAllAction = Toolbar->addAction(QStringLiteral("Frame All"));
    FrameAllAction->setToolTip(QStringLiteral("Fit every entity in view (Home)"));
    connect(FrameAllAction, &QAction::triggered, this, [this]() { Viewport->FrameAll(); });

    Toolbar->addSeparator();

    // --- Bounds --------------------------------------------------------------
    ShowAllBoundsAction = Toolbar->addAction(QStringLiteral("All Bounds"));
    ShowAllBoundsAction->setCheckable(true);
    ShowAllBoundsAction->setToolTip(QStringLiteral("Draw every entity's bounds, not only the selection's"));
    connect(ShowAllBoundsAction, &QAction::toggled, this, [this](const bool bInOn) { Viewport->SetShowAllBounds(bInOn); });

    Toolbar->addSeparator();

    // --- Camera speed --------------------------------------------------------
    Toolbar->addWidget(new QLabel(QStringLiteral(" Speed "), Toolbar));

    Speed = new QSlider(Qt::Horizontal, Toolbar);
    Speed->setRange(SpeedStepMin, SpeedStepMax);
    Speed->setPageStep(1);
    Speed->setFixedWidth(110);
    Speed->setToolTip(QStringLiteral("Fly, pan and dolly speed"));
    Toolbar->addWidget(Speed);

    SpeedLabel = new QLabel(Toolbar);
    SpeedLabel->setMinimumWidth(40);
    Toolbar->addWidget(SpeedLabel);

    connect(Speed, &QSlider::valueChanged, this, &ViewportPanel::SetSpeedStep);
    Speed->setValue(SpeedStepOne);
    SetSpeedStep(SpeedStepOne);
}

void ViewportPanel::SetSpeedStep(const int InStep)
{
    const float Scale = std::pow(2.0f, float(InStep - SpeedStepOne));
    Viewport->SetSpeedScale(Scale);
    SpeedLabel->setText(Scale < 1.0f ? QStringLiteral(" 1/%1x").arg(int(std::lround(1.0f / Scale)))
                                     : QStringLiteral(" %1x").arg(int(std::lround(Scale))));
}

#endif // WITH_EDITOR
