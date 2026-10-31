#if WITH_EDITOR

#include "Private/Editor/PropertyWidgets.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>

#include <QDir>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QToolTip>

#include "CoreQtConversions.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Vector.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/EditorIcons.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Particles/ParticleAssets.h"
#include "Private/Editor/Sequencer/SequencerAssets.h"
#include "Private/Editor/Content/RetargetEditorWindow.h"
#if WITH_AICORE
#include "Private/Editor/AI/BehaviorTreeEditorWindow.h"
#endif

using GameCore::EPropertyType;

// === PropertyEditor ==========================================================

PropertyEditor::PropertyEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
    : QWidget(InParent)
    , Entry(InEntry)
    , Resolve(std::move(InResolve))
{
    if(Entry.Tooltip)
    {
        setToolTip(QString::fromUtf8(Entry.Tooltip));
    }
}

void PropertyEditor::Refresh(const bool bInForce)
{
    if(!bInForce && IsBeingEdited())
    {
        return;
    }
    if(const uint8_t* Component = Resolve ? Resolve() : nullptr)
    {
        ReadFrom(Component + Entry.Offset);
    }
}

uint8_t* PropertyEditor::FieldForWrite() const
{
    if(Entry.IsReadOnly() || !Resolve)
    {
        return nullptr;
    }
    uint8_t* Component = Resolve();
    return Component ? Component + Entry.Offset : nullptr;
}

bool PropertyEditor::IsBeingEdited() const
{
    const QWidget* Focused = QApplication::focusWidget();
    return Focused != nullptr && isAncestorOf(Focused);
}

namespace
{
    /* The drag behind InstallScrub: the value at the press, plus the pixels
     * moved since times the step. */
    class ScrubFilter final : public QObject
    {
    public:
        ScrubFilter(QAbstractSpinBox* InSpin, QObject* InParent) : QObject(InParent), Spin(InSpin) {}

    protected:
        bool eventFilter(QObject* InWatched, QEvent* InEvent) override
        {
            if(!Spin || Spin->isReadOnly() || !Spin->isEnabled())
            {
                return QObject::eventFilter(InWatched, InEvent);
            }
            switch(InEvent->type())
            {
                case QEvent::MouseButtonPress:
                {
                    const auto* Press = static_cast<QMouseEvent*>(InEvent);
                    if(Press->button() != Qt::LeftButton) { break; }
                    bPressed  = true;
                    bDragging = false;
                    PressX    = Press->globalPosition().x();
                    if(const auto* Real = qobject_cast<QDoubleSpinBox*>(Spin)) { StartValue = Real->value(); }
                    if(const auto* Whole = qobject_cast<QSpinBox*>(Spin))      { StartValue = Whole->value(); }
                    return true;
                }
                case QEvent::MouseMove:
                {
                    if(!bPressed) { break; }
                    const auto* Move = static_cast<QMouseEvent*>(InEvent);
                    const double Pixels = Move->globalPosition().x() - PressX;
                    /* A few pixels first: a click on the label is not a drag. */
                    if(!bDragging && std::abs(Pixels) < 3.0) { return true; }
                    bDragging = true;
                    double Scale = 1.0;
                    if(Move->modifiers() & Qt::ShiftModifier)   { Scale *= 10.0; }
                    if(Move->modifiers() & Qt::ControlModifier) { Scale *= 0.1; }
                    if(auto* Real = qobject_cast<QDoubleSpinBox*>(Spin))
                    {
                        Real->setValue(StartValue + Pixels * Real->singleStep() * Scale);
                    }
                    else if(auto* Whole = qobject_cast<QSpinBox*>(Spin))
                    {
                        /* Integers: a step per few pixels, or every one is a jump. */
                        Whole->setValue(int(StartValue + std::round(Pixels * 0.25 * Scale) * Whole->singleStep()));
                    }
                    return true;
                }
                case QEvent::MouseButtonRelease:
                    if(!bPressed) { break; }
                    bPressed = false;
                    return bDragging;
                default:
                    break;
            }
            return QObject::eventFilter(InWatched, InEvent);
        }

    private:
        QPointer<QAbstractSpinBox> Spin;
        bool   bPressed   = false;
        bool   bDragging  = false;
        double PressX     = 0.0;
        double StartValue = 0.0;
    };

    QDoubleSpinBox* MakeRealSpin(QWidget* InParent, const GameCore::ComponentPropertyEntry& InEntry)
    {
        const bool bInReadOnly = InEntry.IsReadOnly();
        auto* Spin = new QDoubleSpinBox(InParent);
        Spin->setRange(InEntry.HasRange() ? double(InEntry.Min) : -1.0e9, InEntry.HasRange() ? double(InEntry.Max) : 1.0e9);
        Spin->setDecimals(3);
        /* A narrow range (0..1, say) steps in hundredths. */
        Spin->setSingleStep(InEntry.HasRange() && InEntry.Max - InEntry.Min <= 2.0f ? 0.01 : 1.0);
        Spin->setKeyboardTracking(false);   // write on Enter / focus-out / arrows, not per keystroke
        Spin->setReadOnly(bInReadOnly);
        Spin->setButtonSymbols(bInReadOnly ? QAbstractSpinBox::NoButtons : QAbstractSpinBox::UpDownArrows);
        Spin->setMinimumWidth(60);
        Spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return Spin;
    }

