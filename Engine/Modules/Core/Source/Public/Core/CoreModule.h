#ifndef ENGINECORE_COREMODULE_H
#define ENGINECORE_COREMODULE_H

#include <cstdint>
#include <cstddef>

#include "Public/Core/CoreAPI.h"

/*
 * The hot-reload module contract: what a swappable DLL must implement, and
 * what the host may assume about it.
 *
 * A module is a shared library that exports four C functions (see below),
 * hands back an IModule, and can be unloaded and replaced while the process
 * runs. CoreModuleManager.h is the other half -- it owns loading, watching,
 * shadow copying and rollback. This header is the part a module author reads.
 *
 * REQUIRES CORE_SHARED=ON. Not a style preference: with a static EngineCore
 * every image gets its own copy of GLogHandler, GFileProvider and
 * ModuleReloadState::Write, so a module's log messages vanish and the reload
 * blob is allocated in one heap and freed in another. None of that produces a
 * diagnostic at build time. nimble_add_hot_module() fails the configure
 * instead; a hand-written module target should too.
 */

namespace EngineCore
{
    /*
     * The durable-state carrier for a hot reload.
     *
     * WHY THE HOST OWNS THE BYTES
     *
     * A module writes its state here in OnShutdown and is then unmapped. If the
     * buffer had been allocated by the module, the host would hold a pointer
     * into a heap whose allocator's CODE no longer exists, and freeing it would
     * be a jump into unmapped memory. So the buffer is allocated, grown and
     * freed exclusively by EngineCore. Nothing a module allocates outlives the
     * module.
     *
     * WHY THE MEMBER FUNCTIONS ARE OUT-OF-LINE (and this class is CORE_API)
     *
     * If Write() were inline in this header, the copy compiled INTO the module
     * would call operator new from the module's image. Under /MD that lands in
     * the same CRT heap and looks fine right up until someone builds a module
     * with /MT, at which point it is a silent cross-heap free. Out-of-line plus
     * dllexport means there is exactly one Write() in the process and it lives
     * in EngineCore.dll.
     *
     * WHAT MAY GO IN THE BLOB
     *
     * Trivially-copyable bytes only. No pointers -- they point into the old
     * image or into a heap the module no longer controls -- no std:: containers,
     * no virtuals, no function pointers. The intended payload is one POD struct
     * with a version constant and a static_assert on its size.
     *
     * WHAT (version, size) CANNOT CATCH -- read before trusting it
     *
     * Read() is an all-or-nothing exact match on both, which catches a changed
     * struct SIZE. It cannot catch a same-size field reorder or type change:
     * {uint32 A; float B;} -> {float B; uint32 A;} passes every check and
     * silently reinterprets. The only defense is a per-module version constant
     * bumped by hand. Nothing enforces it and the failure is silent.
     *
     * LAYOUT IS FROZEN BY THE ABI VERSION
     *
     * The inline getters below read members directly and are compiled into the
     * module. Adding, removing or reordering a member without bumping
     * MODULE_ABI_REVISION produces a module reading the wrong offsets -- the
     * same rule as IModule's vtable, and easier to forget because this one is
     * not an interface.
     */
    class CORE_API ModuleReloadState
    {
    public:
        ModuleReloadState() = default;
        ~ModuleReloadState();

        /* Copying would duplicate ownership of a buffer only EngineCore may
         * free. Moving just hands the pointer over, which is what lets a
         * ModuleRecord live in a std::vector -- and it is out-of-line for the
         * same reason Write() is: move-assignment frees the destination's
         * buffer, and that free has to happen in EngineCore.dll. */
        ModuleReloadState(const ModuleReloadState&)            = delete;
        ModuleReloadState& operator=(const ModuleReloadState&) = delete;

        ModuleReloadState(ModuleReloadState&& InOther) noexcept;
        ModuleReloadState& operator=(ModuleReloadState&& InOther) noexcept;

        /* Replaces any previous payload. False if the allocation failed. A
         * zero-byte write is representable and distinguishable from never
         * having written: IsEmpty() stays true, but GetStateVersion() does not,
         * which is what lets a stateless module still say "I ran". */
        bool Write(uint32_t InStateVersion, const void* InData, size_t InBytes);

