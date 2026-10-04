#if WITH_EDITOR

#include "Private/Editor/PropertyWidgets.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstring>

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
#include "Private/Editor/Graph/GraphAssets.h"

using GameCore::EPropertyType;

// === PropertyEditor ==========================================================

PropertyEditor::PropertyEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
    : QWidget(InParent)
    , Entry(InEntry)
    , Resolve(std::move(InResolve))
{
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
    QDoubleSpinBox* MakeRealSpin(QWidget* InParent, const bool bInReadOnly)
    {
        auto* Spin = new QDoubleSpinBox(InParent);
        Spin->setRange(-1.0e9, 1.0e9);
        Spin->setDecimals(3);
        Spin->setSingleStep(1.0);
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

    /* A small "X" / "Y" / "Z" caption in front of a vector component. */
    QLabel* AxisLabel(const char* InText, QWidget* InParent)
    {
        auto* Label = new QLabel(QString::fromLatin1(InText), InParent);
        Label->setStyleSheet(QStringLiteral("color: palette(mid);"));
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
            Spin = MakeRealSpin(this, Entry.IsReadOnly());
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
            if(Entry.Type == EPropertyType::UInt8) { Spin->setRange(0, 255); }
            else                                  { Spin->setRange(INT_MIN, INT_MAX); }
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
                    Layout->addWidget(AxisLabel(Axes[Axis], this), Row, Column++);
                    QDoubleSpinBox* Spin = MakeRealSpin(this, Entry.IsReadOnly());
                    Spins[Row * 3 + Axis] = Spin;
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
            if((bMesh || bMaterial || bPost || bTarget) && !Entry.IsReadOnly())
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
                };
                const AssetUi& Ui = Kinds[bMesh ? 0 : bMaterial ? 1 : bPost ? 2 : 3];
                AssetKind = Ui.Kind;
                Edit->setPlaceholderText(QString::fromLatin1(Ui.Placeholder));
                Edit->installEventFilter(this);
                if(!bMesh)
                {
                    auto* Completer = new QCompleter(bMaterial ? EditorContent::ListMaterialPaths()
                                                     : bPost   ? EditorContent::ListPostProcessPaths()
                                                               : EditorContent::ListRenderTargetPaths(), Edit);
                    Completer->setCaseSensitivity(Qt::CaseInsensitive);
                    Completer->setFilterMode(Qt::MatchContains);
                    Edit->setCompleter(Completer);
                    Edit->setToolTip(QString::fromLatin1(Ui.Tip));
                }

                auto* Pick = new QToolButton(this);
                Pick->setText(QStringLiteral("..."));
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
        enum class EAsset { None, Mesh, Material, PostProcess, RenderTarget };

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
            return QString();
        }

        QLineEdit* Edit      = nullptr;
        EAsset     AssetKind = EAsset::None;
    };
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