    /* Only on a change (after the box's rounding): setValue always repaints,
     * and the 10 Hz refresh cost a third of the viewport's frame rate. */
    void SetSpin(QDoubleSpinBox* InSpin, const double InValue)
    {
        if(InSpin->value() == InSpin->valueFromText(InSpin->textFromValue(InValue)))
        {
            return;
        }
        const QSignalBlocker Block(InSpin);
        InSpin->setValue(InValue);
    }

    /* A small "X" / "Y" / "Z" caption in front of a vector component: in the
     * gizmo's axis colours (red, green, blue), toned to read on either theme;
     * other captions ("Min") muted. */
    QLabel* AxisLabel(const char* InText, QWidget* InParent)
    {
        auto* Label = new QLabel(QString::fromLatin1(InText), InParent);
        const char* Color = std::strcmp(InText, "X") == 0 ? "#e0605a"
                          : std::strcmp(InText, "Y") == 0 ? "#6cbf52"
                          : std::strcmp(InText, "Z") == 0 ? "#5a8fe0" : nullptr;
        Label->setStyleSheet(Color ? QStringLiteral("color: %1; font-weight: bold;").arg(QLatin1String(Color))
                                   : QStringLiteral("color: palette(mid);"));
        return Label;
    }

    // --- Scalars -------------------------------------------------------------

    class RealEditor final : public PropertyEditor
    {
    public:
        RealEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
            : PropertyEditor(InEntry, std::move(InResolve), InParent)
        {
            auto* Layout = new QHBoxLayout(this);
            Layout->setContentsMargins(0, 0, 0, 0);
            Spin = MakeRealSpin(this, Entry);
            Layout->addWidget(Spin);

            connect(Spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](const double InValue)
            {
                uint8_t* Field = FieldForWrite();
                if(!Field) { return; }
                if(Entry.Type == EPropertyType::Double)
                {
                    std::memcpy(Field, &InValue, sizeof(double));
                }
                else
                {
                    const float Value = float(InValue);
                    std::memcpy(Field, &Value, sizeof(float));
                }
                NotifyWritten();
            });
        }

    protected:
        void ReadFrom(const uint8_t* InField) override
        {
            if(Entry.Type == EPropertyType::Double)
            {
                double Value; std::memcpy(&Value, InField, sizeof(double));
                SetSpin(Spin, Value);
            }
            else
            {
                float Value; std::memcpy(&Value, InField, sizeof(float));
                SetSpin(Spin, double(Value));
            }
        }

    public:
        QAbstractSpinBox* GetScrubSpin() const override { return Spin; }

