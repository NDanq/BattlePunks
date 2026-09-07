#ifndef ENGINECORE_CORECONFIG_H
#define ENGINECORE_CORECONFIG_H

#include <cstdint>
#include <cstring>
#include <string>

#include "Public/Core/CoreAPI.h"

/*
 * Declarative config vars: a subsystem declares a typed, defaulted, named
 * variable at file scope; an .ini on disk or a console command can override it
 * at runtime, and either path fires the same callback.
 *
 * WHY AN INTRUSIVE LIST AND NOT std::vector
 *
 * A config var is a file-scope object in whatever image declared it. Its
 * constructor runs during that image's dynamic initialisation -- before
 * main() for the executable and EngineCore.dll, and during LoadLibrary for a
 * hot module. It has to put itself somewhere, and that somewhere must ALREADY
 * EXIST.
 *
 * A std::vector<ConfigVar*> at file scope does not already exist. It has a
 * constructor of its own, and nothing orders it against the constructors that
 * would push into it: across translation units the order is unspecified, and
 * across DLLs it is not even a question the standard answers. The first var to
 * register would push into a vector whose members are uninitialised garbage.
 * The same argument kills std::map, and CoreModuleManager.h has already made
 * the matching argument against a Meyers singleton for owned state.
 *
 *      ConfigVar* GConfigHead = nullptr;
 *
 * has no constructor. It is CONSTANT-initialised into .bss before any dynamic
 * initialiser in the process runs, so there is no ordering question left to
 * get wrong -- every ConfigVar constructor, in every image, in any order,
 * finds a valid empty list. That is the entire reason for the design; that it
 * also allocates nothing is a bonus, not the point.
 *
 * The cost is a linear walk for lookup. Core already scans linearly everywhere
 * (there is no std::map in this module), and this walk happens on a file load
 * and on a console keystroke, never in a frame loop.
 *
 * WHY THE HEAD LIVES IN EngineCore.dll
 *
 * ConfigVar's constructor and destructor are out-of-line and CORE_API. If they
 * were inline, every image would link its own copy of the list head and every
 * module would build a private registry: the host's console would not see a
 * module's vars, LoadConfigFile would silently apply nothing to them, and
 * there would be no diagnostic anywhere -- the same failure CoreModule.h
 * describes for GLogHandler. This requires CORE_SHARED=ON, which
 * Engine/CMakeLists.txt already forces.
 *
 * See CONFIG HAZARDS at the bottom of this file before declaring a config var
 * inside a hot-reloadable module.
 */

namespace EngineCore
{
    /* CT_ prefix matches EModuleState's MS_ -- how this codebase reads at a switch. */
    enum class EConfigType : uint8_t
    {
        CT_Bool = 0,
        CT_Int,       // int32_t
        CT_Float,
        CT_String
    };

    /* Max bytes of a string config value, NUL included. */
    enum : size_t { CONFIG_STRING_MAX = 128 };

    /*
     * WHY STRINGS ARE A FIXED BUFFER AND NOT std::string
     *
     * A std::string member would allocate from whichever image constructed the
     * var, and be reassigned by a setter running inside EngineCore.dll. Under
     * /MD both land in one heap and it looks fine, right up until somebody
     * builds a module /MT, at which point it is a silent cross-heap free with
     * no diagnostic -- exactly the trap ModuleReloadState.h documents. A fixed
     * inline char[128] has no owner and no allocation, so the question does not
     * arise. A value that needs more than that wants a file path IN the value,
     * not a bigger buffer.
     */
    union ConfigValue
    {
        bool    AsBool;
        int32_t AsInt;
        float   AsFloat;
        char    AsString[CONFIG_STRING_MAX];
    };

    // Header-only, deliberately NOT CORE_API -- see CoreAPI.h.
    constexpr ConfigValue MakeConfigBool(const bool bInValue)
    {
        ConfigValue V{};
        V.AsBool = bInValue;
        return V;
    }

    constexpr ConfigValue MakeConfigInt(const int32_t InValue)
    {
        ConfigValue V{};
        V.AsInt = InValue;
        return V;
    }

    constexpr ConfigValue MakeConfigFloat(const float InValue)
    {
        ConfigValue V{};
        V.AsFloat = InValue;
        return V;
    }

    /* Not constexpr -- copies into the inline buffer. Truncates silently: this
     * is a compiled-in literal the programmer controls, not runtime input, so
     * the loud warning belongs to ConfigVar::SetString instead. */
    inline ConfigValue MakeConfigString(const char* InValue)
    {
        ConfigValue V{};
        if(InValue)
        {
            std::strncpy(V.AsString, InValue, CONFIG_STRING_MAX - 1);
            V.AsString[CONFIG_STRING_MAX - 1] = '\0';
        }
        else
        {
            V.AsString[0] = '\0';
        }
        return V;
    }

