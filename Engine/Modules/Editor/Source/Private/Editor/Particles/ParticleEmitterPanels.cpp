#if WITH_EDITOR

#include "Private/Editor/Particles/ParticleEmitterPanels.h"

#include <cmath>
#include <algorithm>
#include <cstring>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QListWidget>
#include <QMenu>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentProperties.h"
#include "Private/Editor/Particles/ParticleCurveWidgets.h"
#include "Private/Editor/Particles/ParticleDocument.h"
#include "Private/Editor/PropertyWidgets.h"

using RenderCore::ParticleEmitterDesc;

namespace
{
    /* Which emitter types a setting applies to (1 << EParticleType). */
    enum : uint8_t
    {
        FOR_SPRITE = 1 << RenderCore::PT_Sprite,
        FOR_MESH   = 1 << RenderCore::PT_Mesh,
        FOR_RIBBON = 1 << RenderCore::PT_Ribbon,
        FOR_BEAM   = 1 << RenderCore::PT_Beam,
        FOR_ALL    = FOR_SPRITE | FOR_MESH | FOR_RIBBON | FOR_BEAM,
        FOR_SPAWNS = FOR_SPRITE | FOR_MESH | FOR_RIBBON,   // everything that spawns particles
    };

    /* A row that is not a property widget: the over-life key editors. */
    enum class ECustom : uint8_t { None, ColorGradient, SizeCurve };

    /* One setting of ParticleEmitterDesc as a property entry, so the Details
     * panel's widgets edit it (or a custom editor); its tooltip; and when it
     * is shown: for which types, and only While its predicate holds. */
    struct EmitterSetting
    {
        GameCore::ComponentPropertyEntry Entry;
        const char*                      Tip    = nullptr;
        uint8_t                          Types  = FOR_ALL;
        bool                             (*While)(const ParticleEmitterDesc&) = nullptr;
        ECustom                          Custom = ECustom::None;
    };

    bool IsContinuous(const ParticleEmitterDesc& InEmitter) { return InEmitter.SpawnMode == RenderCore::PSM_Continuous; }
    bool IsBurst(const ParticleEmitterDesc& InEmitter)      { return InEmitter.SpawnMode == RenderCore::PSM_Burst; }
    bool IsBox(const ParticleEmitterDesc& InEmitter)        { return InEmitter.Shape == RenderCore::PS_Box; }
    bool IsRound(const ParticleEmitterDesc& InEmitter)      { return InEmitter.Shape != RenderCore::PS_Box; }
    bool IsCone(const ParticleEmitterDesc& InEmitter)       { return InEmitter.Shape == RenderCore::PS_Cone; }
    bool IsRing(const ParticleEmitterDesc& InEmitter)       { return InEmitter.Shape == RenderCore::PS_Ring; }
    bool IsColorStartEnd(const ParticleEmitterDesc& InEmitter) { return InEmitter.ColorMode != RenderCore::POL_Keys; }
    bool IsColorGradient(const ParticleEmitterDesc& InEmitter) { return InEmitter.ColorMode == RenderCore::POL_Keys; }
    bool IsSizeStartEnd(const ParticleEmitterDesc& InEmitter)  { return InEmitter.SizeMode != RenderCore::POL_Keys; }
    bool IsSizeCurve(const ParticleEmitterDesc& InEmitter)     { return InEmitter.SizeMode == RenderCore::POL_Keys; }
    bool IsBouncing(const ParticleEmitterDesc& InEmitter)      { return InEmitter.CollisionResponse == RenderCore::PCR_Bounce; }

    /* What decides which sections and rows show: rebuilt when it changes. */
    uint32_t ShapeOf(const ParticleEmitterDesc& InEmitter)
    {
        return uint32_t(InEmitter.Type) | uint32_t(InEmitter.SpawnMode) << 4 | uint32_t(InEmitter.Shape) << 8
             | uint32_t(InEmitter.ColorMode) << 12 | uint32_t(InEmitter.SizeMode) << 16 | uint32_t(InEmitter.Modules) << 20
             | uint32_t(InEmitter.CollisionResponse) << 28;
    }

    /* The sections, in the order shown: each a category of settings. One
     * with a module is optional -- added and removed as a component is;
     * the rest are always there (when their type has any of their settings). */
    struct SectionInfo
    {
        const char* Name;
        uint8_t     Module;   // EParticleModule; 0: always there
        const char* Tip;
    };

    const SectionInfo SECTIONS[] = {
        { "Rendering", 0, "What each particle is drawn as, and with" },
        { "Emission",  0, "When and how many are born, and how long they live" },
        { "Shape",     RenderCore::PEM_Shape,    "Where each is born: without it, at a point" },
        { "Velocity",  RenderCore::PEM_Velocity, "Each one's speed at birth: without it, they start still" },
        { "Forces",    RenderCore::PEM_Forces,   "Acceleration, drag and swirl as they live: without it, they coast" },
        { "Rotation",  RenderCore::PEM_Rotation, "Spin: without it, sprites and meshes keep their orientation" },
        { "Collision", RenderCore::PEM_Collision, "A ground plane to bounce off or die on: without it, they fall through" },
        { "Flipbook",  RenderCore::PEM_Flipbook, "The texture as a grid of frames played over life: without it, the whole texture" },
        { "Beam",      0, "Where a beam runs and how it jitters" },
        { "Size",      0, "Size over life" },
        { "Color",     0, "Colour and alpha over life" },
    };

    template<typename TField>
    EmitterSetting Setting(const char* InName, TField ParticleEmitterDesc::* InMember, const char* InCategory, const uint8_t InTypes,
                           const char* InTip, const uint8_t InFlags = GameCore::PF_None)
    {
        /* Measured on a default-constructed one, as ComponentPropertyBinder does. */
        static const ParticleEmitterDesc Probe{};
        EmitterSetting Out;
        std::strncpy(Out.Entry.Name, InName, sizeof(Out.Entry.Name) - 1);
        std::strncpy(Out.Entry.Category, InCategory, sizeof(Out.Entry.Category) - 1);
        Out.Entry.Type   = GameCore::ScriptFieldTypeOf<TField>::Value;
        Out.Entry.Flags  = InFlags;
        Out.Entry.Offset = uint32_t(reinterpret_cast<const char*>(&(Probe.*InMember)) - reinterpret_cast<const char*>(&Probe));
        Out.Entry.Size   = uint32_t(sizeof(TField));
        Out.Tip          = InTip;
        Out.Types        = InTypes;
        return Out;
    }