    private:
        QDoubleSpinBox* Spin = nullptr;
    };

    /* A choice (Props.Choice): the field's integer as a dropdown of labels. */
    class ChoiceEditor final : public PropertyEditor
    {
    public:
        ChoiceEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
            : PropertyEditor(InEntry, std::move(InResolve), InParent)
        {
            auto* Layout = new QHBoxLayout(this);
            Layout->setContentsMargins(0, 0, 0, 0);
            Combo = new QComboBox(this);
            Combo->addItems(QString::fromUtf8(Entry.Choices).split(QLatin1Char('|')));
            Combo->setEnabled(!Entry.IsReadOnly());
            Layout->addWidget(Combo);

            connect(Combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int InIndex)
            {
                uint8_t* Field = FieldForWrite();
                if(!Field || InIndex < 0) { return; }
                if(Entry.Type == EPropertyType::UInt8)
                {
                    *Field = uint8_t(InIndex);
                }
                else
                {
                    const int32_t Value = int32_t(InIndex);
                    std::memcpy(Field, &Value, sizeof(int32_t));
                }
                NotifyWritten();
            });
        }

    protected:
        void ReadFrom(const uint8_t* InField) override
        {
            int Value = 0;
            if(Entry.Type == EPropertyType::UInt8)
            {
                Value = int(*InField);
            }
            else
            {
                int32_t Raw; std::memcpy(&Raw, InField, sizeof(int32_t));
                Value = int(Raw);
            }
            /* A value past the labels shows as no selection, not a wrong one. */
            const int Index = Value >= 0 && Value < Combo->count() ? Value : -1;
            if(Combo->currentIndex() != Index)
            {
                const QSignalBlocker Block(Combo);
                Combo->setCurrentIndex(Index);
            }
        }

    private:
        QComboBox* Combo = nullptr;
    };

    class IntEditor final : public PropertyEditor
    {
    public:
        IntEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
            : PropertyEditor(InEntry, std::move(InResolve), InParent)
        {
            auto* Layout = new QHBoxLayout(this);
            Layout->setContentsMargins(0, 0, 0, 0);
            Spin = new QSpinBox(this);
            const int Lowest  = Entry.Type == EPropertyType::UInt8 ? 0 : INT_MIN;
            const int Highest = Entry.Type == EPropertyType::UInt8 ? 255 : INT_MAX;
            if(Entry.HasRange())
            {
                Spin->setRange(std::max(Lowest, int(std::ceil(Entry.Min))), std::min(Highest, int(std::floor(Entry.Max))));
            }
            else
            {
                Spin->setRange(Lowest, Highest);
            }
            Spin->setKeyboardTracking(false);
            Spin->setReadOnly(Entry.IsReadOnly());
            Spin->setButtonSymbols(Entry.IsReadOnly() ? QAbstractSpinBox::NoButtons : QAbstractSpinBox::UpDownArrows);
            Layout->addWidget(Spin);

            connect(Spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](const int InValue)
            {
                uint8_t* Field = FieldForWrite();
                if(!Field) { return; }
                if(Entry.Type == EPropertyType::UInt8)
                {
                    *Field = uint8_t(InValue);
                }
                else
                {
                    const int32_t Value = int32_t(InValue);
                    std::memcpy(Field, &Value, sizeof(int32_t));
                }
                NotifyWritten();
            });
        }

    protected:
        void ReadFrom(const uint8_t* InField) override
        {
            int Value = 0;
            if(Entry.Type == EPropertyType::UInt8)
            {
                Value = int(*InField);
            }
            else
            {
                int32_t Raw; std::memcpy(&Raw, InField, sizeof(int32_t));
                Value = int(Raw);
            }
            /* Unchanged is skipped -- see SetSpin. */
            if(Spin->value() != Value)
            {
                const QSignalBlocker Block(Spin);
                Spin->setValue(Value);
            }
        }

    public:
        QAbstractSpinBox* GetScrubSpin() const override { return Spin; }

    private:
        QSpinBox* Spin = nullptr;
    };

    /* Bool (a C++ bool) and Flag (a uint8_t 0/1) -- both one byte, both a checkbox. */
    class BoolEditor final : public PropertyEditor
    {
    public:
        BoolEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
            : PropertyEditor(InEntry, std::move(InResolve), InParent)
        {
            auto* Layout = new QHBoxLayout(this);
            Layout->setContentsMargins(0, 0, 0, 0);
            Check = new QCheckBox(this);
            Check->setEnabled(!Entry.IsReadOnly());
            Layout->addWidget(Check);
            Layout->addStretch(1);

            connect(Check, &QCheckBox::toggled, this, [this](const bool bInChecked)
            {
                uint8_t* Field = FieldForWrite();
                if(!Field) { return; }
                if(Entry.Type == EPropertyType::Bool)
                {
                    const bool Value = bInChecked;
                    std::memcpy(Field, &Value, sizeof(bool));
                }
                else
                {
                    *Field = bInChecked ? 1 : 0;
                }
                NotifyWritten();
            });
        }

    protected:
        void ReadFrom(const uint8_t* InField) override
        {
            const bool bValue = *InField != 0;
            if(Check->isChecked() != bValue)
            {
                const QSignalBlocker Block(Check);
                Check->setChecked(bValue);
            }
        }

    private:
        QCheckBox* Check = nullptr;
    };

    // --- Vectors -------------------------------------------------------------

    /* Vector3 and AABB, always via GetX/SetX (layout differs under SIMD). */
    class VectorEditor final : public PropertyEditor
    {
    public:
        VectorEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
            : PropertyEditor(InEntry, std::move(InResolve), InParent)
        {
            const bool bIsBox = Entry.Type == EPropertyType::AABB;
            const int  Rows   = bIsBox ? 2 : 1;

            auto* Layout = new QGridLayout(this);
            Layout->setContentsMargins(0, 0, 0, 0);
            Layout->setHorizontalSpacing(4);

            static const char* const Axes[3] = { "X", "Y", "Z" };
            for(int Row = 0; Row < Rows; ++Row)
            {
                int Column = 0;
                if(bIsBox)
                {
                    Layout->addWidget(AxisLabel(Row == 0 ? "Min" : "Max", this), Row, Column++);
                }
                for(int Axis = 0; Axis < 3; ++Axis)
                {
                    QLabel* Caption = AxisLabel(Axes[Axis], this);
                    Layout->addWidget(Caption, Row, Column++);
                    QDoubleSpinBox* Spin = MakeRealSpin(this, Entry);
                    Spins[Row * 3 + Axis] = Spin;
                    InstallScrub(Caption, Spin);
                    Layout->addWidget(Spin, Row, Column++);

                    connect(Spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, Row, Axis](const double InValue)
                    {
                        uint8_t* Field = FieldForWrite();
                        if(!Field) { return; }
                        EngineCore::Vector3& V = VectorAt(Field, Row);
                        switch(Axis)
                        {
                            case 0:  V.SetX(float(InValue)); break;
                            case 1:  V.SetY(float(InValue)); break;
                            default: V.SetZ(float(InValue)); break;
                        }
                        NotifyWritten();
                    });
                }
            }
        }

    protected:
        void ReadFrom(const uint8_t* InField) override
        {
            const int Rows = Entry.Type == EPropertyType::AABB ? 2 : 1;
            for(int Row = 0; Row < Rows; ++Row)
            {
                const EngineCore::Vector3& V = VectorAt(const_cast<uint8_t*>(InField), Row);
                SetSpin(Spins[Row * 3 + 0], double(V.GetX()));
                SetSpin(Spins[Row * 3 + 1], double(V.GetY()));
                SetSpin(Spins[Row * 3 + 2], double(V.GetZ()));
            }
        }

    private:
        EngineCore::Vector3& VectorAt(uint8_t* InField, const int InRow) const
        {
            if(Entry.Type == EPropertyType::AABB)
            {
                auto& Box = *reinterpret_cast<GameCore::AABB*>(InField);
                return InRow == 0 ? Box.Min : Box.Max;
            }
            return *reinterpret_cast<EngineCore::Vector3*>(InField);
        }

        QDoubleSpinBox* Spins[6] = {};
    };

    // --- Color ---------------------------------------------------------------

    class ColorEditor final : public PropertyEditor
    {
    public:
        ColorEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
            : PropertyEditor(InEntry, std::move(InResolve), InParent)
        {
            auto* Layout = new QHBoxLayout(this);
            Layout->setContentsMargins(0, 0, 0, 0);
            Swatch = new QPushButton(this);
            Swatch->setFixedSize(48, 20);
            Swatch->setEnabled(!Entry.IsReadOnly());
            Text = new QLabel(this);
            Layout->addWidget(Swatch);
            Layout->addWidget(Text, 1);

            connect(Swatch, &QPushButton::clicked, this, [this]()
            {
                const QColor Chosen = QColorDialog::getColor(Current, this, QStringLiteral("Choose %1").arg(QString::fromLatin1(Entry.Name)),
                                                             QColorDialog::ShowAlphaChannel);
                if(!Chosen.isValid()) { return; }
                uint8_t* Field = FieldForWrite();
                if(!Field) { return; }
                const EngineCore::Color Value = EngineCore::QtBridge::FromQColor(Chosen);
                std::memcpy(Field, &Value, sizeof(EngineCore::Color));
                Show(Chosen);
                NotifyWritten();
            });
        }

    protected:
        void ReadFrom(const uint8_t* InField) override
        {
            EngineCore::Color Value; std::memcpy(&Value, InField, sizeof(EngineCore::Color));
            Show(EngineCore::QtBridge::ToQColor(Value));
        }

    private:
        void Show(const QColor& InColor)
        {
            /* setStyleSheet re-polishes the widget even for the same sheet --
             * the costliest call in a refresh. Only on a real change. */
            if(InColor == Current && Swatch->styleSheet().size() != 0)
            {
                return;
            }
            Current = InColor;
            Swatch->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid palette(mid);").arg(InColor.name()));
            Text->setText(QStringLiteral("%1, %2, %3, %4").arg(InColor.red()).arg(InColor.green()).arg(InColor.blue()).arg(InColor.alpha()));
        }

        QPushButton* Swatch = nullptr;
        QLabel*      Text   = nullptr;
        QColor       Current;
    };

    // --- Fixed string --------------------------------------------------------

    class StringEditor final : public PropertyEditor
    {
    public:
        StringEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
            : PropertyEditor(InEntry, std::move(InResolve), InParent)
        {
            auto* Layout = new QHBoxLayout(this);
            Layout->setContentsMargins(0, 0, 0, 0);
            Edit = new QLineEdit(this);
            Edit->setReadOnly(Entry.IsReadOnly());
            Layout->addWidget(Edit);

            /* Committed on Enter or focus-out, not per keystroke. */
            connect(Edit, &QLineEdit::editingFinished, this, [this]() { Commit(); });

            /* An asset field takes the asset dragged from the Content Browser
             * (or Explorer, if it is in the project), or one picked; a material
             * field takes material graphs (*.material.nmgraph) only, and
             * completes the project's and the engine's. */
            const bool bMesh     = (Entry.Flags & GameCore::PF_AssetMesh) != 0;
            const bool bMaterial = (Entry.Flags & GameCore::PF_AssetMaterial) != 0;
            const bool bPost     = (Entry.Flags & GameCore::PF_AssetPostProcess) != 0;
            const bool bTarget   = (Entry.Flags & GameCore::PF_AssetRenderTarget) != 0;
            const bool bTexture  = (Entry.Flags & GameCore::PF_AssetTexture) != 0;
            const bool bParticles = (Entry.Flags & GameCore::PF_AssetParticleSystem) != 0;
            const bool bAnimation = (Entry.Flags & GameCore::PF_AssetAnimation) != 0;
            const bool bSound     = (Entry.Flags & GameCore::PF_AssetSound) != 0;
            const bool bPrefab    = (Entry.Flags & GameCore::PF_AssetPrefab) != 0;
            const bool bSequence  = (Entry.Flags & GameCore::PF_AssetSequence) != 0;
            const bool bTerrain   = (Entry.Flags & GameCore::PF_AssetTerrain) != 0;
            const bool bFoliage   = (Entry.Flags & GameCore::PF_AssetFoliage) != 0;
            const bool bNavMesh   = (Entry.Flags & GameCore::PF_AssetNavMesh) != 0;
            const bool bTree      = (Entry.Flags & GameCore::PF_AssetBehaviorTree) != 0;
            const bool bRetarget  = (Entry.Flags & GameCore::PF_AssetRetarget) != 0;
            const bool bAssetField = bMesh || bMaterial || bPost || bTarget || bTexture || bParticles || bAnimation || bSound
                                  || bPrefab || bSequence || bTerrain || bFoliage || bNavMesh || bTree || bRetarget;

            /* The eye: the Content Browser goes to the asset's folder and opens it in its editor. */
            if(bAssetField)
            {
                Layout->addWidget(EditorContent::MakeRevealButton([this]() { return Edit->text(); }, this));
            }

            if(bAssetField && !Entry.IsReadOnly())
            {
                /* What each kind of asset field offers. */
                struct AssetUi { EAsset Kind; const char* Placeholder; const char* Tip; const char* PickTip; const char* Title; const char* Filter; };
                static const AssetUi Kinds[] = {
                    { EAsset::Mesh, "Drop a .nmmesh here", nullptr, "Choose a mesh in the project", "Choose a Mesh", "Meshes (*.nmmesh)" },
                    { EAsset::Material, "Drop a .material.nmgraph here",
                      "A material graph, by its path: Content/Materials/Glow.material.nmgraph",
                      "Choose a material graph", "Choose a Material", "Material Graphs (*.material.nmgraph)" },
                    { EAsset::PostProcess, "Drop a .pp.nmgraph here",
                      "A post process stack, by its path: Content/PostProcess/Arch.pp.nmgraph -- or a PostProcess material graph",
                      "Choose a post process stack", "Choose a Post Process Stack",
                      "Post Process Stacks (*.pp.nmgraph);;PostProcess Materials (*.material.nmgraph)" },
                    { EAsset::RenderTarget, "Drop an .nmrt2d or .nmrt3d here",
                      "A render target, by its path: Content/RenderTargets/Pool.nmrt2d (RenderTargetTex2D, a planar mirror) or "
                      "Content/RenderTargets/Ball.nmrt3d (RenderTargetTex3D, omnidirectional)",
                      "Choose a render target", "Choose a Render Target", "Render Targets (*.nmrt2d *.nmrt3d)" },
                    { EAsset::Texture, "Drop an .nmtexture here",
                      "A texture, by its path: Content/Textures/Sky.nmtexture (an HDRI imported from a Radiance .hdr, for a Sky Light)",
                      "Choose a texture", "Choose a Texture", "Textures (*.nmtexture)" },
                    { EAsset::ParticleSystem, "Drop an .nmparticle here (empty: one default emitter)",
                      "A particle system, by its path: Content/Particles/Smoke.nmparticle -- made and edited in the Particle Editor",
                      "Choose a particle system", "Choose a Particle System", "Particle Systems (*.nmparticle)" },
                    { EAsset::Animation, "Drop an .nmanim or .nmanimstate here",
                      "An animation clip (Content/Animation/Fox_Walk.nmanim), played on its own, or an anim state "
                      "(Content/Animation/Fox.nmanimstate): states, blends and layers driven by the parameters",
                      "Choose an animation", "Choose an Animation", "Animations (*.nmanim *.nmanimstate)" },
                    { EAsset::Sound, "Drop an .nmsound here",
                      "A sound, by its path: Content/Sounds/EngineLoop.nmsound -- imported from a .wav, .flac, .mp3 or .ogg",
                      "Choose a sound", "Choose a Sound", "Sounds (*.nmsound)" },
                    { EAsset::Prefab, "Drop an .nmprefab here",
                      "A prefab, by its path: Content/Prefabs/Pawn.nmprefab -- or a native one, native:Game.Name",
                      "Choose a prefab", "Choose a Prefab", "Prefabs (*.nmprefab)" },
                    { EAsset::Sequence, "Drop an .nmseq here",
                      "A sequence, by its path: Content/Cinematics/Intro.nmseq -- made and edited in the Sequencer",
                      "Choose a sequence", "Choose a Sequence", "Sequences (*.nmseq)" },
                    { EAsset::Terrain, "Drop an .nmterrain here",
                      "A terrain heightfield, by its path: Content/Levels/Hills/Terrain/Ground.nmterrain -- made with World > Terrain",
                      "Choose a terrain", "Choose a Terrain", "Terrains (*.nmterrain)" },
                    { EAsset::Foliage, "Drop an .nmfoliage here",
                      "A terrain's foliage, by its path: Content/Levels/Hills/Terrain/Ground.nmfoliage -- made by the Terrain dock's Foliage tab",
                      "Choose foliage", "Choose Foliage", "Foliage (*.nmfoliage)" },
                    { EAsset::NavMesh, "Drop an .nmnav here (empty: beside the level)",
                      "A baked navmesh, by its path: Content/Levels/Arena.nmnav -- written by World > Navigation > Build",
                      "Choose a navmesh", "Choose a Navmesh", "Navmeshes (*.nmnav)" },
                    { EAsset::BehaviorTree, "Drop an .nmbt here",
                      "A behavior tree, by its path: Content/AI/Guard.nmbt -- made and edited in the behavior tree editor",
                      "Choose a behavior tree", "Choose a Behavior Tree", "Behavior Trees (*.nmbt)" },
                    { EAsset::Retarget, "Drop an .nmretarget here (empty: the anim state's)",
                      "A retarget, by its path: Content/Characters/Mixamo_to_Hero.nmretarget -- clips made for its source "
                      "skeletons play on this mesh's (its target)",
                      "Choose a retarget", "Choose a Retarget", "Retargets (*.nmretarget)" },
                };
                const AssetUi& Ui = Kinds[bMesh ? 0 : bMaterial ? 1 : bPost ? 2 : bTarget ? 3 : bTexture ? 4 : bParticles ? 5
                                          : bAnimation ? 6 : bSound ? 7 : bPrefab ? 8 : bSequence ? 9 : bTerrain ? 10
                                          : bFoliage ? 11 : bNavMesh ? 12 : bTree ? 13 : 14];
                AssetKind = Ui.Kind;
                Edit->setPlaceholderText(QString::fromLatin1(Ui.Placeholder));
                Edit->installEventFilter(this);
                if(!bMesh)
                {
                    auto* Completer = new QCompleter(bMaterial  ? EditorContent::ListMaterialPaths()
                                                     : bPost      ? EditorContent::ListPostProcessPaths()
                                                     : bTarget    ? EditorContent::ListRenderTargetPaths()
                                                     : bParticles ? EditorContent::ListParticleSystemPaths()
                                                     : bAnimation ? EditorContent::ListAnimationPaths()
                                                     : bSound     ? EditorContent::ListSoundPaths()
                                                     : bPrefab    ? EditorContent::ListPrefabPaths()
                                                     : bSequence  ? EditorContent::ListSequencePaths()
                                                     : bTerrain   ? EditorContent::ListTerrainPaths()
                                                     : bFoliage   ? EditorContent::ListFoliagePaths()
                                                     : bNavMesh   ? EditorContent::ListNavMeshPaths()
                                                     : bTree      ? EditorContent::ListBehaviorTreePaths()
                                                     : bRetarget  ? EditorContent::ListRetargetPaths()
                                                                  : EditorContent::ListTexturePaths(), Edit);
                    Completer->setCaseSensitivity(Qt::CaseInsensitive);
                    Completer->setFilterMode(Qt::MatchContains);
                    Edit->setCompleter(Completer);
                    Edit->setToolTip(QString::fromLatin1(Ui.Tip));
                }

                auto* Pick = new QToolButton(this);
                Pick->setIcon(EditorIcons::Get(EditorIcons::EIcon::Browse));
                Pick->setToolTip(QString::fromLatin1(Ui.PickTip));
                Layout->addWidget(Pick);
                const QString Title  = QString::fromLatin1(Ui.Title);
                const QString Filter = QString::fromLatin1(Ui.Filter);
                connect(Pick, &QToolButton::clicked, this, [this, Title, Filter]()
                {
                    const QString Chosen = QFileDialog::getOpenFileName(this, Title, EditorGraphs::GetProjectRoot(), Filter);
                    const QString Value = Chosen.isEmpty() ? QString() : ValueForFile(Chosen);
                    if(!Value.isEmpty()) { SetAndCommit(Value); }
                });

                /* A sequence opens in the Sequencer, made first if the field is empty. */
                if(bSequence)
                {
                    auto* Open = new QToolButton(this);
                    Open->setText(QStringLiteral("Edit"));
                    Open->setToolTip(QStringLiteral("Open this sequence in the Sequencer (empty: make a new one here)"));
                    Layout->addWidget(Open);
                    connect(Open, &QToolButton::clicked, this, [this]()
                    {
                        const QString Runtime = Edit->text().trimmed();
                        if(Runtime.isEmpty())
                        {
                            const QString Made = EditorSequencer::CreateSequence(QString(), window(), false);
                            const QString Value = Made.isEmpty() ? QString() : ValueForFile(Made);
                            if(!Value.isEmpty())
                            {
                                SetAndCommit(Value);
                                EditorSequencer::OpenSequence(Made, window());
                            }
                            return;
                        }
                        EditorSequencer::OpenSequence(EditorParticles::ToProjectPath(Runtime), window());
                    });
                }

#if WITH_AICORE
                /* A behavior tree opens in its editor, made first if the field is empty. */
                if(bTree)
                {
                    auto* Open = new QToolButton(this);
                    Open->setText(QStringLiteral("Edit"));
                    Open->setToolTip(QStringLiteral("Open this behavior tree in its editor (empty: make a new one)"));
                    Layout->addWidget(Open);
                    connect(Open, &QToolButton::clicked, this, [this]()
                    {
                        const QString Runtime = Edit->text().trimmed();
                        if(Runtime.isEmpty())
                        {
                            const QString Folder = QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content/AI"));
                            if(BehaviorTreeEditorWindow* Made = BehaviorTreeEditorWindow::CreateIn(Folder, window()))
                            {
                                const QString Value = ValueForFile(Made->GetPath());
                                if(!Value.isEmpty()) { SetAndCommit(Value); }
                            }
                            return;
                        }
                        const QString File = EditorContent::FileForRuntimePath(Runtime);
                        if(!File.isEmpty()) { BehaviorTreeEditorWindow::Open(File, window()); }
                    });
                }
#endif

                /* A retarget opens in its editor. */
                if(bRetarget)
                {
                    auto* Open = new QToolButton(this);
                    Open->setText(QStringLiteral("Edit"));
                    Open->setToolTip(QStringLiteral("Open this retarget in the retarget editor"));
                    Layout->addWidget(Open);
                    connect(Open, &QToolButton::clicked, this, [this]()
                    {
                        const QString File = EditorContent::FileForRuntimePath(Edit->text().trimmed());
                        if(!File.isEmpty()) { RetargetEditorWindow::Open(File, window()); }
                    });
                }

                /* A particle system opens in the Particle Editor, made first if the field is empty. */
                if(bParticles)
                {
                    auto* Open = new QToolButton(this);
                    Open->setText(QStringLiteral("Edit"));
                    Open->setToolTip(QStringLiteral("Open this particle system in the Particle Editor (empty: make a new one here)"));
                    Layout->addWidget(Open);
                    connect(Open, &QToolButton::clicked, this, [this]()
                    {
                        const QString Runtime = Edit->text().trimmed();
                        if(Runtime.isEmpty())
                        {
                            const QString Made = EditorParticles::CreateParticleSystem(QString(), window(), false);
                            const QString Value = Made.isEmpty() ? QString() : ValueForFile(Made);
                            if(!Value.isEmpty())
                            {
                                SetAndCommit(Value);
                                EditorParticles::OpenParticleSystem(Made, window());
                            }
                            return;
                        }
                        EditorParticles::OpenParticleSystem(EditorParticles::ToProjectPath(Runtime), window());
                    });
                }
            }
        }

    protected:
        bool eventFilter(QObject* InWatched, QEvent* InEvent) override
        {
            if(InWatched == Edit && AssetKind != EAsset::None)
            {
                if(InEvent->type() == QEvent::DragEnter || InEvent->type() == QEvent::DragMove)
                {
                    auto* Drag = static_cast<QDropEvent*>(InEvent);
                    const QString File = EditorContent::SingleDroppedFile(Drag->mimeData());
                    if(!File.isEmpty() && !ValueForFile(File).isEmpty()) { Drag->acceptProposedAction(); }
                    else { Drag->ignore(); }
                    return true;
                }
                if(InEvent->type() == QEvent::Drop)
                {
                    auto* Drop = static_cast<QDropEvent*>(InEvent);
                    const QString Value = ValueForFile(EditorContent::SingleDroppedFile(Drop->mimeData()));
                    if(!Value.isEmpty())
                    {
                        SetAndCommit(Value);
                        Drop->acceptProposedAction();
                    }
                    return true;
                }
            }
            return PropertyEditor::eventFilter(InWatched, InEvent);
        }

        void ReadFrom(const uint8_t* InField) override
        {
            /* strnlen bound: a buffer written by something else might lack its NUL. */
            const char* Text = reinterpret_cast<const char*>(InField);
            size_t Length = 0;
            while(Length < Entry.Size && Text[Length] != '\0') { ++Length; }

            const QString Value = QString::fromUtf8(Text, qsizetype(Length));
            if(Edit->text() != Value)
            {
                const QSignalBlocker Block(Edit);
                Edit->setText(Value);
            }
        }

    private:
        enum class EAsset { None, Mesh, Material, PostProcess, RenderTarget, Texture, ParticleSystem, Animation, Sound, Prefab, Sequence,
                            Terrain, Foliage, NavMesh, BehaviorTree, Retarget };

        void Commit()
        {
            uint8_t* Field = FieldForWrite();
            if(!Field || Entry.Size == 0) { return; }

            /* A material field holds a material graph or nothing: anything
             * else typed is refused, and the field shows what it holds. */
            if(AssetKind == EAsset::Material && !EditorContent::IsMaterialFieldValue(Edit->text().trimmed()))
            {
                QToolTip::showText(Edit->mapToGlobal(QPoint(0, Edit->height())),
                                   QStringLiteral("Not a material graph: give a *.material.nmgraph path"), Edit);
                Refresh(true);
                return;
            }
            if(AssetKind == EAsset::RenderTarget && !EditorContent::IsRenderTargetFieldValue(Edit->text().trimmed()))
            {
                QToolTip::showText(Edit->mapToGlobal(QPoint(0, Edit->height())),
                                   QStringLiteral("Not a render target: give an .nmrt2d or .nmrt3d path (or transient://Name)"), Edit);
                Refresh(true);
                return;
            }
            if(AssetKind == EAsset::PostProcess && !EditorContent::IsPostProcessFieldValue(Edit->text().trimmed()))
            {
                QToolTip::showText(Edit->mapToGlobal(QPoint(0, Edit->height())),
                                   QStringLiteral("Not a post process stack: give a *.pp.nmgraph (or a *.material.nmgraph) path"), Edit);
                Refresh(true);
                return;
            }

            /* Truncated to the buffer on a UTF-8 byte count, always
             * NUL-terminated -- the buffer is fixed, the string is not. */
            const QByteArray Utf8 = Edit->text().toUtf8();
            const size_t Length = std::min(size_t(Utf8.size()), size_t(Entry.Size) - 1);
            std::memset(Field, 0, Entry.Size);
            std::memcpy(Field, Utf8.constData(), Length);
            NotifyWritten();
        }

        void SetAndCommit(const QString& InValue)
        {
            {
                const QSignalBlocker Block(Edit);
                Edit->setText(InValue);
            }
            Commit();
        }

        /* What this field stores for a file, or empty if it is the wrong kind
         * or outside the project (a mesh's path must resolve at run time). */
        QString ValueForFile(const QString& InFile) const
        {
            if(InFile.isEmpty()) { return QString(); }
            if(AssetKind == EAsset::Mesh)
            {
                return InFile.endsWith(QStringLiteral(".nmmesh"), Qt::CaseInsensitive) ? EditorContent::ToRuntimePath(InFile) : QString();
            }
            if(AssetKind == EAsset::Material)
            {
                return EditorContent::MaterialPathFor(InFile);
            }
            if(AssetKind == EAsset::PostProcess)
            {
                return EditorContent::PostProcessPathFor(InFile);
            }
            if(AssetKind == EAsset::RenderTarget)
            {
                return EditorContent::RenderTargetPathFor(InFile);
            }
            if(AssetKind == EAsset::Texture)
            {
                return EditorContent::TexturePathFor(InFile);
            }
            if(AssetKind == EAsset::ParticleSystem)
            {
                return EditorContent::ParticleSystemPathFor(InFile);
            }
            if(AssetKind == EAsset::Animation)
            {
                return EditorContent::AnimationPathFor(InFile);
            }
            if(AssetKind == EAsset::Sound)
            {
                return EditorContent::SoundPathFor(InFile);
            }
            if(AssetKind == EAsset::Prefab)
            {
                return EditorContent::PrefabPathFor(InFile);
            }
            if(AssetKind == EAsset::Sequence)
            {
                return EditorContent::SequencePathFor(InFile);
            }
            if(AssetKind == EAsset::Terrain)
            {
                return EditorContent::TerrainPathFor(InFile);
            }
            if(AssetKind == EAsset::Foliage)
            {
                return EditorContent::FoliagePathFor(InFile);
            }
            if(AssetKind == EAsset::NavMesh)
            {
                return EditorContent::NavMeshPathFor(InFile);
            }
            if(AssetKind == EAsset::BehaviorTree)
            {
                return EditorContent::BehaviorTreePathFor(InFile);
            }
            if(AssetKind == EAsset::Retarget)
            {
                return EditorContent::RetargetPathFor(InFile);
            }
            return QString();
        }

        QLineEdit* Edit      = nullptr;
        EAsset     AssetKind = EAsset::None;
    };
}