        /* Copies the payload out only if BOTH the version and the byte count
         * match exactly. On any mismatch it returns false and leaves OutDst
         * untouched -- a reloaded module that changed its state layout gets a
         * clean cold start rather than a reinterpretation of the old bytes. */
        bool Read(uint32_t InExpectedVersion, void* OutDst, size_t InBytes) const;

        bool        IsEmpty()         const { return SizeBytes == 0; }
        uint32_t    GetStateVersion() const { return StateVersion; }
        size_t      GetSizeBytes()    const { return SizeBytes; }
        const void* GetData()         const { return Data; }

        /* Forces the next load to be a cold start -- an editor "reload clean". */
        void Reset();

    private:
        uint32_t StateVersion = 0;
        size_t   SizeBytes    = 0;
        size_t   Capacity     = 0;
        uint8_t* Data         = nullptr;
    };

    /*
     * DELIBERATELY NOT CORE_API -- do not "fix" this.
     *
     * Every other interface in EngineCore (IFileProvider) is exported because
     * EngineCore both declares AND implements it. IModule is the opposite: the
     * derived class lives in a module DLL, and the vtable is emitted there.
     * Marking this dllimport would make MSVC look for IModule's vtable and
     * type_info inside EngineCore.dll, where -- with every member inline and
     * pure -- nothing emits them. The result is an unresolved external at
     * module link time, or worse, two disagreeing vtables.
     *
     * Consequence: IModule's layout is frozen by NIMBLE_MODULE_ABI_VERSION and
     * by nothing else. Adding, removing or REORDERING a virtual here changes
     * the vtable and MUST bump MODULE_ABI_REVISION, or a stale plugin will call
     * the wrong slot and the failure will look like anything but a vtable
     * mismatch.
     */
    class IModule
    {
    public:
        virtual ~IModule() = default;

        /* Stable identity, used for logging and lookup. Must not allocate. */
        virtual const char* GetName() const = 0;

        /*
         * Free-form build identity: a git sha, a variant letter, anything the
         * module wants. The host only ever logs it and compares it for
         * inequality, which is what makes "did the reload actually take?"
         * answerable from outside the module.
         */
        virtual const char* GetVersionString() const = 0;

        /*
         * InPrev is nullptr on a cold start and non-null after a reload -- AND
         * after a rollback, which means this must tolerate a blob written by
         * its OWN OnShutdown.
         *
         * Return false to abort the load. The host treats that exactly like an
         * ABI mismatch: the instance is destroyed, the library closed, and the
         * previous generation rolled back if there was one. Use it for an init
         * failure the host cannot see for itself -- a missing asset, a refused
         * device, a blob this build chose not to trust.
         *
         * Anything registered into a host registry here must be unregistered in
         * OnShutdown. See RELOAD HAZARDS below.
         */
        virtual bool OnStartup(const ModuleReloadState* InPrev) = 0;

        /*
         * Write durable state into OutState, unregister everything, and drop
         * every pointer the host might still hold. After this returns, the
         * image is unmapped.
         */
        virtual void OnShutdown(ModuleReloadState& OutState) = 0;

        virtual void OnTick(float InDeltaSeconds) {}
    };

