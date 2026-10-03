#if WITH_EDITOR

#include "Public/Editor/ViewportPanel.h"

#include <cmath>

#include <QAction>
#include <QComboBox>
#include <QMenu>
#include <QToolBar>
#include <QToolButton>
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

    /* In EMode order: the mode combo's index is the mode. */
    const GizmoModeButton GizmoModes[] =
    {
        { "Select", "No transform handles -- click to select (Q)",       TransformGizmo::EMode::None      },
        { "Move",   "Move handles: axes, planes, screen centre (W)",     TransformGizmo::EMode::Translate },
        { "Rotate", "Rotate rings: one per axis, the outer about the view (E)", TransformGizmo::EMode::Rotate },
        { "Scale",  "Scale handles: per axis, the centre for all three (R)",    TransformGizmo::EMode::Scale  },
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

    // --- Transform handles ---------------------------------------------------
    /* No shortcuts here: the keys are the viewport's own (see ViewportWidget),
     * so typing Q W E R in a text field never switches modes. */
    ModeCombo = MakeCombo(Toolbar, QStringLiteral("Transform handles (Q / W / E / R)"));
    for(const GizmoModeButton& Button : GizmoModes)
    {
        ModeCombo->addItem(QString::fromLatin1(Button.Label));
        ModeCombo->setItemData(ModeCombo->count() - 1, QString::fromLatin1(Button.Tip), Qt::ToolTipRole);
    }
    ModeCombo->setCurrentIndex(int(Viewport->GetGizmoMode()));
    connect(ModeCombo, &QComboBox::currentIndexChanged, this, [this](const int InIndex)
    {
        Viewport->SetGizmoMode(TransformGizmo::EMode(InIndex));
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
    connect(GizmoSnapAction, &QAction::toggled, Viewport, &ViewportWidget::SetGizmoSnap);
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

    // --- Bounds --------------------------------------------------------------
    ShowAllBoundsAction = Toolbar->addAction(QStringLiteral("All Bounds"));
    ShowAllBoundsAction->setCheckable(true);
    ShowAllBoundsAction->setToolTip(QStringLiteral("Draw every entity's bounds, not only the selection's"));
    connect(ShowAllBoundsAction, &QAction::toggled, this, [this](const bool bInOn) { Viewport->SetShowAllBounds(bInOn); });

    Toolbar->addSeparator();

    // --- Camera speed --------------------------------------------------------
    SpeedCombo = MakeCombo(Toolbar, QStringLiteral("Camera speed: fly, pan and dolly"));
    for(int Step = SpeedStepMin; Step <= SpeedStepMax; ++Step)
    {
        const int Power = Step - SpeedStepOne;
        SpeedCombo->addItem(Power < 0 ? QStringLiteral("Speed 1/%1x").arg(1 << -Power)
                                      : QStringLiteral("Speed %1x").arg(1 << Power));
    }
    connect(SpeedCombo, &QComboBox::currentIndexChanged, this, &ViewportPanel::SetSpeedStep);
    SpeedCombo->setCurrentIndex(SpeedStepOne);
    SetSpeedStep(SpeedStepOne);
}

void ViewportPanel::SetSpeedStep(const int InStep)
{
    Viewport->SetSpeedScale(std::pow(2.0f, float(InStep - SpeedStepOne)));
}

#endif // WITH_EDITOR