    EmitterSetting Switch(const char* InName, uint8_t ParticleEmitterDesc::* InMember, const char* InCategory, const uint8_t InTypes,
                          const char* InTip)
    {
        EmitterSetting Out = Setting(InName, InMember, InCategory, InTypes, InTip);
        Out.Entry.Type = GameCore::EPropertyType::Flag;
        return Out;
    }

    /* A dropdown: InLabels name its values 0, 1, 2..., '|'-separated. */
    EmitterSetting Choice(const char* InName, uint8_t ParticleEmitterDesc::* InMember, const char* InCategory, const uint8_t InTypes,
                          const char* InLabels, const char* InTip)
    {
        EmitterSetting Out = Setting(InName, InMember, InCategory, InTypes, InTip);
        std::strncpy(Out.Entry.Choices, InLabels, sizeof(Out.Entry.Choices) - 1);
        return Out;
    }

    EmitterSetting Only(EmitterSetting InSetting, bool (*InWhile)(const ParticleEmitterDesc&))
    {
        InSetting.While = InWhile;
        return InSetting;
    }

    /* A key editor's row, named as a setting is, covering its keys and their
     * count (adjacent in the emitter) so Reset and "changed" see them. */
    template<typename TKey>
    EmitterSetting KeyEditor(const char* InName, const char* InCategory, const ECustom InCustom,
                             TKey (ParticleEmitterDesc::* InKeys)[RenderCore::PARTICLE_CURVE_KEYS_MAX],
                             int32_t ParticleEmitterDesc::* InCount, const char* InTip)
    {
        static const ParticleEmitterDesc Probe{};
        const char* Base  = reinterpret_cast<const char*>(&Probe);
        const char* Keys  = reinterpret_cast<const char*>(&(Probe.*InKeys));
        const char* Count = reinterpret_cast<const char*>(&(Probe.*InCount));
        EmitterSetting Out;
        std::strncpy(Out.Entry.Name, InName, sizeof(Out.Entry.Name) - 1);
        std::strncpy(Out.Entry.Category, InCategory, sizeof(Out.Entry.Category) - 1);
        Out.Entry.Type   = GameCore::EPropertyType::Struct;
        Out.Entry.Offset = uint32_t(Keys - Base);
        Out.Entry.Size   = uint32_t(Count + sizeof(int32_t) - Keys);
        Out.Tip    = InTip;
        Out.Custom = InCustom;
        return Out;
    }