    /*
     * ===================== RELOAD HAZARDS -- read before writing a module ====
     *
     * DANGLING VTABLES. Every object a module constructed carries a vtable
     *   pointer into the module image. After the library is closed that page is
     *   unmapped and the FIRST virtual call jumps into nothing. Rule: a module
     *   hands out no object that outlives OnShutdown, and the host never stores
     *   an IModule* in a member -- ModuleManager::GetModule is a per-use lookup
     *   and its result is invalid the moment Tick() is entered.
     *
     * DANGLING CALLBACKS. A module calling SetLogHandler(&MyHandler) or
     *   SetFileProvider(&MyProvider) in OnStartup leaves GLogHandler or
     *   GFileProvider pointing into unmapped memory after unload -- and because
     *   CORE_SHARED makes those globals genuinely process-wide, the crash lands
     *   in the HOST's next log call, nowhere near the module. Rule: whatever
     *   OnStartup registers, OnShutdown unregisters. The manager cannot enforce
     *   this, but it does detect the two registries Core owns and warns.
     *
     * DANGLING QUEUED WORK. Anything a module pushed into a host-held
     *   ThreadSafeQueue<T> where T points at a module-defined type is the same
     *   dangling vtable, deferred. Rule: drain module-owned queues in
     *   OnShutdown.
     *
     * STATICS ARE DESTROYED. Function-local statics, file-scope globals and any
     *   self-registration list inside the module are gone and rebuilt from
     *   scratch. Anything not written into ModuleReloadState did not survive. A
     *   host-side index into a module's registration list must be rebuilt in
     *   OnStartup.
     *
     * EXCEPTIONS AND RTTI DO NOT CROSS. A throw from module code caught in the
     *   host requires an identical toolchain and, on MSVC, the same CRT DLL.
     *   dynamic_cast and typeid on module-defined types are unreliable across
     *   the boundary and outright broken under the RTLD_LOCAL the POSIX seam
     *   deliberately uses. Rule: no exception escapes OnStartup, OnShutdown or
     *   OnTick, and the host does not dynamic_cast an IModule*. Everything
     *   crosses as an IModule* or as bytes; a module cannot expose a type
     *   hierarchy to the host.
     *
     * TOOLCHAIN AND CRT MUST MATCH. Same compiler, same /MD[d], same
     *   _ITERATOR_DEBUG_LEVEL, same WITH_SIMD, same CORE_NO_GLOBAL_ALIASES. The
     *   flags folded into NIMBLE_MODULE_ABI_VERSION catch the last three and
     *   the debug/release split; they do NOT catch a different MSVC toolset at
     *   the same configuration, and nothing can.
     *
     * A DEPENDENCY CHANGE IS NOT A RELOAD. The manager watches a module's own
     *   file. Rebuilding EngineCore.dll does not retrigger the modules that
     *   link it, and cannot -- the host has it loaded. Any change to a Core
     *   public header is a full-restart change. The watcher is not broken.
     *
     * NO CRASH ISOLATION. A module that faults inside OnShutdown takes the
     *   process with it. There is no SEH or signal guard here.
     * ========================================================================
     */

    /*
     * The load gate. The host refuses any DLL whose reported value differs.
     *
     * The low 16 bits are a hand-bumped interface revision. The high bits are
     * BUILD CONFIGURATION FLAGS, and they are the important half.
     *
     * WHY THE FLAGS EXIST: Core/CMakeLists.txt already warns that a WITH_SIMD
     * mismatch changes sizeof/alignof of Vector2/3/4 and Matrix4x4 and corrupts
     * memory with no compiler diagnostic at all. Until now that could only
     * happen if someone hand-edited a link line. Hot reload makes it reachable
     * at RUNTIME, by dropping a DLL built from a differently-configured tree
     * into a watched directory. Folding the flags into the version turns silent
     * corruption into a refused load and a log line.
     *
     * Each module compiles this constant from ITS OWN macro state -- which is
     * exactly the state that determines its struct layouts -- so the comparison
     * is self-verifying. Never compute it from anything a module could get
     * wrong by hand.
     */
    enum : uint32_t
    {
        MODULE_ABI_REVISION     = 1,

        MODULE_ABI_FLAG_SIMD    = 1u << 16,   // WITH_SIMD              -> Vector/Matrix layout
        MODULE_ABI_FLAG_NOALIAS = 1u << 17,   // CORE_NO_GLOBAL_ALIASES -> which names exist globally
        MODULE_ABI_FLAG_DEBUG   = 1u << 18,   // _DEBUG                 -> MSVC _ITERATOR_DEBUG_LEVEL, CRT
        MODULE_ABI_FLAG_PTR64   = 1u << 19    // 64-bit build
    };

    constexpr uint32_t NIMBLE_MODULE_ABI_VERSION =
          uint32_t(MODULE_ABI_REVISION)
#if defined(WITH_SIMD)
        | uint32_t(MODULE_ABI_FLAG_SIMD)
#endif
#if defined(CORE_NO_GLOBAL_ALIASES)
        | uint32_t(MODULE_ABI_FLAG_NOALIAS)
#endif
#if defined(_DEBUG)
        | uint32_t(MODULE_ABI_FLAG_DEBUG)
#endif
        | (sizeof(void*) == 8 ? uint32_t(MODULE_ABI_FLAG_PTR64) : 0u);

