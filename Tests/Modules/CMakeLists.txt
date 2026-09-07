# ===========================================================================
# Hot-reload test fixtures.
#
# Three builds of one source file, so the smoke test can simulate a rebuild
# without invoking a compiler -- see the comment at the top of
# ExampleHotModule.cpp for why that is necessary.
#
# WHY THIS IS WRITTEN OUT BY HAND RATHER THAN WITH nimble_add_hot_module()
#
# EngineCore is a vendored git subtree of a standalone repository and must keep
# configuring on its own. Reaching for Engine/cmake/NimbleModule.cmake from in
# here would make the engine-level aggregate mandatory for a `cmake -S
# Engine/Modules/Core` configure, which is exactly the property this directory's
# location exists to preserve. This block collapses into three
# nimble_add_hot_module() calls the day NimbleModule.cmake is vendored into
# Core/cmake/ the way GitVersion.cmake already is.
# ===========================================================================

foreach(NM_VARIANT A B STALE)
    set(NM_TARGET ExampleHotModule${NM_VARIANT})

    # MODULE, not SHARED: a hot-reload plugin is never linked against, which is
    # precisely what CMake's MODULE library type means, and it keeps the target
    # out of any accidental target_link_libraries.
    add_library(${NM_TARGET} MODULE ExampleHotModule.cpp)

    # No "lib" prefix, so the filename is identical on every platform and the
    # host's watch path needs no per-platform spelling.
    set_target_properties(${NM_TARGET} PROPERTIES
            PREFIX ""
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    # PUBLIC from EngineCore is where WITH_SIMD, /arch:AVX2 and the global-alias
    # setting arrive. A module must NEVER set those itself: they change
    # sizeof/alignof of the math types, and a mismatch corrupts memory with no
    # compiler diagnostic. See the SIMD block in Core/CMakeLists.txt.
    target_link_libraries(${NM_TARGET} PRIVATE EngineCore::EngineCore)

    target_compile_definitions(${NM_TARGET} PRIVATE
            EXAMPLE_MODULE_VARIANT_STRING="${NM_VARIANT}")

    if(MSVC)
        # Record only the PDB's FILENAME in the DLL rather than an absolute
        # path, so a shadow copy resolves its symbols from the PDB copied
        # alongside it and the debugger never opens -- and so never locks -- the
        # original. Without this, hot reload merely trades a locked DLL for a
        # locked PDB and the next build fails with LNK1201 instead of LNK1104.
        target_link_options(${NM_TARGET} PRIVATE /PDBALTPATH:%_PDB%)
        target_compile_options(${NM_TARGET} PRIVATE /wd4251)
    elseif(UNIX AND NOT APPLE)
        # A shadow copy sits one directory below the real libraries, so
        # $ORIGIN/.. is where libEngineCore.so is under a flat output layout.
        set_target_properties(${NM_TARGET} PROPERTIES BUILD_RPATH "$ORIGIN/..")
    endif()
endforeach()

# The forged stale plugin: the same source, lying about its ABI version through
# the real macro machinery.
target_compile_definitions(ExampleHotModuleSTALE PRIVATE EXAMPLE_MODULE_FORCE_BAD_ABI=1)