    const std::vector<EmitterSetting>& GetSettings()
    {
        using namespace RenderCore;
        static const std::vector<EmitterSetting> Settings = {
            Choice("Type", &ParticleEmitterDesc::Type, "Rendering", FOR_ALL, "Sprite|Mesh|Ribbon|Beam",
                    "What its particles are drawn as. Sprite: a quad each, turned to the view as Billboard says. Mesh: a mesh each, "
                    "turned as Mesh Orientation says. Ribbon: one strip through its particles in the order they were born, a trail. "
                    "Beam: one strip from the emitter to Beam End"),
            Setting("Mesh",     &ParticleEmitterDesc::Mesh, "Rendering", FOR_SPRITE | FOR_MESH,
                    "What each particle draws as: empty is a 1 x 1 quad for a Sprite, a 1-unit box for a Mesh", GameCore::PF_AssetMesh),
            Setting("Material", &ParticleEmitterDesc::Material, "Rendering", FOR_ALL,
                    "Its material; the particle's colour reaches it as Vertex Color. A ribbon's or beam's U runs across it, "
                    "V along each segment. Empty is DefaultLit", GameCore::PF_AssetMaterial),
            Setting("Offset",   &ParticleEmitterDesc::Offset, "Rendering", FOR_ALL,
                    "Where it emits from, relative to the entity, in the entity's own axes"),
            Choice ("Billboard", &ParticleEmitterDesc::Billboard, "Rendering", FOR_SPRITE,
                    "Face Camera|Vertical|Velocity Aligned|Fixed",
                    "How each sprite is turned. Face Camera: square to the view. Vertical: upright, turned about world up "
                    "toward the eye (flames, foliage). Velocity Aligned: along its motion, stretched by speed (sparks, rain). "
                    "Fixed: the quad's own axes"),
            Setting("Velocity Stretch", &ParticleEmitterDesc::VelocityStretch, "Rendering", FOR_SPRITE,
                    "Velocity Aligned: seconds of travel added to its length -- 0.05 at 400 units a second adds 20"),
            Choice ("Mesh Orientation", &ParticleEmitterDesc::MeshOrientation, "Rendering", FOR_MESH,
                    "Fixed|Random Spin|Align to Velocity",
                    "How each mesh is turned. Fixed: its own axes, spinning about +Y at Rotation Rate. Random Spin: about an "
                    "axis of its own (debris). Align to Velocity: its +Z along its motion (arrows)"),

            Choice("Spawn Mode", &ParticleEmitterDesc::SpawnMode, "Emission", FOR_SPAWNS, "Continuous|Burst",
                    "Continuous: Spawn Rate a second while it emits. Burst: Burst Count at once, then again every Burst "
                    "Interval. Switching Emitting off and on starts the bursts over"),
            Setting("Max Particles",     &ParticleEmitterDesc::MaxParticles, "Emission", FOR_SPAWNS,
                    "How many it holds at once (1 to 2048); past that the oldest are replaced"),
            Only(Setting("Spawn Rate", &ParticleEmitterDesc::SpawnRate, "Emission", FOR_SPAWNS, "Particles born a second"),
                 &IsContinuous),
            Only(Setting("Burst Count", &ParticleEmitterDesc::BurstCount, "Emission", FOR_SPAWNS,
                         "Particles born at once in each burst"), &IsBurst),
            Only(Setting("Burst Interval", &ParticleEmitterDesc::BurstInterval, "Emission", FOR_SPAWNS,
                         "Seconds from one burst to the next; 0: a single burst"), &IsBurst),
            Only(Setting("Burst Cycles", &ParticleEmitterDesc::BurstCycles, "Emission", FOR_SPAWNS,
                         "How many bursts, with an interval; 0: no end"), &IsBurst),
            Setting("Lifetime",          &ParticleEmitterDesc::Lifetime, "Emission", FOR_SPAWNS, "How long each lives, in seconds"),
            Setting("Lifetime Variance", &ParticleEmitterDesc::LifetimeVariance, "Emission", FOR_SPAWNS,
                    "0 to 1: each lives Lifetime less up to this much of it, at random (keep a ribbon's at 0: it breaks where one dies early)"),
            Setting("Seed",              &ParticleEmitterDesc::Seed, "Emission", FOR_ALL, "Changes its random numbers (a beam's jitter)"),
            Setting("Cull Distance",     &ParticleEmitterDesc::CullDistance, "Emission", FOR_ALL,
                    "Beyond this distance from the view it is neither simulated nor drawn, and its particles are let go; 0 never"),
            Switch ("Pause Offscreen",   &ParticleEmitterDesc::bPauseOffscreen, "Emission", FOR_ALL,
                    "Stop simulating once no view has drawn it for half a second, and go on where it left off when one does"),

            Choice ("Shape", &ParticleEmitterDesc::Shape, "Shape", FOR_SPAWNS, "Sphere|Hemisphere|Box|Cone|Ring",
                    "Where each is born, in the entity's own axes (it turns with the entity). Sphere and Hemisphere (its +Y half): "
                    "within Radius. Box: within Box Size. Cone: on a disc of Radius facing +Y, sent out within Cone Angle. "
                    "Ring: on a circle of Radius about +Y"),
            Only(Setting("Radius", &ParticleEmitterDesc::EmitterRadius, "Shape", FOR_SPAWNS,
                         "The sphere's, the cone's disc's or the ring's radius; 0 for a point"), &IsRound),
            Only(Setting("Box Size", &ParticleEmitterDesc::BoxSize, "Shape", FOR_SPAWNS, "The box's size along each of its axes"), &IsBox),
            Only(Setting("Cone Angle", &ParticleEmitterDesc::ConeAngle, "Shape", FOR_SPAWNS,
                         "Degrees from +Y each is sent out within (with Outward Speed): 0 a jet, 90 a fan"), &IsCone),
            Only(Setting("Ring Width", &ParticleEmitterDesc::RingWidth, "Shape", FOR_SPAWNS,
                         "How wide a band about the circle each is born in; 0 on the line"), &IsRing),
            Switch ("On Surface", &ParticleEmitterDesc::bShapeSurface, "Shape", FOR_SPAWNS,
                    "Born on the shape's surface only: a sphere's shell, a box's faces, a cone's rim, the ring's circle"),
            Setting("Outward Speed", &ParticleEmitterDesc::ShapeSpeed, "Shape", FOR_SPAWNS,
                    "Plus this speed out of the shape where each is born (from the centre, through the face, along the cone); "
                    "negative pulls in"),

            Setting("Velocity",        &ParticleEmitterDesc::Velocity, "Velocity", FOR_SPAWNS,
                    "Each one's speed and direction at birth, in world units a second"),
            Setting("Velocity Spread", &ParticleEmitterDesc::VelocitySpread, "Velocity", FOR_SPAWNS, "Plus up to this speed in a random direction"),

            Setting("Acceleration",    &ParticleEmitterDesc::Acceleration, "Forces", FOR_SPAWNS,
                    "Buoyancy up, gravity down: world units a second, a second"),
            Setting("Drag",            &ParticleEmitterDesc::Drag, "Forces", FOR_SPAWNS, "Velocity lost a second (1/s): smoke slows, sparks do not"),
            Setting("Turbulence",      &ParticleEmitterDesc::Turbulence, "Forces", FOR_SPAWNS,
                    "A swirl's acceleration, pushing each along a moving noise"),

            Switch ("Random Rotation", &ParticleEmitterDesc::bRandomRotation, "Rotation", FOR_SPRITE | FOR_MESH,
                    "Each starts at a random angle and spins either way"),
            Setting("Rotation Rate", &ParticleEmitterDesc::RotationRate, "Rotation", FOR_SPRITE | FOR_MESH,
                    "Degrees a second: a sprite in its own plane, a mesh about its axis"),

            Choice ("Response", &ParticleEmitterDesc::CollisionResponse, "Collision", FOR_SPAWNS, "None|Bounce|Kill",
                    "What a particle does when it reaches the plane from above: bounce off it, or die there"),
            Setting("Height", &ParticleEmitterDesc::CollisionHeight, "Collision", FOR_SPAWNS,
                    "The plane's height above the entity (world up); 0 at the entity"),
            Only(Setting("Bounce", &ParticleEmitterDesc::Bounce, "Collision", FOR_SPAWNS,
                         "0 to 1: the share of its speed into the plane it keeps, back out; 0 stops it on the plane"), &IsBouncing),
            Only(Setting("Friction", &ParticleEmitterDesc::Friction, "Collision", FOR_SPAWNS,
                         "0 to 1: the share of its speed along the plane it loses each bounce"), &IsBouncing),

            Setting("Columns", &ParticleEmitterDesc::FlipbookColumns, "Flipbook", FOR_SPRITE | FOR_MESH,
                    "Frames across the texture (1 to 64)"),
            Setting("Rows", &ParticleEmitterDesc::FlipbookRows, "Flipbook", FOR_SPRITE | FOR_MESH,
                    "Frames down the texture (1 to 64); frames run left to right, then top to bottom"),
            Setting("Frames", &ParticleEmitterDesc::FlipbookFrames, "Flipbook", FOR_SPRITE | FOR_MESH,
                    "How many frames it plays, from the first; 0 every one"),
            Setting("Frame Rate", &ParticleEmitterDesc::FlipbookRate, "Flipbook", FOR_SPRITE | FOR_MESH,
                    "Frames a second, looping; 0 plays them over its life instead (Cycles times)"),
            Setting("Cycles", &ParticleEmitterDesc::FlipbookCycles, "Flipbook", FOR_SPRITE | FOR_MESH,
                    "With Frame Rate 0: how many times it plays the frames over its life"),
            Switch ("Random Start", &ParticleEmitterDesc::bFlipbookRandomStart, "Flipbook", FOR_SPRITE | FOR_MESH,
                    "Each starts on a frame of its own, so they are not in step"),

            Setting("Beam End",         &ParticleEmitterDesc::BeamEnd, "Beam", FOR_BEAM,
                    "Where the beam ends, relative to the emitter in the entity's own axes (a component's Beam Target overrides it)"),
            Setting("Beam Segments",    &ParticleEmitterDesc::BeamSegments, "Beam", FOR_BEAM,
                    "How many pieces it is drawn in (1 to 128): more bends more smoothly"),
            Setting("Beam Noise",       &ParticleEmitterDesc::BeamNoise, "Beam", FOR_BEAM,
                    "How far its middle strays from the straight line, in world units; its ends stay put"),
            Setting("Beam Noise Speed", &ParticleEmitterDesc::BeamNoiseSpeed, "Beam", FOR_BEAM, "How fast the jitter changes"),

            Choice ("Size Mode",   &ParticleEmitterDesc::SizeMode, "Size", FOR_ALL, "Start/End|Curve",
                    "Start/End: straight from Start Size to End Size. Curve: through keys you place (a beam's from the emitter to its end)"),
            Only(Setting("Start Size",  &ParticleEmitterDesc::StartSize, "Size", FOR_ALL,
                    "Its size at birth, in world units across (a ribbon's width at its newest point, a beam's at the emitter)"), &IsSizeStartEnd),
            Only(Setting("End Size",    &ParticleEmitterDesc::EndSize, "Size", FOR_ALL, "Its size at death (a beam's at its end)"),
                 &IsSizeStartEnd),
            Only(KeyEditor("Size Curve", "Size", ECustom::SizeCurve, &ParticleEmitterDesc::SizeKeys, &ParticleEmitterDesc::SizeKeyCount,
                           "Its size over life, through up to 8 keys"), &IsSizeCurve),
            Choice ("Color Mode",  &ParticleEmitterDesc::ColorMode, "Color", FOR_ALL, "Start/End|Gradient",
                    "Start/End: Start Color to End Color by Color Curve, faded in and out. Gradient: colour and alpha through keys "
                    "(starting from what Start/End gave)"),
            Only(Setting("Start Color", &ParticleEmitterDesc::StartColor, "Color", FOR_ALL,
                         "Its colour and alpha at birth (a beam's at the emitter)"), &IsColorStartEnd),
            Only(Setting("End Color",   &ParticleEmitterDesc::EndColor, "Color", FOR_ALL,
                         "Its colour and alpha at death (a beam's at its end)"), &IsColorStartEnd),
            Only(Setting("Color Curve", &ParticleEmitterDesc::ColorCurve, "Color", FOR_ALL,
                         "The colour follows age^Curve: under 1 it reaches End Color sooner, over 1 later"), &IsColorStartEnd),
            Only(Setting("Fade In",     &ParticleEmitterDesc::FadeIn, "Color", FOR_ALL,
                         "0 to 1: the share of its life over which alpha rises from 0"), &IsColorStartEnd),
            Only(Setting("Fade Out",    &ParticleEmitterDesc::FadeOut, "Color", FOR_ALL,
                         "0 to 1: the share of its life over which alpha falls to 0"), &IsColorStartEnd),
            Only(KeyEditor("Color Gradient", "Color", ECustom::ColorGradient, &ParticleEmitterDesc::ColorKeys,
                           &ParticleEmitterDesc::ColorKeyCount, "Its colour and alpha over life, through up to 8 keys"), &IsColorGradient),
        };
        return Settings;
    }

