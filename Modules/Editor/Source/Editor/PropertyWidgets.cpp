#include "Editor/PropertyWidgets.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstring>

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

#include "CoreQtConversions.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Vector.h"

using GameCore::EPropertyType;

// === PropertyEditor ==========================================================

PropertyEditor::PropertyEditor(const GameCore::ComponentPropertyEntry& InEntry, ResolveFn InResolve, QWidget* InParent)
    : QWidget(InParent)
    , Entry(InEntry)
    , Resolve(std::move(InResolve))
{
}

void PropertyEditor::Refresh()
{
    if(IsBeingEdited())
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

    void SetSpin(QDoubleSpinBox* InSpin, const double InValue)
    {
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
            const QSignalBlocker Block(Spin);
            if(Entry.Type == EPropertyType::UInt8)
            {
                Spin->setValue(int(*InField));
            }
            else
            {
                int32_t Value; std::memcpy(&Value, InField, sizeof(int32_t));
                Spin->setValue(int(Value));
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
            const QSignalBlocker Block(Check);
            Check->setChecked(*InField != 0);
        }

    private:
        QCheckBox* Check = nullptr;
    };

    // --- Vectors -------------------------------------------------------------

    /*
     * Vector3 and AABB (two Vector3s). Always read and written through
     * GetX/SetX on the real Vector3 object, never its raw floats -- the layout
     * differs between SIMD and scalar builds (ComponentSerializer.h's
     * SerializeVector3 note).
     */
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
            connect(Edit, &QLineEdit::editingFinished, this, [this]()
            {
                uint8_t* Field = FieldForWrite();
                if(!Field || Entry.Size == 0) { return; }

                /* Truncated to the buffer on a UTF-8 byte count, always
                 * NUL-terminated -- the buffer is fixed, the string is not. */
                const QByteArray Utf8 = Edit->text().toUtf8();
                const size_t Length = std::min(size_t(Utf8.size()), size_t(Entry.Size) - 1);
                std::memset(Field, 0, Entry.Size);
                std::memcpy(Field, Utf8.constData(), Length);
                NotifyWritten();
            });
        }

    protected:
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
        QLineEdit* Edit = nullptr;
    };
}

PropertyEditor* CreatePropertyEditor(const GameCore::ComponentPropertyEntry& InEntry,
                                     PropertyEditor::ResolveFn InResolve, QWidget* InParent)
{
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