void InstallScrub(QWidget* InHandle, QAbstractSpinBox* InSpin)
{
    if(!InHandle || !InSpin || InSpin->isReadOnly())
    {
        return;
    }
    InHandle->installEventFilter(new ScrubFilter(InSpin, InHandle));
    InHandle->setCursor(Qt::SizeHorCursor);
}

bool IsPropertyAtDefault(const GameCore::ComponentPropertyEntry& InEntry, const uint8_t* InField, const uint8_t* InDefault)
{
    auto SameVector = [](const EngineCore::Vector3& A, const EngineCore::Vector3& B)
    {
        return A.GetX() == B.GetX() && A.GetY() == B.GetY() && A.GetZ() == B.GetZ();
    };

    switch(InEntry.Type)
    {
        case EPropertyType::Vector3:
            return SameVector(*reinterpret_cast<const EngineCore::Vector3*>(InField),
                              *reinterpret_cast<const EngineCore::Vector3*>(InDefault));

        case EPropertyType::AABB:
        {
            const auto& A = *reinterpret_cast<const GameCore::AABB*>(InField);
            const auto& B = *reinterpret_cast<const GameCore::AABB*>(InDefault);
            return SameVector(A.Min, B.Min) && SameVector(A.Max, B.Max);
        }

        case EPropertyType::FixedString:
            return std::strncmp(reinterpret_cast<const char*>(InField), reinterpret_cast<const char*>(InDefault), InEntry.Size) == 0;

        default:
            return std::memcmp(InField, InDefault, InEntry.Size) == 0;
    }
}