    /* Whether InSetting applies to InEmitter, as its type, modes and shape are. */
    bool Applies(const EmitterSetting& InSetting, const ParticleEmitterDesc& InEmitter)
    {
        const uint8_t Type = InEmitter.Type < RenderCore::PT_Count ? InEmitter.Type : uint8_t(RenderCore::PT_Sprite);
        return (InSetting.Types & (1u << Type)) != 0 && (!InSetting.While || InSetting.While(InEmitter));
    }

    /* Every setting of InCategory back to a fresh emitter's: a section's Reset, a module's removal. */
    void ResetCategory(ParticleEmitterDesc& InOut, const char* InCategory)
    {
        static const ParticleEmitterDesc Defaults{};
        for(const EmitterSetting& Each : GetSettings())
        {
            if(std::strcmp(Each.Entry.Category, InCategory) != 0) { continue; }
            std::memcpy(reinterpret_cast<uint8_t*>(&InOut) + Each.Entry.Offset,
                        reinterpret_cast<const uint8_t*>(&Defaults) + Each.Entry.Offset, Each.Entry.Size);
        }
    }

    /* Whether any setting of InCategory belongs to InEmitter's type: what the Add Module menu offers. */
    bool ModuleApplies(const char* InCategory, const ParticleEmitterDesc& InEmitter)
    {
        const uint8_t Type = InEmitter.Type < RenderCore::PT_Count ? InEmitter.Type : uint8_t(RenderCore::PT_Sprite);
        for(const EmitterSetting& Each : GetSettings())
        {
            if(std::strcmp(Each.Entry.Category, InCategory) == 0 && (Each.Types & (1u << Type)) != 0) { return true; }
        }
        return false;
    }

    QToolButton* MakeButton(QWidget* InParent, const QString& InText, const QString& InTip)
    {
        auto* Button = new QToolButton(InParent);
        Button->setText(InText);
        Button->setToolTip(InTip);
        Button->setAutoRaise(true);
        return Button;
    }