    class ConfigVar;

    /*
     * A raw function pointer, matching LogHandler, and NOT std::function.
     * std::function stored in a ConfigVar would allocate in the image that
     * declared the var and be freed by whoever ran the destructor. A
     * capture-free function pointer can dangle in exactly one way -- its image
     * unloaded -- and that case is already handled, because the var holding it
     * dies in the same unload. See CONFIG HAZARDS.
     *
     * The callback receives the var it belongs to and must read ONLY that var:
     * LoadConfigFile fires callbacks mid-file, so a sibling var may not have
     * its new value applied yet.
     */
    using ConfigChangedFn = void (*)(const ConfigVar& InVar);

    // Declared here, ahead of the class, so the friend declarations below can
    // name them without repeating CORE_API on a friend decl -- see the
    // Registry section further down for the actual (identical) declarations.
    CORE_API void ConfigRegisterVar  (ConfigVar* InVar);
    CORE_API void ConfigUnregisterVar(ConfigVar* InVar);

    /*
     * LAYOUT IS FROZEN, same rule as ModuleReloadState.
     *
     * Modules construct ConfigVar objects, so their compiled code bakes in this
     * class's size and member offsets. Adding, removing or reordering a member
     * without bumping MODULE_ABI_REVISION in CoreModule.h produces a module
     * reading and writing at the wrong offsets, with no diagnostic.
     *
     * The constructor and destructor are out-of-line so that registration
     * policy -- duplicate detection, the list splice, the retained-entry
     * back-fill -- lives in exactly one image and can change without
     * rebuilding every module that declares a var.
     */
    class CORE_API ConfigVar
    {
    public:
        ConfigVar(EConfigType InType, const char* InSection, const char* InName,
                  ConfigValue InDefault, ConfigChangedFn InOnChanged);
        ~ConfigVar();

        /* Self-registering singletons are not copyable: a copy would link a
         * second node with the same identity, and the copy's destructor would
         * unlink whichever node the walk found first. Not movable either --
         * a node's address IS its list identity. */
        ConfigVar(const ConfigVar&)            = delete;
        ConfigVar& operator=(const ConfigVar&) = delete;
        ConfigVar(ConfigVar&&)                 = delete;

        const char* GetSection() const { return Section; }
        const char* GetName()    const { return Name; }
        EConfigType GetType()    const { return Type; }
        ConfigVar*  GetNext()    const { return Next; }
        bool        IsOverridden() const { return bOverridden; }

        /*
         * A type mismatch is a programming error, not user input, so it is a
         * CORE_CHECK and not a silent conversion: reading an int var through
         * GetBool() means the call site and the declaration disagree, and
         * quietly returning `Value.AsInt != 0` would hide that forever.
         *
         * GetString's pointer aims into this node's buffer and is invalidated
         * by the next set -- copy it if you keep it.
         */
        bool        GetBool()   const;
        int32_t     GetInt()    const;
        float       GetFloat()  const;
        const char* GetString() const;

        /* Each returns true if the value ACTUALLY CHANGED, and fires OnChanged
         * exactly once in that case -- an .ini restating the compiled default
         * must not tear down a swapchain. */
        bool SetBool  (bool        bInValue);
        bool SetInt   (int32_t     InValue);
        bool SetFloat (float       InValue);
        bool SetString(const char* InValue);

        /*
         * Parses InText per this var's type. Returns false (no change, no
         * callback) if the text is not a valid value for this var -- this is
         * the ONE entry point the .ini parser and the console parser share, so
         * the two can never drift apart on what "on" means.
         *
         * OutbChanged, if given, reports whether the parsed value differed
         * from the current one -- independent of the return value, which only
         * reports whether InText parsed at all.
         */
        bool SetFromString(const std::string& InText, bool* OutbChanged = nullptr);

        std::string ToString() const;             // "true" / "60" / "1.5" / the text
        std::string GetQualifiedName() const;      // "Section.Name", or "Name" if sectionless

        bool ResetToDefault();                     // fires the callback if it changed
        void FireCallback() const;                 // unconditional; used by ConfigApplyAll

    private:
        // Repeats CORE_API to match the declaration above exactly -- MSVC
        // treats a friend re-declaration with different dll linkage as
        // inconsistent (C4273) even though it names the same function.
        friend CORE_API void ConfigRegisterVar  (ConfigVar* InVar);
        friend CORE_API void ConfigUnregisterVar(ConfigVar* InVar);

        const char*     Section;
        const char*     Name;
        ConfigValue     Value;
        ConfigValue     Default;
        ConfigChangedFn OnChanged;
        ConfigVar*      Next;
        EConfigType     Type;
        bool            bOverridden;   // an .ini or a console set touched this
    };

