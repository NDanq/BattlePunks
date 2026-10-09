#ifndef SEQUENCER_SEQUENCEASSET_H
#define SEQUENCER_SEQUENCEASSET_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/GameCore/EntityGuid.h"
#include "Public/Sequencer/SequencerAPI.h"

/*
 * A sequence (.nmseq): a cutscene's timeline, as data. It names the entities
 * it controls (bindings) and what it does to them over time (tracks). Times
 * are whole frames at the sequence's own rate, so a key sits exactly where it
 * was put; the playhead between them is fractional.
 *
 * Nothing here touches a World -- SequenceWorld.h plays an asset. README.md
 * has the file format.
 */
namespace Sequencer
{
    constexpr const char* SEQUENCE_EXTENSION = ".nmseq";

    /* How a key's value travels to the next key. The named eases are
     * UIAnimation.h's set; Auto is a smooth curve through the neighbours
     * (Catmull-Rom), the default for anything that moves. */
    enum class ESequenceInterp : uint8_t
    {
        Constant = 0,   // hold until the next key
        Linear,
        Auto,
        QuadIn,
        QuadOut,
        QuadInOut,
        CubicIn,
        CubicOut,
        CubicInOut,
        BackOut,
        ElasticOut,
        User,           // a curve with the key's own tangents (the curve editor's)
        Count
    };

    /* Never null; "Auto" for out-of-range. The file's spelling. */
    SEQUENCER_API const char* InterpName(ESequenceInterp InInterp);
    /* False, OutInterp untouched, for an unknown name. */
    SEQUENCER_API bool        ParseInterp(const char* InName, ESequenceInterp& OutInterp);

    struct SequenceKey
    {
        int32_t         Frame  = 0;
        float           Value  = 0.0f;
        ESequenceInterp Interp = ESequenceInterp::Auto;   // toward the next key

        /* User keys: the slope arriving and leaving, in value per frame. A
         * curve through a User key uses them; Auto works its own out. */
        float           InTangent  = 0.0f;
        float           OutTangent = 0.0f;
    };

    /* One animated value: keys in frame order, no two on one frame. */
    struct SEQUENCER_API SequenceChannel
    {
        std::vector<SequenceKey> Keys;

        bool IsEmpty() const { return Keys.empty(); }

        /* The value at InFrame (fractional). Held flat before the first key
         * and after the last. InDefault if there are no keys. */
        float Evaluate(double InFrame, float InDefault = 0.0f) const;

        /* Sorts by frame; a later key on a frame already used replaces it. */
        void Normalize();

        /* The slope the curve leaves key InIndex with (value per frame):
         * a User key's own, else the smooth one Auto uses. */
        float GetLeaveSlope(size_t InIndex) const;
        float GetArriveSlope(size_t InIndex) const;
    };

    /* What a track does. */
    enum class ESequenceTrackType : uint8_t
    {
        Transform = 0,   // the binding's Transform: up to nine channels
        Float,           // one number field of one of its components
        Bool,            // one flag field of one of its components, stepped
        Event,           // named EventBus events at frames
        Animation,       // the binding's clips, in sections (blending where they overlap)
        Audio,           // sounds in sections: at the binding, or heard everywhere on a track of no binding
        CameraCut,       // which camera binding the game is seen through, in sections; blends between them
        Fade,            // the screen to black: one channel, 0 clear .. 1 black
        SubSequence,     // other sequences, played in sections
        PostProcess,     // post process stacks (.pp.nmgraph) in sections, faded in by weight over the level's volumes
        PostProcessSetting, // one post process setting (Field: "Exposure", "DofFocusDistance"...) keyed over everything
        Count
    };

    /* Section tracks keep sections, not keys. */
    SEQUENCER_API bool IsSectionTrack(ESequenceTrackType InType);

    SEQUENCER_API const char* TrackTypeName(ESequenceTrackType InType);
    SEQUENCER_API bool        ParseTrackType(const char* InName, ESequenceTrackType& OutType);

    /* A Transform track's channels, in this order. */
    enum ETransformChannel : uint8_t
    {
        TC_PositionX = 0, TC_PositionY, TC_PositionZ,
        TC_RotationX, TC_RotationY, TC_RotationZ,
        TC_ScaleX, TC_ScaleY, TC_ScaleZ,
        TC_Count
    };

    /* "Position.X" ... "Scale.Z"; never null. */
    SEQUENCER_API const char* TransformChannelName(size_t InChannel);
    /* TC_Count for an unknown name. */
    SEQUENCER_API size_t      FindTransformChannel(const char* InName);

    /* One argument of an event key. A Binding argument is the entity a
     * binding resolved to, when the event fires. */
    struct SequenceEventArg
    {
        enum class EType : uint8_t { Number = 0, Bool, String, Binding };

        EType       Type   = EType::String;
        double      Number = 0.0;
        bool        bValue = false;
        std::string Text;            // String: the text; Binding: the binding's Id

        /* The file's spelling: a number, true / false, @id for a binding,
         * anything else a string. */
        static SequenceEventArg FromText(const std::string& InText);
        std::string             ToText() const;
    };

    enum : size_t { SEQUENCE_EVENT_ARGS_MAX = 4 };   // an EventBus event's

    struct SequenceEventKey
    {
        int32_t                       Frame = 0;
        std::string                   Event;             // the EventBus name
        std::vector<SequenceEventArg> Args;              // up to SEQUENCE_EVENT_ARGS_MAX
        bool                          bFireOnSkip = false;
    };