    /* The reset arrow, as the Details panel draws it. */
    QToolButton* MakeResetButton(QWidget* InParent, const QString& InTip)
    {
        auto* Button = new QToolButton(InParent);
        Button->setText(QString(QChar(0x21BA)));
        Button->setToolTip(InTip);
        Button->setAutoRaise(true);
        Button->setFixedSize(22, 22);
        Button->setStyleSheet(QStringLiteral("QToolButton { color: #ffa51e; font-weight: bold; }"));
        QSizePolicy Policy = Button->sizePolicy();
        Policy.setRetainSizeWhenHidden(true);
        Button->setSizePolicy(Policy);
        Button->setVisible(false);
        return Button;
    }
}

// === Emitters ======================================================================

ParticleEmitterListPanel::ParticleEmitterListPanel(ParticleDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Document(InDocument)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);

    List = new QListWidget(this);
    List->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    List->setContextMenuPolicy(Qt::CustomContextMenu);
    List->setToolTip(QStringLiteral("Each emitter runs on its own and is drawn with its own mesh and material. "
                                    "The check box turns one off; F2 or a double-click renames it."));
    Layout->addWidget(List, 1);

    auto* Buttons = new QHBoxLayout();
    Buttons->setSpacing(2);
    AddButton  = MakeButton(this, QStringLiteral("Add"), QStringLiteral("Add an emitter at its defaults"));
    DupButton  = MakeButton(this, QStringLiteral("Duplicate"), QStringLiteral("Copy the selected emitter, seeded apart"));
    DelButton  = MakeButton(this, QStringLiteral("Remove"), QStringLiteral("Remove the selected emitter"));
    UpButton   = MakeButton(this, QString(QChar(0x2191)), QStringLiteral("Move up: drawn earlier"));
    DownButton = MakeButton(this, QString(QChar(0x2193)), QStringLiteral("Move down: drawn later"));
    for(QToolButton* Each : { AddButton, DupButton, DelButton, UpButton, DownButton }) { Buttons->addWidget(Each); }
    Buttons->addStretch(1);
    Layout->addLayout(Buttons);

    connect(AddButton,  &QToolButton::clicked, this, [this]() { Document.AddEmitter(); });
    connect(DupButton,  &QToolButton::clicked, this, [this]() { Document.DuplicateEmitter(Document.GetSelected()); });
    connect(DelButton,  &QToolButton::clicked, this, [this]() { Document.RemoveEmitter(Document.GetSelected()); });
    connect(UpButton,   &QToolButton::clicked, this, [this]() { Document.MoveEmitter(Document.GetSelected(), -1); });
    connect(DownButton, &QToolButton::clicked, this, [this]() { Document.MoveEmitter(Document.GetSelected(), 1); });

    connect(List, &QListWidget::currentRowChanged, this, [this](const int InRow)
    {
        if(!bRebuilding && InRow >= 0) { Document.SetSelected(InRow); }
    });
    connect(List, &QListWidget::itemChanged, this, [this](QListWidgetItem* InItem)
    {
        if(bRebuilding) { return; }
        const int Row = List->row(InItem);
        const ParticleEmitterDesc* Emitter = Document.GetEmitter(Row);
        if(!Emitter) { return; }
        const bool bEnabled = InItem->checkState() == Qt::Checked;
        if(bEnabled != (Emitter->bEnabled != 0))
        {
            Document.SetEmitterEnabled(Row, bEnabled);
        }
        else
        {
            Document.RenameEmitter(Row, InItem->text());
            if(InItem->text() != QString::fromUtf8(Emitter->Name)) { Rebuild(); }   // refused: show the name it has
        }
    });
    connect(List, &QListWidget::customContextMenuRequested, this, [this](const QPoint& InAt)
    {
        QMenu Menu(this);
        Menu.addAction(QStringLiteral("Add Emitter"), this, [this]() { Document.AddEmitter(); })
            ->setEnabled(AddButton->isEnabled());
        Menu.addAction(QStringLiteral("Duplicate"), this, [this]() { Document.DuplicateEmitter(Document.GetSelected()); })
            ->setEnabled(DupButton->isEnabled());
        Menu.addAction(QStringLiteral("Rename"), this, [this]()
        {
            if(QListWidgetItem* Item = List->currentItem()) { List->editItem(Item); }
        })->setEnabled(List->currentItem() != nullptr);
        Menu.addAction(QStringLiteral("Remove"), this, [this]() { Document.RemoveEmitter(Document.GetSelected()); })
            ->setEnabled(DelButton->isEnabled());
        Menu.exec(List->viewport()->mapToGlobal(InAt));
    });

    connect(&Document, &ParticleDocument::EmittersChanged, this, &ParticleEmitterListPanel::Rebuild);
    connect(&Document, &ParticleDocument::SelectionChanged, this, [this](const int InIndex)
    {
        const QSignalBlocker Block(List);
        List->setCurrentRow(InIndex);
        UpdateButtons();
    });
    Rebuild();
}