PropertyEditor* CreatePropertyEditor(const GameCore::ComponentPropertyEntry& InEntry,
                                     PropertyEditor::ResolveFn InResolve, QWidget* InParent)
{
    if(InEntry.IsChoice() && (InEntry.Type == EPropertyType::UInt8 || InEntry.Type == EPropertyType::Int32))
    {
        return new ChoiceEditor(InEntry, std::move(InResolve), InParent);
    }

    switch(InEntry.Type)
    {
        case EPropertyType::Float:
        case EPropertyType::Double:      return new RealEditor  (InEntry, std::move(InResolve), InParent);
        case EPropertyType::Int32:
        case EPropertyType::UInt8:       return new IntEditor   (InEntry, std::move(InResolve), InParent);
        case EPropertyType::Bool:
        case EPropertyType::Flag:        return new BoolEditor  (InEntry, std::move(InResolve), InParent);
        case EPropertyType::Vector3:
        case EPropertyType::AABB:        return new VectorEditor(InEntry, std::move(InResolve), InParent);
        case EPropertyType::Color:       return new ColorEditor (InEntry, std::move(InResolve), InParent);
        case EPropertyType::FixedString: return new StringEditor(InEntry, std::move(InResolve), InParent);
    }
    return nullptr;
}

#endif // WITH_EDITOR