    // === Registry ===========================================================
    // ConfigRegisterVar / ConfigUnregisterVar are declared above, ahead of the
    // class, for the friend declarations -- this is the rest of the seam.

    /* for(ConfigVar* V = ConfigGetFirst(); V; V = V->GetNext())
     * Order is reverse registration order and is NOT stable across builds --
     * never depend on it for anything but display. */
    CORE_API ConfigVar* ConfigGetFirst();
    CORE_API int        ConfigGetVarCount();

    /* Case-insensitive on both parts. An empty section matches only vars
     * declared with an empty section. */
    CORE_API ConfigVar* ConfigFindVar(const char* InSection, const char* InName);

    /* "Render.VSync", or a bare "VSync" to search every section. OutbAmbiguous,
     * if given, is set when a bare name matches more than one section; the
     * first match in registry order is still returned. */
    CORE_API ConfigVar* ConfigFindVarByPath(const std::string& InQualifiedName,
                                             bool* OutbAmbiguous = nullptr);

    // === Load ================================================================

    struct ConfigLoadStats
    {
        int LinesRead      = 0;
        int KeysApplied    = 0;   // matched a registered var and parsed
        int ValuesChanged  = 0;   // ... and actually differed
        int UnknownKeys    = 0;   // retained; a later-loading module may claim them
        int BadValues      = 0;
        int MalformedLines = 0;
    };

    /* Reads through GetFileProvider() when one is installed, falling back to
     * appFileReadAll. Never <fstream>. Returns false with a WARNING, not an
     * error, when the file cannot be read -- a missing user override file is
     * the normal case on a first run. */
    CORE_API bool LoadConfigFile(const std::string& InPath, ConfigLoadStats* OutStats = nullptr);

    /* The whole parser with the file read factored out -- what the smoke test
     * drives, for full grammar coverage at no scratch-file cost. */
    CORE_API bool LoadConfigText(const std::string& InText, ConfigLoadStats* OutStats = nullptr);

    /* Fires every registered var's callback unconditionally, changed or not.
     * NOT part of the load path -- see WHY CALLBACKS FIRE INLINE in
     * CoreConfig.cpp. For a host that builds its subsystems after reading
     * config and needs one sweep to push current values out. */
    CORE_API void ConfigApplyAll();
    CORE_API void ConfigResetAllToDefaults();

    /* One CORE_LOG_WARN per entry parsed by a LoadConfigFile/LoadConfigText
     * call that no registered var ever claimed. Call once every module is up:
     * an unknown key during parsing is only Log verbosity, on the assumption
     * that a module loading later will claim it. This is the deferred check
     * that catches an actual typo. */
    CORE_API void ConfigWarnUnclaimedEntries();

    // === Console =============================================================

    /*
     * Accepted syntax (get and set only in this cut -- the "config." prefix is
     * reserved for future verbs such as list/reset/save and must never be used
     * as a section name):
     *
     *     Name                  get, searched across every section
     *     Section.Name          get
     *     Section.Name Value    set
     *     Section.Name = Value  set -- '=' is optional sugar, dropped as a token
     *
     * OutResponse is ALWAYS written, including on failure, and is what a
     * console echoes next to the line the user typed. The return value means
     * "this was a config command and it succeeded", so a host console can try
     * its own verbs on false rather than pre-classifying the line.
     *
     * An unknown key is a FAILURE, not an auto-create: a CVar system that
     * creates on set turns every typo into a variable nobody will ever read,
     * and hands back a success message for a command that did nothing.
     */
    CORE_API bool ExecConfigCommand(const std::string& InCommand, std::string& OutResponse);
}

/*
 * ONE per config var, at FILE SCOPE, in a .cpp -- never in a header.
 *
 * A definition in a header gives every including translation unit its own
 * ConfigVar. All of them self-register, so LoadConfigFile sets all N and a
 * direct SetInt through one leaves the other N-1 stale, with readers in
 * different TUs silently disagreeing. Registration warns on a duplicate, which
 * is the diagnostic for exactly this mistake -- see CoreConfig.cpp. Use
 * NIMBLE_CONFIG_DECLARE in a header when another TU must read the var.
 *
 * CallbackFn may be nullptr -- a var nobody has to react to is the common case.
 *
 * No trailing ';' in the expansion -- these are declarations, not statements,
 * matching NIMBLE_IMPLEMENT_MODULE rather than CORE_LOG. Call sites write the
 * ';' themselves.
 */
#define NIMBLE_CONFIG_BOOL(VarName, SectionLiteral, KeyLiteral, bInDefault, CallbackFn) \
    ::EngineCore::ConfigVar VarName                                                     \
    (                                                                                   \
        ::EngineCore::EConfigType::CT_Bool,                                             \
        SectionLiteral,                                                                 \
        KeyLiteral,                                                                     \
        ::EngineCore::MakeConfigBool(bInDefault),                                       \
        CallbackFn                                                                      \
    )