void ParticleEmitterListPanel::Rebuild()
{
    bRebuilding = true;
    List->clear();
    for(int Index = 0; Index < Document.GetEmitterCount(); ++Index)
    {
        const ParticleEmitterDesc& Emitter = *Document.GetEmitter(Index);
        auto* Item = new QListWidgetItem(QString::fromUtf8(Emitter.Name), List);
        Item->setFlags(Item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        Item->setCheckState(Emitter.bEnabled ? Qt::Checked : Qt::Unchecked);
    }
    List->setCurrentRow(Document.GetSelected());
    bRebuilding = false;
    UpdateButtons();
}

void ParticleEmitterListPanel::UpdateButtons()
{
    const int  Selected = Document.GetSelected();
    const int  Count    = Document.GetEmitterCount();
    const bool bRoom    = Count < int(RenderCore::PARTICLE_SYSTEM_EMITTERS_MAX);
    AddButton->setEnabled(bRoom);
    DupButton->setEnabled(bRoom && Selected >= 0);
    DelButton->setEnabled(Selected >= 0);
    UpButton->setEnabled(Selected > 0);
    DownButton->setEnabled(Selected >= 0 && Selected + 1 < Count);
}

// === Details =======================================================================

ParticleEmitterDetailsPanel::ParticleEmitterDetailsPanel(ParticleDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Document(InDocument)
{
    auto* Outer = new QVBoxLayout(this);
    Outer->setContentsMargins(0, 0, 0, 0);

    auto* Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    Outer->addWidget(Scroll, 1);

    Content = new QWidget(Scroll);
    ContentLayout = new QVBoxLayout(Content);
    ContentLayout->setContentsMargins(6, 6, 6, 6);
    Scroll->setWidget(Content);

    Budget = new QLabel(this);
    Budget->setWordWrap(true);
    Budget->setContentsMargins(8, 4, 8, 6);
    Outer->addWidget(Budget);

    connect(&Document, &ParticleDocument::SelectionChanged, this, &ParticleEmitterDetailsPanel::Rebuild);
    connect(&Document, &ParticleDocument::Changed, this, &ParticleEmitterDetailsPanel::Refresh);
    Rebuild();
}

void ParticleEmitterDetailsPanel::Rebuild()
{
    Rows.clear();
    Sections.clear();
    Gradient = nullptr;
    Curve    = nullptr;
    while(QLayoutItem* Item = ContentLayout->takeAt(0))
    {
        delete Item->widget();
        delete Item;
    }

    const int Selected = Document.GetSelected();
    if(!Document.GetEmitter(Selected))
    {
        auto* None = new QLabel(Document.GetEmitterCount() == 0 ? QStringLiteral("No emitters: Add one in the Emitters list")
                                                                : QStringLiteral("Select an emitter"), Content);
        None->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        ContentLayout->addWidget(None);
        ContentLayout->addStretch(1);
        Refresh();
        return;
    }

    /* Looked up fresh on every read and write: the selected emitter's bytes. */
    ParticleDocument* Doc = &Document;
    const auto Resolve = [Doc, Selected]() -> uint8_t* { return reinterpret_cast<uint8_t*>(Doc->GetEmitter(Selected)); };

    const ParticleEmitterDesc& Shown = *Document.GetEmitter(Selected);
    ShownShape = ShapeOf(Shown);

    /* One property row: the editor and its reset arrow. */
    const std::vector<EmitterSetting>& Settings = GetSettings();
    const auto AddRow = [&](const size_t InIndex, QFormLayout* InForm, QWidget* InParent)
    {
        const EmitterSetting& Setting = Settings[InIndex];
        auto* Field    = new QWidget(InParent);
        auto* FieldRow = new QHBoxLayout(Field);
        FieldRow->setContentsMargins(0, 0, 0, 0);
        FieldRow->setSpacing(2);

        PropertyEditor* Editor = CreatePropertyEditor(Setting.Entry, Resolve, Field);
        if(!Editor)
        {
            delete Field;
            return;
        }
        FieldRow->addWidget(Editor, 1);
        const QString Name = QString::fromLatin1(Setting.Entry.Name);
        QToolButton* Reset = MakeResetButton(Field, QStringLiteral("Changed from the default -- click to reset %1").arg(Name));
        FieldRow->addWidget(Reset);

        auto* Label = new QLabel(Name, InParent);
        Label->setToolTip(QString::fromLatin1(Setting.Tip));
        Editor->setToolTip(QString::fromLatin1(Setting.Tip));
        InForm->addRow(Label, Field);

        connect(Editor, &PropertyEditor::Written, this, [this, Name]()
        {
            /* A gradient or curve chosen for the first time starts as what Start/End gave. */
            if(ParticleEmitterDesc* Edited = Document.GetEmitter(Document.GetSelected()))
            {
                if(Name == QStringLiteral("Color Mode") && Edited->ColorMode == RenderCore::POL_Keys && Edited->ColorKeyCount == 0)
                {
                    Edited->SeedColorKeys();
                }
                if(Name == QStringLiteral("Size Mode") && Edited->SizeMode == RenderCore::POL_Keys && Edited->SizeKeyCount == 0)
                {
                    Edited->SeedSizeKeys();
                }
            }
            Document.CommitLiveEdit(QStringLiteral("Edit %1").arg(Name));
        });
        connect(Reset, &QToolButton::clicked, this, [this, InIndex, Name]()
        {
            const GameCore::ComponentPropertyEntry& Entry = GetSettings()[InIndex].Entry;
            const int At = Document.GetSelected();
            static const ParticleEmitterDesc Defaults{};
            Document.Edit(QStringLiteral("Reset %1").arg(Name), [&Entry, At](RenderCore::ParticleSystemDesc& InOut)
            {
                std::memcpy(reinterpret_cast<uint8_t*>(&InOut.Emitters[size_t(At)]) + Entry.Offset,
                            reinterpret_cast<const uint8_t*>(&Defaults) + Entry.Offset, Entry.Size);
            });
        });
        Rows.push_back({ Editor, Reset, InIndex });
    };

    /* A section a category, in SECTIONS' order: an optional one only once
     * added, any only if its type has a setting in it. */
    for(const SectionInfo& Info : SECTIONS)
    {
        if(Info.Module != 0 && !Shown.HasModule(Info.Module)) { continue; }
        std::vector<size_t> Indices;
        for(size_t Index = 0; Index < Settings.size(); ++Index)
        {
            if(std::strcmp(Settings[Index].Entry.Category, Info.Name) == 0 && Applies(Settings[Index], Shown)) { Indices.push_back(Index); }
        }
        if(Indices.empty()) { continue; }

        QWidget*     Body = nullptr;
        QFormLayout* Form = AddSection(Info.Name, Info.Module, QString::fromLatin1(Info.Tip), Body);
        for(const size_t Index : Indices)
        {
            const EmitterSetting& Setting = Settings[Index];
            if(Setting.Custom != ECustom::None)
            {
                AddKeyEditor(Setting.Custom == ECustom::ColorGradient, QString::fromLatin1(Setting.Entry.Name),
                             QString::fromLatin1(Setting.Tip), Form);
                continue;
            }
            AddRow(Index, Form, Body);
        }
    }

    ContentLayout->addWidget(BuildAddModuleButton());
    ContentLayout->addStretch(1);
    Refresh();
}

QFormLayout* ParticleEmitterDetailsPanel::AddSection(const char* InName, const uint8_t InModule, const QString& InTip, QWidget*& OutBody)
{
    const QString Name = QString::fromLatin1(InName);
    auto* Section = new QFrame(Content);
    Section->setFrameShape(QFrame::StyledPanel);
    auto* SectionLayout = new QVBoxLayout(Section);
    SectionLayout->setContentsMargins(4, 2, 4, 4);
    SectionLayout->setSpacing(2);

    // --- Header: collapse toggle, Reset, Remove (an optional module's) ------
    auto* HeaderRow = new QHBoxLayout();
    HeaderRow->setContentsMargins(0, 0, 0, 0);
    const bool bOpen = !CollapsedSections.contains(Name);
    auto* Toggle = new QToolButton(Section);
    Toggle->setText(Name);
    Toggle->setToolTip(InTip);
    Toggle->setCheckable(true);
    Toggle->setChecked(bOpen);
    Toggle->setArrowType(bOpen ? Qt::DownArrow : Qt::RightArrow);
    Toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    Toggle->setAutoRaise(true);
    Toggle->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; }"));
    HeaderRow->addWidget(Toggle);
    HeaderRow->addStretch(1);

    auto* ResetAll = new QToolButton(Section);
    ResetAll->setText(QStringLiteral("Reset"));
    ResetAll->setToolTip(QStringLiteral("Reset every setting of %1 to its default").arg(Name));
    ResetAll->setAutoRaise(true);
    ResetAll->setEnabled(false);
    connect(ResetAll, &QToolButton::clicked, this, [this, Name]()
    {
        const QByteArray Category = Name.toLatin1();
        const int At = Document.GetSelected();
        Document.Edit(QStringLiteral("Reset %1").arg(Name), [Category, At](RenderCore::ParticleSystemDesc& InOut)
        {
            ResetCategory(InOut.Emitters[size_t(At)], Category.constData());
        });
    });
    HeaderRow->addWidget(ResetAll);

    if(InModule != 0)
    {
        auto* Remove = new QToolButton(Section);
        Remove->setText(QString(QChar(0x2715)));   // multiplication x, as the Details panel's
        Remove->setToolTip(QStringLiteral("Remove %1 from this emitter: its settings go back to their defaults and have no effect").arg(Name));
        Remove->setAutoRaise(true);
        connect(Remove, &QToolButton::clicked, this, [this, Name, InModule]()
        {
            const QByteArray Category = Name.toLatin1();
            const int At = Document.GetSelected();
            Document.Edit(QStringLiteral("Remove %1").arg(Name), [Category, At, InModule](RenderCore::ParticleSystemDesc& InOut)
            {
                ParticleEmitterDesc& Emitter = InOut.Emitters[size_t(At)];
                ResetCategory(Emitter, Category.constData());
                Emitter.Modules = uint8_t(Emitter.Modules & ~InModule);
            });
        });
        HeaderRow->addWidget(Remove);
    }
    SectionLayout->addLayout(HeaderRow);

    // --- Its settings ----------------------------------------------------------
    auto* Body = new QWidget(Section);
    auto* Form = new QFormLayout(Body);
    Form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    Form->setContentsMargins(14, 0, 0, 0);
    Body->setVisible(bOpen);
    SectionLayout->addWidget(Body);

    connect(Toggle, &QToolButton::toggled, this, [this, Toggle, Body, Name](const bool bInOpen)
    {
        Toggle->setArrowType(bInOpen ? Qt::DownArrow : Qt::RightArrow);
        Body->setVisible(bInOpen);
        if(bInOpen) { CollapsedSections.remove(Name); }
        else        { CollapsedSections.insert(Name); }
    });

    ContentLayout->addWidget(Section);
    Sections.push_back({ Name, ResetAll });
    OutBody = Body;
    return Form;
}

QWidget* ParticleEmitterDetailsPanel::BuildAddModuleButton()
{
    auto* Button = new QToolButton(Content);
    Button->setText(QStringLiteral("+ Add Module"));
    Button->setToolTip(QStringLiteral("Add an optional module -- Shape, Velocity, Forces, Rotation, Collision, Flipbook -- that this emitter's type uses"));
    Button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    Button->setPopupMode(QToolButton::InstantPopup);
    Button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    Button->setMinimumHeight(28);

    /* Filled when opened: the emitter may change between this rebuild and the click. */
    auto* Menu = new QMenu(Button);
    connect(Menu, &QMenu::aboutToShow, this, [this, Menu]()
    {
        Menu->clear();
        const int At = Document.GetSelected();
        const ParticleEmitterDesc* Emitter = Document.GetEmitter(At);
        if(Emitter)
        {
            for(const SectionInfo& Info : SECTIONS)
            {
                if(Info.Module == 0 || Emitter->HasModule(Info.Module) || !ModuleApplies(Info.Name, *Emitter)) { continue; }
                const QString Name = QString::fromLatin1(Info.Name);
                const uint8_t Module = Info.Module;
                QAction* Action = Menu->addAction(Name);
                Action->setToolTip(QString::fromLatin1(Info.Tip));
                connect(Action, &QAction::triggered, this, [this, Name, Module, At]()
                {
                    CollapsedSections.remove(Name);
                    Document.Edit(QStringLiteral("Add %1").arg(Name), [At, Module](RenderCore::ParticleSystemDesc& InOut)
                    {
                        InOut.Emitters[size_t(At)].Modules |= Module;
                    });
                });
            }
        }
        if(Menu->isEmpty())
        {
            Menu->addAction(QStringLiteral("(every module this emitter's type uses is present)"))->setEnabled(false);
        }
    });
    Button->setMenu(Menu);
    return Button;
}

void ParticleEmitterDetailsPanel::AddKeyEditor(const bool bInGradient, const QString& InName, const QString& InTip, QFormLayout* InForm)
{
    /* Written straight into the emitter's keys: previewed while dragged, one undo step when done. */
    const auto Store = [this, bInGradient, InName](const bool bInFinished)
    {
        ParticleEmitterDesc* Emitter = Document.GetEmitter(Document.GetSelected());
        if(!Emitter) { return; }
        if(bInGradient)
        {
            const std::vector<RenderCore::ParticleColorKey>& Keys = Gradient->GetKeys();
            Emitter->ColorKeyCount = int32_t(std::min(Keys.size(), size_t(RenderCore::PARTICLE_CURVE_KEYS_MAX)));
            std::copy(Keys.begin(), Keys.begin() + Emitter->ColorKeyCount, Emitter->ColorKeys);
        }
        else
        {
            const std::vector<RenderCore::ParticleSizeKey>& Keys = Curve->GetKeys();
            Emitter->SizeKeyCount = int32_t(std::min(Keys.size(), size_t(RenderCore::PARTICLE_CURVE_KEYS_MAX)));
            std::copy(Keys.begin(), Keys.begin() + Emitter->SizeKeyCount, Emitter->SizeKeys);
        }
        if(bInFinished) { Document.CommitLiveEdit(QStringLiteral("Edit %1").arg(InName)); }
        else            { Document.PreviewLiveEdit(); }
    };

    auto* Label = new QLabel(InName, Content);
    Label->setToolTip(InTip);
    QWidget* Editor = nullptr;
    if(bInGradient)
    {
        Gradient = new ParticleGradientEdit(Content);
        connect(Gradient, &ParticleGradientEdit::Edited, this, Store);
        Editor = Gradient;
    }
    else
    {
        Curve = new ParticleCurveEdit(Content);
        connect(Curve, &ParticleCurveEdit::Edited, this, Store);
        Editor = Curve;
    }
    InForm->addRow(Label);
    InForm->addRow(Editor);
}

void ParticleEmitterDetailsPanel::Refresh()
{
    const ParticleEmitterDesc* Emitter = Document.GetEmitter(Document.GetSelected());

    /* A new type, mode or shape shows other settings: rebuilt once this edit's signals are done with the widgets. */
    if(Emitter && (!Rows.empty() || Gradient || Curve) && ShapeOf(*Emitter) != ShownShape)
    {
        if(!bRebuildQueued)
        {
            bRebuildQueued = true;
            QTimer::singleShot(0, this, [this]()
            {
                bRebuildQueued = false;
                Rebuild();
            });
        }
        return;
    }
    if(Emitter && Gradient) { Gradient->SetKeys(Emitter->ColorKeys, Emitter->ColorKeyCount); }
    if(Emitter && Curve)    { Curve->SetKeys(Emitter->SizeKeys, Emitter->SizeKeyCount); }

    static const ParticleEmitterDesc Defaults{};

    /* A section's Reset, while any of its settings -- shown or not -- differs from a fresh emitter's. */
    for(const SectionState& Section : Sections)
    {
        bool bChanged = false;
        const QByteArray Category = Section.Name.toLatin1();
        for(const EmitterSetting& Each : GetSettings())
        {
            if(!Emitter || bChanged || std::strcmp(Each.Entry.Category, Category.constData()) != 0) { continue; }
            bChanged = !IsPropertyAtDefault(Each.Entry, reinterpret_cast<const uint8_t*>(Emitter) + Each.Entry.Offset,
                                            reinterpret_cast<const uint8_t*>(&Defaults) + Each.Entry.Offset);
        }
        Section.ResetAll->setEnabled(bChanged);
    }

    for(const Row& Each : Rows)
    {
        Each.Editor->Refresh(true);
        if(Emitter)
        {
            const GameCore::ComponentPropertyEntry& Entry = GetSettings()[Each.Entry].Entry;
            Each.Reset->setVisible(!IsPropertyAtDefault(Entry, reinterpret_cast<const uint8_t*>(Emitter) + Entry.Offset,
                                                        reinterpret_cast<const uint8_t*>(&Defaults) + Entry.Offset));
        }
    }

    if(!Emitter)
    {
        Budget->clear();
        return;
    }
    const RenderCore::ParticleEmitterSettings Settings = Emitter->ToSettings();
    if(Settings.Type == RenderCore::PT_Beam)
    {
        Budget->setText(QStringLiteral("A beam of <b>%1</b> segments: %2 points, redrawn every frame.")
                            .arg(Settings.BeamSegments).arg(Settings.BeamSegments + 1));
        return;
    }
    const float Held  = Emitter->GetSteadyStateCount();
    /* What it would hold with no cap: the rate times a mean life, or the bursts alive at once. */
    const float MeanLife = Settings.Lifetime * (1.0f - 0.5f * Settings.LifetimeVariance);
    float Wants = Settings.SpawnRate * MeanLife;
    if(Settings.SpawnMode == RenderCore::PSM_Burst)
    {
        float Overlap = Settings.BurstInterval > 0.0f ? std::max(1.0f, std::ceil(MeanLife / Settings.BurstInterval)) : 1.0f;
        if(Settings.BurstInterval > 0.0f && Settings.BurstCycles > 0) { Overlap = std::min(Overlap, float(Settings.BurstCycles)); }
        Wants = float(Settings.BurstCount) * Overlap;
    }
    QString Text = QStringLiteral("About <b>%1</b> of %2 particles once it has run %3 s.")
                       .arg(int(std::lround(Held))).arg(Settings.MaxParticles).arg(double(Settings.Lifetime), 0, 'g', 3);
    if(Wants > float(Settings.MaxParticles) * 1.02f)
    {
        Text += QStringLiteral("<br><span style='color:#e0a030'>Full: it would hold about %1, so the oldest are replaced "
                               "before they die. Raise Max Particles (up to %2), or spawn fewer or shorter-lived ones.</span>")
                    .arg(int(std::lround(Wants))).arg(RenderCore::PARTICLE_MAX_PER_SYSTEM);
    }
    Budget->setText(Text);
}

#endif // WITH_EDITOR