    /*
     * A span of a section track, [Start, End) in frames. What Asset means
     * depends on the track: Animation a .nmanim, Audio a .nmsound,
     * SubSequence a .nmseq, CameraCut the id of the camera's binding,
     * PostProcess a .pp.nmgraph stack.
     */
    struct SequenceSection
    {
        int32_t     Start = 0;
        int32_t     End   = 0;
        std::string Asset;

        float   PlayRate = 1.0f;   // Animation, Audio, SubSequence: how fast the asset plays
        float   Volume   = 1.0f;   // Audio's volume; PostProcess's weight (0..1)
        int32_t Offset   = 0;      // frames into the asset at Start (a trimmed start)
        int32_t BlendIn  = 0;      // frames: Animation crossfade, Audio fade-in, CameraCut blend
        int32_t BlendOut = 0;      // frames: Animation, Audio fade-out
        bool    bLoop    = false;  // Animation, Audio: wrap when the asset runs out

        int32_t GetLength() const { return End > Start ? End - Start : 0; }
        bool    Contains(double InFrame) const { return InFrame >= double(Start) && InFrame < double(End); }

        /* Seconds into the asset at InFrame, at InFrameRate. */
        double AssetSeconds(double InFrame, uint32_t InFrameRate) const
        {
            return (InFrame - double(Start) + double(Offset)) * double(PlayRate) / double(InFrameRate ? InFrameRate : 1);
        }
    };

    struct SEQUENCER_API SequenceTrack
    {
        ESequenceTrackType Type = ESequenceTrackType::Transform;

        /* Float and Bool: the component (its registered name, "Camera") and
         * the field as Lua sees it ("FovY") -- any field the component binds
         * for scripts. PostProcessSetting: Field is the setting's name
         * (RenderCore's PostProcessParamName). */
        std::string Component;
        std::string Field;

        /* Transform: TC_Count channels, empty ones not animated. Float and
         * Bool: one. Event: none. */
        std::vector<SequenceChannel> Channels;

        /* Event only, in frame order. */
        std::vector<SequenceEventKey> Events;

        /* Section tracks, by Start. */
        std::vector<SequenceSection> Sections;

        /* The track's label; "" shows its type. */
        std::string Name;

        /* Muted tracks are skipped when the sequence plays (the editor's). */
        bool bMuted = false;

        /* Channels sized for Type, keys and events sorted. */
        void Normalize();

        /* The last frame anything on this track happens; -1 if empty. */
        int32_t GetLastFrame() const;
    };

    /* How a binding finds its entity. */
    enum class ESequenceBindingKind : uint8_t
    {
        Possess = 0,   // one already in the level: by Guid, else by Name
        Spawn,         // made from Prefab when the sequence starts, destroyed when it ends
    };

    struct SequenceBinding
    {
        std::string          Id;            // unique in the asset; what @id and Bind() name
        std::string          Name;          // the entity's name: shown, and the fallback lookup
        ESequenceBindingKind Kind = ESequenceBindingKind::Possess;
        GameCore::EntityGuid Guid;          // Possess
        std::string          Prefab;        // Spawn
        std::vector<SequenceTrack> Tracks;
    };

    struct SEQUENCER_API SequenceAsset
    {
        uint32_t FrameRate = 24;   // frames per second, 1..1000
        int32_t  Length    = 0;    // in frames; the sequence ends here

        std::vector<SequenceBinding> Bindings;

        /* Tracks of no binding: Event, Audio (heard everywhere), CameraCut,
         * Fade, SubSequence, PostProcess and PostProcessSetting. */
        std::vector<SequenceTrack> Tracks;

        /* Named frames, for authoring (the editor's markers) and Lua
         * (Sequence.SeekToMarker). */
        struct Marker
        {
            int32_t     Frame = 0;
            std::string Name;
        };
        std::vector<Marker> Markers;

        /* -1 if none has InName. */
        int32_t FindMarker(const std::string& InName) const;

        double FramesToSeconds(double InFrames) const { return InFrames / double(FrameRate ? FrameRate : 1); }
        double SecondsToFrames(double InSeconds) const { return InSeconds * double(FrameRate ? FrameRate : 1); }
        double GetDurationSeconds() const { return FramesToSeconds(double(Length)); }

        /* -1 if no binding has InId. */
        int32_t FindBinding(const std::string& InId) const;

        /* Every track normalized, FrameRate clamped. */
        void Normalize();

        /* Every asset file its sections and spawn bindings name (clips,
         * sounds, sub-sequences, prefabs), once each: what the cook checks
         * is there. */
        std::vector<std::string> GetReferencedAssets() const;

        /* What is wrong with it, one line each; empty when it is sound.
         * Loading does not refuse an asset with problems -- the player skips
         * what it cannot use -- but the editor and the cook report them. */
        std::vector<std::string> Validate() const;
    };

    /*
     * Loaded sequences, by runtime path, shared by every player. Game thread
     * only. A path that fails to load is logged once and remembered as
     * failed until Invalidate.
     */
    class SEQUENCER_API SequenceAssetCache
    {
    public:
        /* Null if InPath cannot be loaded. */
        static std::shared_ptr<const SequenceAsset> Find(const std::string& InPath);

        /* The next Find reads InPath again (the editor saved it). Players
         * already holding the old asset keep it until they restart. */
        static void Invalidate(const std::string& InPath);
        static void Clear();

        /* InAsset is what InPath loads until Invalidate: an asset that is
         * not on disk (yet) -- the editor's unsaved edit, a test's. */
        static void Set(const std::string& InPath, std::shared_ptr<const SequenceAsset> InAsset);
    };

    /* The easing curve of InInterp at InT (0..1), for the named eases --
     * Constant 0, Linear and Auto InT. */
    SEQUENCER_API float Ease(ESequenceInterp InInterp, float InT);
}

#endif //SEQUENCER_SEQUENCEASSET_H
