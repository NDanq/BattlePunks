#ifndef SEQUENCER_SEQUENCERAPI_H
#define SEQUENCER_SEQUENCERAPI_H

/*
 * Symbol visibility for Sequencer -- the three states CoreAPI.h documents:
 *
 *   SEQUENCER_SHARED_BUILD undefined         -> SEQUENCER_API is empty (static)
 *   SEQUENCER_SHARED_BUILD + _BUILDING       -> dllexport (building the DLL)
 *   SEQUENCER_SHARED_BUILD alone             -> dllimport (consuming the DLL)
 *
 * Always SHARED (CMakeLists.txt): one asset cache and one SequenceWorld
 * registry per process, as AnimationSystem's.
 */

#if defined(_WIN32)
    #define SEQUENCER_API_EXPORT __declspec(dllexport)
    #define SEQUENCER_API_IMPORT __declspec(dllimport)
#else
    #define SEQUENCER_API_EXPORT __attribute__((visibility("default")))
    #define SEQUENCER_API_IMPORT __attribute__((visibility("default")))
#endif

#ifdef SEQUENCER_SHARED_BUILD
    #ifdef SEQUENCER_BUILDING
        #define SEQUENCER_API SEQUENCER_API_EXPORT
    #else
        #define SEQUENCER_API SEQUENCER_API_IMPORT
    #endif
#else
    #define SEQUENCER_API
#endif

#endif //SEQUENCER_SEQUENCERAPI_H
