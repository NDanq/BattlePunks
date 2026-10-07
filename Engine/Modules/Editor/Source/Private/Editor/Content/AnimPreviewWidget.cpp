#if WITH_EDITOR

#include "Private/Editor/Content/AnimPreviewWidget.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/RenderCore/SkeletonAsset.h"
#include "Private/Editor/Content/MeshPreviewView.h"

namespace
{
    constexpr int SCRUB_STEPS = 1000;   // the scrubber's resolution over a clip
}

AnimPreviewWidget::AnimPreviewWidget(QWidget* InParent) : QWidget(InParent)
{
    View = new MeshPreviewView(this);

    PlayButton = new QToolButton(this);
    PlayButton->setToolTip(QStringLiteral("Play or pause (the level is not affected)"));
    connect(PlayButton, &QToolButton::clicked, this, [this]() { SetPlaying(!bPlaying); });

    Scrubber = new QSlider(Qt::Horizontal, this);
    Scrubber->setRange(0, SCRUB_STEPS);
    Scrubber->setToolTip(QStringLiteral("Drag to scrub the first layer's current clip"));
    connect(Scrubber, &QSlider::sliderPressed, this, [this]() { bScrubbing = true; SetPlaying(false); });
    connect(Scrubber, &QSlider::sliderReleased, this, [this]() { bScrubbing = false; });
    connect(Scrubber, &QSlider::valueChanged, this, [this](const int InValue)
    {
        if(bScrubbing) { Seek(Instance.GetDuration() * float(InValue) / float(SCRUB_STEPS)); }
    });

    TimeLabel = new QLabel(this);
    TimeLabel->setMinimumWidth(90);

    Speed = new QDoubleSpinBox(this);
    Speed->setRange(0.0, 4.0);
    Speed->setSingleStep(0.1);
    Speed->setValue(1.0);
    Speed->setSuffix(QStringLiteral("x"));
    Speed->setToolTip(QStringLiteral("Play rate"));
    connect(Speed, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](const double InRate) { Instance.SetPlayRate(float(InRate)); });

    Loop = new QCheckBox(QStringLiteral("Loop"), this);
    Loop->setChecked(true);
    Loop->setToolTip(QStringLiteral("A clip loops here whatever it says; off, it plays as it says (an anim state's states choose their own)"));
    connect(Loop, &QCheckBox::toggled, this, [this](const bool bInLoop) { Instance.SetLoopOverride(bInLoop ? 1 : -1); });
    Instance.SetLoopOverride(1);

    Bones = new QCheckBox(QStringLiteral("Bones"), this);
    Bones->setToolTip(QStringLiteral("Draw the skeleton over the mesh"));
    connect(Bones, &QCheckBox::toggled, this, [this]() { PushPose(); });

    auto* Transport = new QHBoxLayout();
    Transport->setContentsMargins(0, 0, 0, 0);
    Transport->addWidget(PlayButton);
    Transport->addWidget(Scrubber, 1);
    Transport->addWidget(TimeLabel);
    Transport->addWidget(Speed);
    Transport->addWidget(Loop);
    Transport->addWidget(Bones);

    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->addWidget(View, 1);
    Layout->addLayout(Transport);

    /* About as often as the preview answers; each step sends the pose. */
    Clock = new QTimer(this);
    Clock->setInterval(33);
    connect(Clock, &QTimer::timeout, this, &AnimPreviewWidget::Step);

    UpdateTransport();
}

bool AnimPreviewWidget::SetMesh(const QString& InRuntimePath, const RenderCore::MeshData& InMesh)
{
    MeshCopy = InMesh;
    Mesh = &MeshCopy;

    /* The bones the skin uses. Ancestors no vertex follows -- an armature's
     * root, a Z-up node -- would draw long lines to the origin. */
    Weighted.assign(InMesh.InverseBind.size(), false);
    for(size_t At = 0; At < InMesh.Joints.size() && At < InMesh.Weights.size(); ++At)
    {
        if(InMesh.Weights[At] > 0.0f && InMesh.Joints[At] < Weighted.size()) { Weighted[InMesh.Joints[At]] = true; }
    }
    View->SetMesh(InRuntimePath);
    std::shared_ptr<const RenderCore::SkeletonData> Skeleton =
        InMesh.IsSkinned() ? RenderCore::SkeletonAssetCache::Find(InMesh.Skeleton) : nullptr;
    Instance.SetSkeleton(Skeleton);
    PushPose();
    UpdateTransport();
    return Skeleton != nullptr;
}

void AnimPreviewWidget::PlayClip(std::shared_ptr<const RenderCore::AnimClipData> InClip)
{
    Instance.PlayClip(std::move(InClip));
    PushPose();
    SetPlaying(Instance.GetClip() != nullptr);
}

void AnimPreviewWidget::PlayState(std::shared_ptr<const RenderCore::AnimStateData> InState)
{
    Instance.SetState(std::move(InState));
    PushPose();
    SetPlaying(Instance.GetState() != nullptr);
}

void AnimPreviewWidget::Repose()
{
    Instance.Evaluate();
    PushPose();
    emit Advanced();
}