#define NIMBLE_CONFIG_INT(VarName, SectionLiteral, KeyLiteral, InDefault, CallbackFn)   \
    ::EngineCore::ConfigVar VarName                                                     \
    (                                                                                   \
        ::EngineCore::EConfigType::CT_Int,                                              \
        SectionLiteral,                                                                 \
        KeyLiteral,                                                                     \
        ::EngineCore::MakeConfigInt(InDefault),                                         \
        CallbackFn                                                                      \
    )

#define NIMBLE_CONFIG_FLOAT(VarName, SectionLiteral, KeyLiteral, InDefault, CallbackFn) \
    ::EngineCore::ConfigVar VarName                                                     \
    (                                                                                   \
        ::EngineCore::EConfigType::CT_Float,                                            \
        SectionLiteral,                                                                 \
        KeyLiteral,                                                                     \
        ::EngineCore::MakeConfigFloat(InDefault),                                       \
        CallbackFn                                                                      \
    )

#define NIMBLE_CONFIG_STRING(VarName, SectionLiteral, KeyLiteral, InDefault, CallbackFn) \
    ::EngineCore::ConfigVar VarName                                                      \
    (                                                                                    \
        ::EngineCore::EConfigType::CT_String,                                            \
        SectionLiteral,                                                                  \
        KeyLiteral,                                                                      \
        ::EngineCore::MakeConfigString(InDefault),                                       \
        CallbackFn                                                                       \
    )

/* For a header that must expose a config var declared elsewhere to other
 * translation units -- extern only, never a second definition. */
#define NIMBLE_CONFIG_DECLARE(VarName) extern ::EngineCore::ConfigVar VarName

/*
 * ============== CONFIG HAZARDS -- read before declaring one in a module ====
 *
 * THE DESTRUCTOR IS THE ONLY THING SAVING YOU FROM DANGLING CALLBACKS. Unlike
 *   SetLogHandler and SetFileProvider (see DANGLING CALLBACKS in CoreModule.h),
 *   a config var needs no OnShutdown cleanup: the var IS the registration, and
 *   its destructor runs when its image is unmapped. That guarantee holds only
 *   for a var with STATIC STORAGE DURATION in the module. A heap-allocated
 *   ConfigVar the module forgets to delete, or a module function installed as
 *   the callback of a var owned by the HOST, both leave a pointer into an
 *   unmapped image -- and both crash in the host, nowhere near the module.
 *
 * DO NOT HOLD A ConfigVar* ACROSS A RELOAD. Same rule as IModule*: look it up
 *   with ConfigFindVar per use. A pointer to a module's var dangles the instant
 *   its image unloads, which is why the console's own walk is per-command.
 *
 * A RELOADED MODULE COMES BACK WITH THE FILE'S VALUES, NOT ITS DEFAULTS. The
 *   retained entry store outlives the module, so a var reconstructed on reload
 *   is back-filled from the last-loaded .ini again. Usually what you want. It
 *   also means a value set only by a console command is NOT retained across a
 *   reload -- a console set writes the var, not the retained store.
 *
 * REQUIRES CORE_SHARED. With a static EngineCore every image links its own
 *   list head. A module's vars then never appear in the host's console,
 *   LoadConfigFile silently applies nothing to them, and there is no
 *   diagnostic anywhere -- the same failure CoreModule.h describes for
 *   GLogHandler.
 *
 * THE REGISTRY IS NOT THREAD-SAFE. No mutex, matching the rest of Core's
 *   process-wide seams. Registration happens on the thread that calls
 *   LoadLibrary and unregistration on the thread that calls FreeLibrary, so
 *   every LoadConfigFile, ExecConfigCommand and registry walk must happen on
 *   the thread that ticks ModuleManager. Scalar reads are benign races; a
 *   string read concurrent with a set is not.
 *
 * CALLBACKS FIRE MID-FILE. LoadConfigFile applies keys in file order and fires
 *   each callback as it goes. A callback that reads a SIBLING var is reading a
 *   half-applied file. Read only the var you were handed.
 *
 * A DUPLICATE KEY IS A WARNING AND BOTH VARS ARE SET. If a module and the host
 *   both declare "Render.VSync", every file load and every console set writes
 *   both, so they agree -- until somebody calls SetBool through one directly,
 *   at which point they silently diverge. The registration warning is the only
 *   notice you get.
 *
 * LAYOUT IS FROZEN BY THE ABI VERSION. ConfigVar's inline getters are compiled
 *   into the module and read members at fixed offsets. Adding, removing or
 *   reordering a member here MUST bump MODULE_ABI_REVISION in CoreModule.h.
 * ==========================================================================
 */

#endif //ENGINECORE_CORECONFIG_H