    /*
     * The four exported symbol names.
     *
     * THESE NAMES ARE PERMANENT ABI. Never rename them; version through the
     * integer NimbleModuleGetABIVersion returns. If a name ever changed, an old
     * DLL would fail the symbol lookup instead of failing the version check,
     * and the error message would become "symbol not found" instead of
     * "ABI 1 vs 3".
     *
     * ORDER IS LOAD-BEARING: the version is resolved and compared BEFORE
     * NimbleModuleCreate is called, and before ANY virtual on the returned
     * object is invoked -- including GetName(). A stale plugin's IModule layout
     * is precisely what is untrustworthy, so calling a virtual on it to find
     * out what it is defeats the check. That is why NimbleModuleGetName exists
     * as a free function: it is the one piece of identity that is safe to
     * obtain from a DLL you have already decided to reject.
     */
    constexpr const char* MODULE_SYMBOL_ABI_VERSION = "NimbleModuleGetABIVersion";
    constexpr const char* MODULE_SYMBOL_NAME        = "NimbleModuleGetName";
    constexpr const char* MODULE_SYMBOL_CREATE      = "NimbleModuleCreate";
    constexpr const char* MODULE_SYMBOL_DESTROY     = "NimbleModuleDestroy";

    using ModuleGetABIVersionFn = uint32_t    (*)();
    using ModuleGetNameFn       = const char* (*)();
    using ModuleCreateFn        = IModule*    (*)();
    using ModuleDestroyFn       = void        (*)(IModule*);

    enum class EModuleState : uint8_t
    {
        MS_Unregistered = 0,   // returned for a name the manager does not know
        MS_Unloaded,           // registered, no library open
        MS_Loaded,             // open, instance live
        MS_Failed              // last load or reload failed; see the log
    };
}

/*
 * NIMBLE_MODULE_API is ALWAYS export, and deliberately does not follow
 * CoreAPI.h's three-state pattern. A module DLL is never linked against at
 * compile time by anybody -- the host reaches it only through appDLLGetSymbol
 * -- so there is no consumer side and therefore no import state to model.
 */
#if defined(_WIN32)
    #define NIMBLE_MODULE_API __declspec(dllexport)
#else
    #define NIMBLE_MODULE_API __attribute__((visibility("default")))
#endif

/*
 * Emits the four exported entry points.
 *
 * MSVC DECORATION HAZARD: extern "C" on x64 produces undecorated names, so
 * GetProcAddress("NimbleModuleCreate") works with no .def file. NEVER declare
 * these __stdcall -- on x86 that decorates to _NimbleModuleCreate@0 and every
 * lookup silently fails with an error that says nothing about calling
 * conventions.
 *
 * Destroy is exported rather than letting the host call delete so that
 * allocation and deallocation stay in the same image, which survives a module
 * built with /MT. The ordering (Destroy strictly before the library is closed)
 * means it would also happen to work without the export -- but only by
 * accident, and the accident stops holding the moment somebody changes the CRT
 * linkage.
 *
 * The _VERSIONED form exists so a test can forge a stale plugin through the
 * real macro machinery rather than hand-writing four extern "C" bodies that
 * could drift away from these. Modules use NIMBLE_IMPLEMENT_MODULE.
 */
#define NIMBLE_IMPLEMENT_MODULE_VERSIONED(ClassName, ModuleNameLiteral, AbiExpr)           \
    extern "C" NIMBLE_MODULE_API uint32_t NimbleModuleGetABIVersion()                      \
    {                                                                                     \
        return (AbiExpr);                                                                 \
    }                                                                                     \
    extern "C" NIMBLE_MODULE_API const char* NimbleModuleGetName()                        \
    {                                                                                     \
        return ModuleNameLiteral;                                                         \
    }                                                                                     \
    extern "C" NIMBLE_MODULE_API ::EngineCore::IModule* NimbleModuleCreate()              \
    {                                                                                     \
        return new ClassName();                                                           \
    }                                                                                     \
    extern "C" NIMBLE_MODULE_API void NimbleModuleDestroy(::EngineCore::IModule* InModule) \
    {                                                                                     \
        delete InModule;                                                                  \
    }

/* Exactly one per DLL, at file scope, outside any namespace. */
#define NIMBLE_IMPLEMENT_MODULE(ClassName, ModuleNameLiteral) \
    NIMBLE_IMPLEMENT_MODULE_VERSIONED(ClassName, ModuleNameLiteral, ::EngineCore::NIMBLE_MODULE_ABI_VERSION)

#endif //ENGINECORE_COREMODULE_H