void AnimPreviewWidget::SetBonesShown(const bool bInShown)
{
    Bones->setChecked(bInShown);
}

void AnimPreviewWidget::SetPlaying(const bool bInPlaying)
{
    bPlaying = bInPlaying && HasSkeleton();
    if(bPlaying)
    {
        Elapsed.restart();
        Clock->start();
    }
    else
    {
        Clock->stop();
    }
    UpdateTransport();
}

void AnimPreviewWidget::Seek(const float InSeconds)
{
    Instance.SetTime(InSeconds);
    PushPose();
    UpdateTransport();
    emit Advanced();
}

void AnimPreviewWidget::Step()
{
    /* Real time since the last step, so a slow answer does not slow the clip. */
    const float Delta = std::min(float(Elapsed.restart()) / 1000.0f, 0.1f);
    Instance.Update(Delta);
    for(const std::string& Name : Instance.GetFiredNotifies())
    {
        emit NotifyFired(QString::fromStdString(Name));
    }
    PushPose();
    UpdateTransport();
    emit Advanced();
}

void AnimPreviewWidget::PushPose()
{
    if(!Mesh || !Instance.HasPose())
    {
        static const float Zero[3] = { 0.0f, 0.0f, 0.0f };
        View->SetSkinPalette({});
        View->SetSkeletonOverlay({}, Zero, Zero);
        return;
    }

    /* The palette: each bone's pose after the mesh's inverse bind. */
    const std::vector<RenderCore::AnimMatrix>& Model = Instance.GetModelPose();
    const size_t Count = std::min(Model.size(), Mesh->InverseBind.size());
    std::vector<float> Palette(Count * 12);
    for(size_t Bone = 0; Bone < Count; ++Bone)
    {
        const RenderCore::AnimMatrix Skin = RenderCore::AnimMath::Mul(Model[Bone], Mesh->InverseBind[Bone]);
        std::memcpy(&Palette[Bone * 12], Skin.M, sizeof(Skin.M));
    }
    View->SetSkinPalette(std::move(Palette));

    std::vector<float> Segments;
    if(Bones->isChecked())
    {
        /* Weighted bones only: each a joint, joined to its parent if that is weighted too. */
        const RenderCore::SkeletonData& Skeleton = *Instance.GetSkeleton();
        const auto IsWeighted = [this](const int32_t InBone) { return InBone >= 0 && size_t(InBone) < Weighted.size() && Weighted[size_t(InBone)]; };
        for(size_t Bone = 0; Bone < Model.size() && Bone < Skeleton.Bones.size(); ++Bone)
        {
            if(!IsWeighted(int32_t(Bone))) { continue; }
            const int32_t Parent = Skeleton.Bones[Bone].Parent;
            const RenderCore::AnimMatrix& To = Model[size_t(IsWeighted(Parent) ? Parent : int32_t(Bone))];
            Segments.insert(Segments.end(), { Model[Bone].M[0][3], Model[Bone].M[1][3], Model[Bone].M[2][3],
                                              To.M[0][3], To.M[1][3], To.M[2][3] });
        }
    }
    View->SetSkeletonOverlay(std::move(Segments), Mesh->BoundsMin, Mesh->BoundsMax);
}

void AnimPreviewWidget::UpdateTransport()
{
    PlayButton->setIcon(style()->standardIcon(bPlaying ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    PlayButton->setEnabled(HasSkeleton());
    Scrubber->setEnabled(HasSkeleton() && Instance.GetDuration() > 0.0f);
    const float Duration = Instance.GetDuration();
    if(!bScrubbing)
    {
        const QSignalBlocker Block(Scrubber);
        Scrubber->setValue(Duration > 0.0f ? int(Instance.GetTime() / Duration * float(SCRUB_STEPS)) : 0);
    }
    TimeLabel->setText(HasSkeleton() ? QStringLiteral("%1 / %2 s").arg(double(Instance.GetTime()), 0, 'f', 2).arg(double(Duration), 0, 'f', 2)
                                     : QStringLiteral("no skeleton"));
}

void SyncParameterControls(const std::vector<AnimParameterControl>& InControls, const RenderCore::AnimInstance& InInstance)
{
    for(const AnimParameterControl& Control : InControls)
    {
        if(!Control.Widget || Control.Widget->hasFocus())
        {
            continue;
        }
        const float Value = InInstance.GetParameter(Control.Name);
        const QSignalBlocker Block(Control.Widget);
        if(auto* Spin = qobject_cast<QDoubleSpinBox*>(Control.Widget))
        {
            if(std::abs(Spin->value() - double(Value)) > 1e-4) { Spin->setValue(double(Value)); }
        }
        else if(auto* Check = qobject_cast<QCheckBox*>(Control.Widget))
        {
            Check->setChecked(Value != 0.0f);
        }
        else if(auto* Button = qobject_cast<QPushButton*>(Control.Widget))
        {
            /* Set and waiting for a transition to use it, or spent. */
            Button->setText(Value != 0.0f ? QStringLiteral("Fired...") : QStringLiteral("Fire"));
        }
    }
}

#endif // WITH_EDITOR
