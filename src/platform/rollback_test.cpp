// RECHARGED_ROLLBACK_TEST=<file> rolls matches back while they are played,
// offline and in network matches, to test the rollback before it predicts
// anything: after every fixed step it saves the match (platform/rollback_snapshot.h),
// and every RECHARGED_ROLLBACK_TEST_INTERVAL steps (default 1) it goes back 1
// to RECHARGED_ROLLBACK_TEST_FRAMES steps (default 8, in turn) and runs them
// again with the inputs they first had (tweak patch 0032). It goes back at
// the start of the next frame, before any task sets up its cameras and views,
// and replays through the end of the last step's frame, frames without steps
// as one with the next (RECHARGED_ROLLBACK_TEST_MERGE=0: one by one). The
// game's checksum after each replayed step must be the one the step first
// gave; in a network match the machines also compare their checksums as the
// original game does.
// <file> logs the session; <file>.summary, rewritten every ten seconds, holds
// its counts and costs. Unset, it changes nothing. Diagnostic only.
//
// RECHARGED_ROLLBACK_TEST_COMPARE=N checks every Nth rollback: it copies the
// memory the rollback tracks before the restore and compares it after the
// replay, when it should be as it was. It also watches what no rollback
// restores (the module's other globals, what the test leaves out of the
// arenas), which a replay should not change. <file> lists the places that
// differed in the first comparisons, <file>.summary every place with how
// often: state the replay does not bring back as it was, or moves forward a
// second time.
//
// The rollback restores the original game's arenas and globals, as the sync
// test saves them, except what lives on outside the simulation: the network
// session (but its players' inputs), sockets and lobby, the input routers and
// manager, the pads, the tasks' clocks and the fixed update task's
// accumulated time, the frame memory (used afresh every frame) and the audio
// memory. Everything else, the drawing included, goes back with the match,
// with the native pool records the game points at (their own heap) and the
// registry of the game's memory (its rollback checkpoint).
//
// A replay runs whole frames again, as the task manager ran them, each task
// with the time step it had and the frame rate option's state each frame
// started with: the drawing recomputes its own state with the match's (with
// nowhere to draw, GLView drops it), and the frame's end empties the views
// and the frame memory without sending anything. The pads, sound, network,
// disc and movies stay out, and the fixed update runs the recorded steps: no
// pads, no capture, nothing sent, no deterministic data queued (it went out
// the first time). The mega strike meter's presses, which come from the
// shooter's machine outside the inputs, go again to the steps that received
// them.
//
// The game makes no sounds at all while the test runs, menus included: audio
// is not part of the rollback yet, and the audio code's slot pools keep their
// free lists in globals the rollback leaves alone but their blocks in the
// arenas it restores. A rollback is skipped when a disc read was in flight or
// started, or the game or task state changed, in the steps it would undo
// (their completion, presentation and loading went into memory it restores).
// Tracking stops when no step comes for a quarter of a second (pause, end of
// match).
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The rollback test belongs to the isolated original game module
#endif
#include "mscharged_rollback.h"

#if defined(_WIN32)
extern "C" bool mscharged_rollback_active(void) { return false; }
extern "C" bool mscharged_rollback_resimulating(void) { return false; }
extern "C" void mscharged_rollback_inputs_applied(const MschargedRollbackInputs*) {}
extern "C" void mscharged_rollback_meter_received(const bool*, int) {}
extern "C" void mscharged_rollback_task_time(const void*, float) {}
extern "C" void mscharged_rollback_between_steps(int (*)(const MschargedRollbackInputs*), unsigned (*)(void), void*,
                                                 unsigned long)
{
}
extern "C" void mscharged_rollback_frame_start(void) {}
extern "C" bool mscharged_rollback_predicting(void) { return false; }
extern "C" int mscharged_rollback_network_reset(void) { return 4; }
extern "C" bool mscharged_rollback_has_input(void) { return true; }
extern "C" void mscharged_rollback_input_ready(void) {}
extern "C" bool mscharged_rollback_can_capture(void) { return true; }
extern "C" int mscharged_rollback_sync_data(unsigned*, unsigned*, unsigned*) { return 0; }
#else
#include "Game/Audio/AudioBackend.h"
#include "Game/Game.h"
#include "Game/InputManager.h"
#include "Game/InputRouter.h"
#include "Game/NetworkInput.h"
#include "Game/NetworkInputMessages.h"
#include "Game/NetworkPeer.h"
#include "Game/NetworkRandomSeed.h"
#include "Game/NetworkSession.h"
#include "Game/NetworkSync.h"
#include "Game/Task/GameTaskState.h"
#include "Game/Effects/EffectsTemplate.h"
#include "Game/Task/DispatchEventsTask.h"
#include "Game/Task/FixedUpdateTask.h"
#include "NL/MemAlloc.h"
#include "NL/gl/glView.h"
#include "NL/globalpad.h"
#include "NL/nlMemory.h"
#include "NL/nlSlotPool.h"
#include "NL/nlTask.h"
#include "NL/platpad.h"
#include "platform/game_allocation_ownership.h"
#include "platform/game_resource_records.h"
#include "platform/rollback_snapshot.h"
#include "platform/tweak_frame_state.h"

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <link.h>
#include <execinfo.h>
#include <iterator>
#include <typeinfo>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

// Bounds of the simulation's globals (cmake/rollback_sections.ld).
extern "C" char __rbk_data_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_data_end[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_end[] __attribute__((weak, visibility("hidden")));
// The routers (InputRouter.cpp).
extern InputRouter* gSimpleInputRouter;
extern NetworkInputRouter* gNetworkInputRouter;
// The network transport's slot pools (NL/plat/Transport*.cpp), by their common
// base: a variable's symbol is its name, and SlotPool<NetworkMessageStorage>
// has no header.
extern SlotPoolBase gTransportPacketPool;
extern SlotPoolBase gNetworkMessagePool;
extern SlotPoolBase gTransportConnectionPool;

// No std containers: in the game module the global operator new is the
// original nlMalloc, and the test must not allocate in the arenas it restores.
namespace
{
constexpr int kMaxDepth = 8;
constexpr int kRing = kMaxDepth + 2;     // steps kept: the deepest rollback and the step it starts from
constexpr int kMaxRanges = 16;
constexpr int kMaxExcluded = 160;
constexpr int kMaxKept = 16;             // ranges inside excluded ones that are restored after all
constexpr int kMaxRegions = 256;
constexpr std::uint64_t kIdleNs = 250'000'000;
constexpr int kSummaryEvery = 120;       // steps (two seconds)
constexpr int kUntrackedEvery = 600;     // steps between checks for writes behind the tracking (reads everything)
constexpr int kListedMismatches = 20;
constexpr int kMaxMeter = 64;            // mega strike meter presses received before one step
constexpr int kMaxWatched = 256;         // ranges the comparison watches although no rollback restores them
constexpr int kMaxPlaces = 4096;         // places the comparison tells apart
constexpr int kDetailedCompares = 3;     // comparisons listed in full in the log
constexpr int kListedPlaces = 60;        // places per detailed comparison, and in the summary
constexpr int kMaxTasks = 32;            // tasks the task manager runs
constexpr float kLongestFrame = 0.1f;    // the task manager's bound on a frame (g_fTaskTimeUpperBound)
constexpr int kFrameRing = 256;          // frames kept: those of the deepest rollback, at any frame rate
constexpr int kMachines = 4;
constexpr int kMachineEvents = 4;        // deterministic data events in one input message
constexpr int kInputRing = 32;           // RECHARGED_ROLLBACK: steps whose inputs are kept
constexpr int kConfirmedRing = 64;       // RECHARGED_ROLLBACK: checksums of steps whose inputs all arrived
constexpr int kRemoteChecks = 32;        // RECHARGED_ROLLBACK: other machines' checksums waiting for ours

// What a replayed frame does with each task: run it as it ran, leave it out
// (pads, sound, network, disc, movies, saving), or its replay part.
enum TaskReplay
{
    kSkip,
    kRun,
    kFixed, // its steps, after its work before them (RollbackReplayFixedFrame)
    kEnd    // the frame's end without sending it (RollbackReplayEndFrame)
};
constexpr const char* kRunTasks[] = {"Begin Frame",          "World",     "Game Render",    "Particle Update",
                                     "NIS Transition Wiper", "Front End", "Dispatch Events"};

// One frame as the task manager ran it: each task's time step and real time,
// < 0 for a task that did not run, and the frame rate option's state at its
// start.
struct FrameRecord
{
    float dt[kMaxTasks];
    float real[kMaxTasks];
    MschargedTweakFrameState tweak;
    // The fixed update's time left over after the frame's steps, which the
    // drawing interpolates with (left out of the rollback, as the clocks).
    float accumulated, interpolation;
};

struct Range
{
    std::uintptr_t start, end;
    const char* name;
};
// What a rollback must not cross: the game's own state and the task manager's.
struct Phases
{
    int game;
    unsigned current, pending;
    bool operator==(const Phases&) const = default;
};
struct Step
{
    unsigned frame;
    std::uint64_t snapshot;
    mscharged::platform::GameAllocationMark mark;
    MschargedRollbackInputs inputs; // what the router applied before it
    unsigned checksum;              // after it, as it first ran (in a network match, as it ran last)
    unsigned seed;                  // the network random seed after it
    Phases phases;                  // after it
    bool reading;                   // a disc read in flight after it
    std::uint64_t readsStarted;     // disc reads started until then
    std::uint64_t hostFrame;        // the frame it ran in
    // The mega strike meter's presses received from the shooter's machine
    // before it, outside the router's inputs.
    bool meter[kMaxMeter];
    int meterCount;
    bool meterOverflowed;
};

// RECHARGED_ROLLBACK: one machine's inputs for one step, as its message gave
// them (each message holds what changed since the machine's previous one) or
// as predicted: its last inputs that arrived, without events.
struct MachineInput
{
    unsigned char players;
    unsigned short remap;
    unsigned char records[4][16];   // PackedDetInput, by the machine's player
    unsigned char states[4];
    int eventCount;
    unsigned char events[kMachineEvents][0x24]; // DetermDataEvent
};
struct StepInputs
{
    unsigned frame; // the step they are for; another value: none kept
    int machineCount;
    MachineInput machines[kMachines];
    bool predicted[kMachines];
};
struct SyncPoint
{
    unsigned frame, checksum, seed;
    int machine;
};

// RECHARGED_ROLLBACK_TEST_COMPARE: a place whose bytes after a replay were not
// those before the restore. A global by its module offset, an allocation by
// its first word's (a vtable names the class) and its size, or a range with
// no live allocation.
enum PlaceKind
{
    kGlobal = 1,
    kAllocation,
    kUnowned,
    kNotRestored // a replay changed it, and nothing takes it back
};
struct Place
{
    int kind;
    std::uintptr_t key;
    std::size_t size;
    std::uint64_t compares, bytes; // comparisons it differed in, bytes that differed in all
    std::uintptr_t firstOffset;    // in the allocation, the first time
    std::uint64_t lastCompare;
    const char* region;
    std::uint8_t before[16], after[16];
};

struct State
{
    FILE* out;
    char* summaryPath;
    int maxDepth;
    int interval;

    Range ranges[kMaxRanges];
    int rangeCount;
    Range excluded[kMaxExcluded];
    int excludedCount;
    Range kept[kMaxKept];
    int keptCount;
    mscharged::platform::RollbackRegion regions[kMaxRegions];
    int regionCount;

    bool active;
    bool resimulating;
    bool haveInputs;
    MschargedRollbackInputs pending; // reported for the step under way
    bool received[kMaxMeter];        // meter presses received since the last step
    int receivedCount;
    bool receivedOverflowed;
    nlTask* tasks[kMaxTasks];       // in the task manager's order
    TaskReplay replays[kMaxTasks];
    int taskCount;
    FrameRecord frames[kFrameRing];
    std::uint64_t hostFrame;        // frames since tracking started, the one under way
    // The random seeds the game's presentation draws from, as they were before
    // a task a replay leaves out (the audio's sounds and the transitions also
    // draw from the default one): put back after it, so that the first run
    // draws what the replay does.
    bool seedsSaved;
    unsigned int defaultSeed, effectsSeed;
    Step steps[kRing];
    unsigned latest;  // frame of the last step saved
    int stored;       // consecutive steps saved since tracking started
    int nextDepth;
    int pendingDepth;            // steps the next frame start goes back, 0: none
    int (*replay)(const MschargedRollbackInputs*);
    unsigned (*checksum)(void);
    std::uint64_t lastHookNs;

    // Counts and costs.
    int starts;
    std::uint64_t stepCount, rollbacks, byDepth[kMaxDepth + 1], replayed;
    std::uint64_t skippedReads, skippedStarts, skippedPhases, skippedEvents, skippedShort;
    std::uint64_t meterReceived, meterRedelivered, framesReplayed, framesMerged;
    bool merge; // frames without steps replayed as one with the next

    // RECHARGED_ROLLBACK_TEST_COMPARE=N: every Nth rollback, the tracked memory
    // after the replay against a copy taken before the restore.
    int compareEvery;
    std::uint8_t* compareCopy;
    std::size_t compareBytes;
    std::uint64_t compares, comparesDiffering, compareDiffBytes, compareRangesDropped;
    std::uintptr_t moduleBase;
    Range watched[kMaxWatched];
    int watchedCount;
    Place* places;
    int placeCount;
    std::uint64_t placesDropped;
    std::uint64_t mismatches, nisInReplay, lossyFrees, failedRestores, untracked;
    unsigned firstMismatchFrame;
    std::uint64_t rollbackNs, maxRollbackNs, restoreNs, maxRestoreNs;
    // Where the replays' time goes: each task's, and in the fixed update's the
    // steps' and the saves'.
    std::uint64_t taskNs[kMaxTasks], stepNs, saveNs;

    // RECHARGED_ROLLBACK: network matches that predict.
    bool netplay;
    int inputDelay;
    StepInputs inputs[kInputRing];
    unsigned lastInputFrame;          // the last step whose inputs were applied
    unsigned firstPredicted[kMachines]; // by machine: its first step still predicted, 0: none
    bool haveLastReal;
    MachineInput lastReal[kMachines]; // by machine: its last inputs that arrived
    unsigned waitThrough;             // steps through this one wait for all their inputs (they were undone)
    unsigned undoFrom;                // a predicted step changed the match's phase: undone at the frame start
    unsigned replayEnd;               // the last step a replay runs
    bool captured;
    unsigned capturedFrame;           // the local inputs were captured through this frame
    bool haveConfirmed;
    unsigned confirmed;               // the last step whose inputs all arrived
    SyncPoint history[kConfirmedRing];
    SyncPoint remote[kRemoteChecks];
    int remoteCount;
    std::uint64_t predictedSteps, lockstepWaits, windowWaits, mispredictions[kMachines], correctionRollbacks;
    std::uint64_t undone, truncated, impossible, desyncs, remoteChecks, predictedDepth[kMaxDepth + 1];
    unsigned firstDesyncFrame;
    // The time sync: by machine, how far ahead its inputs come here (messages
    // queued, less steps predicted; smoothed), and the same as it measures
    // ours. Half the difference is how far this machine runs ahead of it.
    float queuedAhead[kMachines], reportedAhead[kMachines];
    bool haveReported[kMachines];
    float lead;       // steps this machine runs ahead of the furthest behind
    bool slowing;
    double slowedSeconds, leadSum, maxLead;
    std::uint64_t leadSamples;
};
State* sState;
std::mutex sMutex; // the hooks (main thread) and the watchdog

std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

int EnvInt(const char* name, int fallback, int low, int high)
{
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    return std::clamp(std::atoi(text), low, high);
}

void AddRange(const void* base, std::size_t bytes, const char* name)
{
    State& s = *sState;
    if (!base || !bytes || s.rangeCount == kMaxRanges) return;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    for (int i = 0; i < s.rangeCount; ++i)
        if (start >= s.ranges[i].start && start + bytes <= s.ranges[i].end) return; // inside one already
    s.ranges[s.rangeCount++] = {start, start + bytes, name};
}

void Exclude(const void* base, std::size_t bytes, const char* name)
{
    State& s = *sState;
    if (!base || !bytes) return;
    if (s.excludedCount == kMaxExcluded)
    {
        std::fprintf(s.out, "rollback test: too many exclusions, %s not excluded\n", name);
        return;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    s.excluded[s.excludedCount++] = {start, start + bytes, name};
}

// The whole original allocation holding pointer (bytes when it has none).
void ExcludeAllocation(const void* pointer, std::size_t bytes, const char* name)
{
    if (!pointer) return;
    mscharged::platform::GameAllocationSpan span{};
    if (mscharged::platform::FindGameAllocationSpan(pointer, 1, span))
        Exclude(span.base, span.bytes, name);
    else
        Exclude(pointer, bytes, name);
}

void Keep(const void* base, std::size_t bytes)
{
    State& s = *sState;
    if (!base || !bytes || s.keptCount == kMaxKept) return;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    s.kept[s.keptCount++] = {start, start + bytes, "kept"};
}

bool Excluded(std::uintptr_t address)
{
    const State& s = *sState;
    for (int i = 0; i < s.keptCount; ++i)
        if (address >= s.kept[i].start && address < s.kept[i].end) return false;
    for (int i = 0; i < s.excludedCount; ++i)
        if (address >= s.excluded[i].start && address < s.excluded[i].end) return true;
    return false;
}

// What the rollback restores, and what lives on outside the simulation.
void FindRanges(FixedUpdateTask* task, unsigned long taskBytes)
{
    State& s = *sState;
    s.rangeCount = s.excludedCount = s.keptCount = 0;
    if (__rbk_data_start && +__rbk_data_end > +__rbk_data_start)
        AddRange(__rbk_data_start, __rbk_data_end - __rbk_data_start, "game data");
    if (__rbk_bss_start && +__rbk_bss_end > +__rbk_bss_start)
        AddRange(__rbk_bss_start, __rbk_bss_end - __rbk_bss_start, "game bss");
    AddRange(StandardAllocator.m_memory, StandardAllocator.m_memory_size, "MEM1");
    AddRange(VirtualAllocator.m_memory, VirtualAllocator.m_memory_size, "MEM2");
    AddRange(&StandardAllocator, sizeof(StandardAllocator), "StandardAllocator");
    AddRange(&VirtualAllocator, sizeof(VirtualAllocator), "VirtualAllocator");
    AddRange(&CurrentAllocator, sizeof(CurrentAllocator), "CurrentAllocator");
    AddRange(AllocatorStack, sizeof(AllocatorStack), "AllocatorStack");
    AddRange(&AllocatorStackDepth, sizeof(AllocatorStackDepth), "AllocatorStackDepth");
    AddRange(task, taskBytes, "fixedUpdateTask");
    // Native representations of pool records (markers, texture headers) the game points at.
    const void* records = nullptr;
    std::size_t recordBytes = 0;
    mscharged::platform::GameResourceRecordStorage(&records, &recordBytes);
    AddRange(records, recordBytes, "pool records");

    // Audio: the host plays what it describes.
    if (g_pAudioBackend)
    {
        Exclude(g_pAudioBackend->m_AudioAllocator.m_memory, g_pAudioBackend->m_AudioAllocator.m_memory_size,
                "audio memory");
        Exclude(g_pAudioBackend, sizeof(AudioBackend), "AudioBackend");
    }
    // The network: the session goes on receiving, sending and pausing. Its
    // players' inputs (applied from the router before every step) are match state.
    if (NetworkSession* session = g_pNetworkSession)
    {
        ExcludeAllocation(session, sizeof(NetworkSession), "NetworkSession");
        for (int machine = 0; machine < 4; ++machine)
            for (int channel = 0; channel < 4; ++channel)
            {
                NetworkPeerChannel* peerChannel = session->GetPeer(machine)->GetNetworkPeerChannel(channel);
                Keep(&peerChannel->mPrevInput, sizeof(DetInput));
                Keep(&peerChannel->mInput, sizeof(DetInput));
            }
        ExcludeAllocation(session->mDirectSocket, 0, "NetworkSocket");
        ExcludeAllocation(session->mTransport, 0, "LANLobby");
        ExcludeAllocation(session->mLobby, 0, "NetworkLobby");
        ExcludeAllocation(session->mStatsReporter, 0, "NetworkStatsReporter");
        ExcludeAllocation(session->mRankingReporter, 0, "NetworkRanking");
    }
    // The transport's slot pools keep their free lists in its globals, left out
    // with the network, and their slots in blocks of the arenas: the blocks go too.
    for (SlotPoolBase* pool : {&gTransportPacketPool, &gNetworkMessagePool, &gTransportConnectionPool})
        for (SlotPoolBlock* block = pool->m_BlockList; block; block = block->next)
            ExcludeAllocation(block, 0, "network transport pool");
    ExcludeAllocation(gSimpleInputRouter, 0, "SimpleInputRouter");
    ExcludeAllocation(gNetworkInputRouter, 0, "NetworkInputRouter");
    ExcludeAllocation(gInputManager, 0, "NetworkInputManager");
    ExcludeAllocation(gNetworkSyncState, 0, "NetworkSyncState");
    // The pads: read again only for new steps.
    if (PadManager* pads = g_pPadManager)
    {
        ExcludeAllocation(pads, sizeof(PadManager), "PadManager");
        if (pads->m_aPads)
        {
            ExcludeAllocation(pads->m_aPads, 0, "PadManager pads");
            for (int i = 0; i < pads->mPadCount * pads->mPadSetCount; ++i)
                if (cGlobalPad* pad = pads->m_aPads[i])
                {
                    ExcludeAllocation(pad, 0, "cGlobalPad");
                    ExcludeAllocation(pad->mBackend, 0, "PadBackend");
                }
        }
    }
    ExcludeAllocation(g_pPlatPadManager, 0, "PlatPadManager");
    // The frame memory: used afresh every frame, with its cursors in glxMemory's statics.
    const void* frame[2];
    const int frameCount = mscharged_rollback_frame_memory(frame, 2);
    for (int i = 0; i < frameCount; ++i) ExcludeAllocation(frame[i], 0, "frame memory");
    // The clocks: each task's last run, and the time the fixed steps have still to catch up.
    if (nlTaskManager* manager = nlTaskManager::m_pInstance; manager && manager->mTaskList)
    {
        nlTask* first = manager->mTaskList;
        nlTask* t = first;
        do
        {
            Exclude(&t->mPreviousTicker, sizeof(t->mPreviousTicker), "task clock");
            Exclude(&t->mExecutionTime, sizeof(t->mExecutionTime), "task execution time");
            t = t->m_next;
        } while (t && t != first);
    }
    Exclude(&task->mAccumulatedDeltaT, sizeof(task->mAccumulatedDeltaT), "fixed steps' accumulated time");
    Exclude(&task->mInterpolationDeltaT, sizeof(task->mInterpolationDeltaT), "fixed steps' interpolation time");
}

// The snapshot regions: the ranges without the excluded ones, and the kept
// ones inside those. False when they do not fit.
bool BuildRegions()
{
    State& s = *sState;
    s.regionCount = 0;
    Range cuts[kMaxExcluded];
    std::copy(s.excluded, s.excluded + s.excludedCount, cuts);
    std::sort(cuts, cuts + s.excludedCount, [](const Range& a, const Range& b) { return a.start < b.start; });
    auto add = [&](std::uintptr_t start, std::uintptr_t end) {
        if (start >= end) return true;
        if (s.regionCount == kMaxRegions) return false;
        s.regions[s.regionCount++] = {reinterpret_cast<void*>(start), end - start};
        return true;
    };
    for (int r = 0; r < s.rangeCount; ++r)
    {
        std::uintptr_t at = s.ranges[r].start;
        for (int c = 0; c < s.excludedCount; ++c)
        {
            if (cuts[c].end <= at || cuts[c].start >= s.ranges[r].end) continue;
            if (!add(at, std::max(at, cuts[c].start))) return false;
            at = std::max(at, cuts[c].end);
        }
        if (!add(at, s.ranges[r].end)) return false;
    }
    // Kept ranges lie inside excluded ones, so no region overlaps them;
    // each is restored only where a saved range holds it.
    for (int k = 0; k < s.keptCount; ++k)
        for (int r = 0; r < s.rangeCount; ++r)
        {
            const std::uintptr_t start = std::max(s.kept[k].start, s.ranges[r].start),
                                 end = std::min(s.kept[k].end, s.ranges[r].end);
            if (start < end && !add(start, end)) return false;
        }
    return true;
}

Phases CurrentPhases()
{
    Phases phases{-2, 0, 0};
    if (g_pGame) phases.game = g_pGame->GetGameState();
    if (nlTaskManager::m_pInstance)
    {
        phases.current = nlTaskManager::m_pInstance->mCurrentState;
        phases.pending = nlTaskManager::m_pInstance->mPendingState;
    }
    return phases;
}

// What a rollback past this step must check.
void RecordFrame(Step& step)
{
    step.phases = CurrentPhases();
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    step.reading = producers.pending != 0;
    step.readsStarted = producers.started;
}

void Record(std::uint64_t value, std::uint64_t& total, std::uint64_t& max)
{
    total += value;
    max = std::max(max, value);
}

void PrintPlace(FILE* file, const Place& place);
void BuildWatched();
unsigned OldestPredicted();

void WriteSummary()
{
    const State& s = *sState;
    FILE* file = std::fopen(s.summaryPath, "w");
    if (!file) return;
    const auto& h = mscharged::platform::RollbackSnapshotStatistics();
    if (s.netplay)
    {
        std::fprintf(file, "rollback: network matches predict, input delay %d steps\n", s.inputDelay);
        std::fprintf(file, "DESYNCS: %llu", static_cast<unsigned long long>(s.desyncs));
        if (s.desyncs) std::fprintf(file, " (first at frame %u)", s.firstDesyncFrame);
        std::fprintf(file, "; checksums compared with other machines %llu; ROLLBACKS IMPOSSIBLE %llu\n",
                     static_cast<unsigned long long>(s.remoteChecks), static_cast<unsigned long long>(s.impossible));
        std::fprintf(file, "steps with predicted inputs %llu, by how many steps predicted:",
                     static_cast<unsigned long long>(s.predictedSteps));
        for (int d = 1; d <= kMaxDepth; ++d)
            if (s.predictedDepth[d])
                std::fprintf(file, " %d: %llu", d, static_cast<unsigned long long>(s.predictedDepth[d]));
        std::fprintf(file, "\nmispredicted steps by machine:");
        for (int m = 0; m < kMachines; ++m)
            std::fprintf(file, " %llu", static_cast<unsigned long long>(s.mispredictions[m]));
        std::fprintf(file, "; rollbacks to correct them %llu\n",
                     static_cast<unsigned long long>(s.correctionRollbacks));
        std::fprintf(file, "steps undone (a phase change on predicted inputs) %llu, replays cut at a phase change "
                           "%llu; waits for inputs: as the original router %llu, prediction window full %llu\n",
                     static_cast<unsigned long long>(s.undone), static_cast<unsigned long long>(s.truncated),
                     static_cast<unsigned long long>(s.lockstepWaits), static_cast<unsigned long long>(s.windowWaits));
        std::fprintf(file, "time sync: ahead of the others by %.2f steps now, %.2f on average, %.2f at most; "
                           "slowed by %.2f s in all\n",
                     s.lead, s.leadSamples ? s.leadSum / static_cast<double>(s.leadSamples) : 0.0, s.maxLead,
                     s.slowedSeconds);
    }
    else
        std::fprintf(file, "rollback test: up to %d steps back every %d steps\n", s.maxDepth, s.interval);
    std::fprintf(file, "%llu steps, tracking started %d times; %d regions, %zu pages tracked, %zu hot\n",
                 static_cast<unsigned long long>(s.stepCount), s.starts, s.regionCount, h.trackedPages, h.hotPages);
    std::fprintf(file, "rollbacks: %llu, steps replayed %llu, frames %llu replayed as %llu; by steps undone:",
                 static_cast<unsigned long long>(s.rollbacks), static_cast<unsigned long long>(s.replayed),
                 static_cast<unsigned long long>(s.framesReplayed + s.framesMerged),
                 static_cast<unsigned long long>(s.framesReplayed));
    for (int d = 1; d <= kMaxDepth; ++d)
        if (s.byDepth[d]) std::fprintf(file, " %d: %llu", d, static_cast<unsigned long long>(s.byDepth[d]));
    std::fprintf(file, "\nREPLAYED STEPS WITH ANOTHER CHECKSUM: %llu", static_cast<unsigned long long>(s.mismatches));
    if (s.mismatches) std::fprintf(file, " (first at frame %u)", s.firstMismatchFrame);
    std::fprintf(file, "\nfailed restores %llu; replays reaching a NIS %llu; frees the registry could not undo %llu; "
                       "pages written behind the tracking %llu\n",
                 static_cast<unsigned long long>(s.failedRestores), static_cast<unsigned long long>(s.nisInReplay),
                 static_cast<unsigned long long>(s.lossyFrees), static_cast<unsigned long long>(s.untracked));
    std::fprintf(file, "skipped: disc read in flight %llu, disc read started %llu, game or task state change %llu, "
                       "too many events %llu, too few steps saved %llu\n",
                 static_cast<unsigned long long>(s.skippedReads), static_cast<unsigned long long>(s.skippedStarts),
                 static_cast<unsigned long long>(s.skippedPhases),
                 static_cast<unsigned long long>(s.skippedEvents), static_cast<unsigned long long>(s.skippedShort));
    std::fprintf(file, "mega strike meter presses received %llu, given again in replays %llu\n",
                 static_cast<unsigned long long>(s.meterReceived), static_cast<unsigned long long>(s.meterRedelivered));
    if (h.saves)
        std::fprintf(file, "save: %.1f us avg / %.1f us max; %llu pages avg / %llu max\n", h.saveNs / 1e3 / h.saves,
                     h.maxSaveNs / 1e3, static_cast<unsigned long long>(h.savedPages / h.saves),
                     static_cast<unsigned long long>(h.maxSavedPages));
    if (s.rollbacks)
    {
        std::fprintf(file, "restore: %.1f us avg / %.1f us max\n", s.restoreNs / 1e3 / s.rollbacks,
                     s.maxRestoreNs / 1e3);
        std::fprintf(file, "rollback (restore, replay, saves): %.2f ms avg / %.2f ms max\n",
                     s.rollbackNs / 1e6 / s.rollbacks, s.maxRollbackNs / 1e6);
    }
    if (s.framesReplayed)
    {
        const double frames = static_cast<double>(s.framesReplayed);
        std::fprintf(file, "replay, us per frame replayed:");
        for (int i = 0; i < s.taskCount; ++i)
            if (s.taskNs[i])
            {
                const char* name = s.tasks[i]->GetName();
                std::fprintf(file, " %s %.0f", name ? name : "?", s.taskNs[i] / 1e3 / frames);
            }
        if (s.replayed)
            std::fprintf(file, "; us per step replayed: step %.0f, save %.0f", s.stepNs / 1e3 / s.replayed,
                         s.saveNs / 1e3 / s.replayed);
        std::fprintf(file, "\n");
    }
    const auto registry = mscharged::platform::CurrentGameAllocationCheckpointStats();
    std::fprintf(file, "registry: %llu copies of an allocation's facts, %zu journal entries held; changes it does "
                       "not follow %llu, failed copies %llu\n",
                 static_cast<unsigned long long>(registry.copies), registry.entries,
                 static_cast<unsigned long long>(registry.untracked),
                 static_cast<unsigned long long>(registry.failedCopies));
    std::fprintf(file, "faults %llu, cold pages turned hot %llu, hot turned cold %llu, saves dropped early %llu\n",
                 static_cast<unsigned long long>(h.faults), static_cast<unsigned long long>(h.promoted),
                 static_cast<unsigned long long>(h.demoted), static_cast<unsigned long long>(h.evicted));
    if (s.compareEvery)
    {
        std::fprintf(file, "\ncompared every %d rollbacks: %llu, %llu with differences, %llu bytes in all; "
                           "places %d (%llu not told apart)\n",
                     s.compareEvery, static_cast<unsigned long long>(s.compares),
                     static_cast<unsigned long long>(s.comparesDiffering),
                     static_cast<unsigned long long>(s.compareDiffBytes), s.placeCount,
                     static_cast<unsigned long long>(s.placesDropped));
        // The places, most often differing first (sorting a copy of the indices).
        int order[kMaxPlaces];
        for (int i = 0; i < s.placeCount; ++i) order[i] = i;
        std::sort(order, order + s.placeCount, [&](int a, int b) {
            return s.places[a].compares != s.places[b].compares ? s.places[a].compares > s.places[b].compares
                                                                : s.places[a].bytes > s.places[b].bytes;
        });
        for (int i = 0; i < s.placeCount && i < kListedPlaces * 2; ++i)
        {
            const Place& place = s.places[order[i]];
            std::fprintf(file, "  %llu times, %llu bytes: ", static_cast<unsigned long long>(place.compares),
                         static_cast<unsigned long long>(place.bytes));
            PrintPlace(file, place);
            std::fprintf(file, "\n");
        }
    }
    std::fprintf(file, "\nnot restored:\n");
    for (int i = 0; i < s.excludedCount; ++i)
        std::fprintf(file, "  %s: %zu bytes\n", s.excluded[i].name,
                     static_cast<std::size_t>(s.excluded[i].end - s.excluded[i].start));
    std::fprintf(file, "restored inside those: %d ranges (the network players' inputs)\n", s.keptCount);
    std::fclose(file);
}

// The tasks in the task manager's order, and what a replay does with each.
bool FindTasks(FixedUpdateTask* fixed)
{
    State& s = *sState;
    s.taskCount = 0;
    nlTaskManager* manager = nlTaskManager::m_pInstance;
    if (!manager || !manager->mTaskList) return false;
    nlTask* first = manager->mTaskList;
    nlTask* t = first;
    std::fprintf(s.out, "tasks:");
    do
    {
        if (s.taskCount == kMaxTasks) return false;
        const char* name = t->GetName();
        TaskReplay replay = kSkip;
        if (t == fixed)
            replay = kFixed;
        else if (name && !std::strcmp(name, "End Frame"))
            replay = kEnd;
        else
            for (const char* run : kRunTasks)
                if (name && !std::strcmp(name, run)) replay = kRun;
        std::fprintf(s.out, " %s (%s)", name ? name : "?",
                     replay == kSkip ? "left out" : replay == kRun ? "run" : "its replay part");
        s.tasks[s.taskCount] = t;
        s.replays[s.taskCount++] = replay;
        t = t->m_next;
    } while (t && t != first);
    std::fprintf(s.out, "\n");
    return true;
}

void RestoreSeeds()
{
    State& s = *sState;
    if (!s.seedsSaved) return;
    s.seedsSaved = false;
    nlDefaultSeed = s.defaultSeed;
    gEffectsRandomSeed = s.effectsSeed;
}

int fixedIndexOf()
{
    const State& s = *sState;
    int index = 0;
    while (index < s.taskCount - 1 && s.replays[index] != kFixed) ++index;
    return index;
}

FrameRecord& Frame(std::uint64_t frame)
{
    return sState->frames[frame % kFrameRing];
}

void BeginFrameRecord()
{
    State& s = *sState;
    FrameRecord& record = Frame(s.hostFrame);
    std::fill(std::begin(record.dt), std::end(record.dt), -1.0f);
    std::fill(std::begin(record.real), std::end(record.real), -1.0f);
    mscharged_tweak_save_frame_state(&record.tweak);
}

bool Start(FixedUpdateTask* task, unsigned long taskBytes, unsigned (*checksum)(void))
{
    State& s = *sState;
    if (!FindTasks(task))
    {
        std::fprintf(s.out, "rollback test: cannot list the tasks; stopped\n");
        std::fflush(s.out);
        return false;
    }
    FindRanges(task, taskBytes);
    if (!BuildRegions()
        || !mscharged::platform::ConfigureRollbackSnapshots(s.regions, s.regionCount, kMaxDepth + 1,
                                                            static_cast<std::size_t>(kMaxDepth + 1) * 512 + 2048))
    {
        std::fprintf(s.out, "rollback test: cannot track the match; stopped\n");
        std::fflush(s.out);
        return false;
    }
    if (s.compareEvery) BuildWatched();
    const unsigned frame = static_cast<unsigned>(task->GetFrame());
    mscharged::platform::BeginGameAllocationCheckpoint(Excluded);
    Step& step = s.steps[frame % kRing];
    step.frame = frame;
    step.snapshot = mscharged::platform::BeginRollbackSnapshots();
    step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
    step.checksum = checksum();
    step.seed = GetNetworkRandomSeed();
    step.hostFrame = 0;
    RecordFrame(step);
    s.latest = frame;
    s.stored = 1;
    s.haveInputs = false;
    s.receivedCount = 0;
    s.receivedOverflowed = false;
    s.hostFrame = 0;
    s.seedsSaved = false;
    BeginFrameRecord();
    s.active = true;
    ++s.starts;
    std::fprintf(s.out, "tracking from frame %u\n", frame);
    std::fflush(s.out);
    return true;
}

void Stop(const char* why)
{
    State& s = *sState;
    if (!s.active) return;
    mscharged::platform::EndRollbackSnapshots();
    mscharged::platform::EndGameAllocationCheckpoint();
    s.active = false;
    s.pendingDepth = 0;
    s.undoFrom = 0;
    if (OldestPredicted())
    {
        // Nothing can go back before them any more.
        std::fprintf(s.out, "STOPPED WITH INPUTS STILL PREDICTED: a misprediction now desyncs\n");
        for (unsigned& first : s.firstPredicted) first = 0;
    }
    std::fprintf(s.out, "tracking stopped after frame %u: %s\n", s.latest, why);
    std::fflush(s.out);
    WriteSummary();
}

// RECHARGED_ROLLBACK_TEST_COMPARE also watches what no rollback restores:
// the module's other globals, and what the test leaves out of the arenas but
// the audio and frame memory (the replay draws and the audio thread plays).
// A replay should not change them; what it does change goes forward twice.
constexpr const char* kModuleGlobals = "module global";

int AddLoadSegments(dl_phdr_info* info, std::size_t, void* data)
{
    State& s = *sState;
    if (info->dlpi_addr != s.moduleBase) return 0;
    auto* segments = static_cast<Range*>(data);
    int count = 0;
    for (int i = 0; i < info->dlpi_phnum && count < 8; ++i)
    {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || !(header.p_flags & PF_W)) continue;
        const std::uintptr_t start = info->dlpi_addr + header.p_vaddr;
        segments[count++] = {start, start + header.p_memsz, kModuleGlobals};
    }
    return count;
}

void BuildWatched()
{
    State& s = *sState;
    s.watchedCount = 0;
    Range segments[8] = {};
    dl_iterate_phdr(AddLoadSegments, segments);
    Range cuts[kMaxRanges];
    std::copy(s.ranges, s.ranges + s.rangeCount, cuts);
    std::sort(cuts, cuts + s.rangeCount, [](const Range& a, const Range& b) { return a.start < b.start; });
    auto add = [&](std::uintptr_t start, std::uintptr_t end, const char* name) {
        if (start < end && s.watchedCount < kMaxWatched) s.watched[s.watchedCount++] = {start, end, name};
    };
    for (const Range& segment : segments)
    {
        if (!segment.start) continue;
        std::uintptr_t at = segment.start;
        for (int c = 0; c < s.rangeCount; ++c)
        {
            if (cuts[c].end <= at || cuts[c].start >= segment.end) continue;
            add(at, std::max(at, cuts[c].start), kModuleGlobals);
            at = std::max(at, cuts[c].end);
        }
        add(at, segment.end, kModuleGlobals);
    }
    for (int i = 0; i < s.excludedCount; ++i)
    {
        const char* name = s.excluded[i].name;
        if (!std::strcmp(name, "audio memory") || !std::strcmp(name, "AudioBackend") || !std::strcmp(name, "frame memory")
            || !std::strncmp(name, "task ", 5) || !std::strncmp(name, "fixed steps'", 12))
            continue;
        add(s.excluded[i].start, s.excluded[i].end, name);
    }
}

// RECHARGED_ROLLBACK_TEST_COMPARE: the tracked memory before a restore, and
// the watched memory.
bool CopyForCompare()
{
    State& s = *sState;
    std::size_t bytes = 0;
    for (int i = 0; i < s.regionCount; ++i) bytes += s.regions[i].bytes;
    for (int i = 0; i < s.watchedCount; ++i) bytes += s.watched[i].end - s.watched[i].start;
    if (bytes != s.compareBytes)
    {
        if (s.compareCopy) munmap(s.compareCopy, s.compareBytes);
        void* copy = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        s.compareCopy = copy == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(copy);
        s.compareBytes = s.compareCopy ? bytes : 0;
        if (!s.compareCopy) return false;
    }
    std::uint8_t* to = s.compareCopy;
    for (int i = 0; i < s.regionCount; ++i)
    {
        std::memcpy(to, s.regions[i].base, s.regions[i].bytes);
        to += s.regions[i].bytes;
    }
    for (int i = 0; i < s.watchedCount; ++i)
    {
        const std::size_t length = s.watched[i].end - s.watched[i].start;
        std::memcpy(to, reinterpret_cast<const void*>(s.watched[i].start), length);
        to += length;
    }
    return true;
}

bool ModuleOffset(std::uintptr_t value, std::uintptr_t& offset)
{
    Dl_info info{};
    if (!value || !dladdr(reinterpret_cast<void*>(value), &info)) return false;
    if (reinterpret_cast<std::uintptr_t>(info.dli_fbase) != sState->moduleBase) return false;
    offset = value - sState->moduleBase;
    return true;
}

const char* RangeName(std::uintptr_t address)
{
    const State& s = *sState;
    for (int i = 0; i < s.rangeCount; ++i)
        if (address >= s.ranges[i].start && address < s.ranges[i].end) return s.ranges[i].name;
    return "?";
}

Place* FindPlace(int kind, std::uintptr_t key, std::size_t size)
{
    State& s = *sState;
    for (int i = 0; i < s.placeCount; ++i)
        if (s.places[i].kind == kind && s.places[i].key == key && s.places[i].size == size) return &s.places[i];
    if (s.placeCount == kMaxPlaces)
    {
        ++s.placesDropped;
        return nullptr;
    }
    Place& place = s.places[s.placeCount++];
    place = {};
    place.kind = kind;
    place.key = key;
    place.size = size;
    return &place;
}

void PrintPlace(FILE* file, const Place& place)
{
    switch (place.kind)
    {
    case kGlobal:
        std::fprintf(file, "global module+0x%lx (%s)", static_cast<unsigned long>(place.key), place.region);
        break;
    case kAllocation:
        std::fprintf(file, "%zu-byte allocation (%s), first differing +0x%lx, ", place.size, place.region,
                     static_cast<unsigned long>(place.firstOffset));
        if (place.key)
            std::fprintf(file, "first word module+0x%lx", static_cast<unsigned long>(place.key));
        else
            std::fprintf(file, "no vtable");
        break;
    case kNotRestored:
        if (place.region == kModuleGlobals)
            std::fprintf(file, "NOT RESTORED module global module+0x%lx", static_cast<unsigned long>(place.key));
        else
            std::fprintf(file, "NOT RESTORED %s, +0x%lx", place.region, static_cast<unsigned long>(place.firstOffset));
        break;
    default:
        std::fprintf(file, "no live allocation (%s)", place.region);
        break;
    }
}

void PrintBytes(FILE* file, const char* label, const std::uint8_t* bytes)
{
    std::fprintf(file, "      %s", label);
    for (int i = 0; i < 16; ++i) std::fprintf(file, " %02x", bytes[i]);
    std::fprintf(file, "\n");
}

// One range that differs, [start, end), with `before` its bytes in the copy;
// `watched` the watched range it lies in, if any.
void NoteDifference(std::uintptr_t start, std::uintptr_t end, const std::uint8_t* before, const Range* watched)
{
    State& s = *sState;
    s.compareDiffBytes += end - start;
    int kind = kUnowned;
    std::uintptr_t key = 0, offset = 0;
    std::size_t size = 0;
    if (watched)
    {
        Place* place = FindPlace(kNotRestored,
                                 watched->name == kModuleGlobals ? start - s.moduleBase : watched->start, 0);
        if (!place) return;
        place->bytes += end - start;
        if (place->lastCompare == s.compares) return;
        place->lastCompare = s.compares;
        if (!place->compares++)
        {
            place->region = watched->name;
            place->firstOffset = start - watched->start;
            const std::size_t shown = std::min<std::size_t>(16, end - start);
            std::memcpy(place->before, before, shown);
            std::memcpy(place->after, reinterpret_cast<const void*>(start), shown);
        }
        return;
    }
    const auto data = reinterpret_cast<std::uintptr_t>(__rbk_data_start);
    const auto bss = reinterpret_cast<std::uintptr_t>(__rbk_bss_start);
    mscharged::platform::GameAllocationSpan span{};
    if ((data && start >= data && start < reinterpret_cast<std::uintptr_t>(__rbk_data_end))
        || (bss && start >= bss && start < reinterpret_cast<std::uintptr_t>(__rbk_bss_end)))
    {
        kind = kGlobal;
        key = start - s.moduleBase;
    }
    else if (mscharged::platform::FindGameAllocationSpan(reinterpret_cast<const void*>(start), 1, span))
    {
        kind = kAllocation;
        const auto base = reinterpret_cast<std::uintptr_t>(span.base);
        size = span.bytes;
        offset = start - base;
        std::uintptr_t first = 0;
        if (span.bytes >= sizeof(first)) std::memcpy(&first, span.base, sizeof(first));
        if (!ModuleOffset(first, key)) key = 0;
    }
    Place* place = FindPlace(kind, key, size);
    if (!place) return;
    place->bytes += end - start;
    if (place->lastCompare == s.compares) return;
    place->lastCompare = s.compares;
    if (!place->compares++)
    {
        place->firstOffset = offset;
        place->region = RangeName(start);
        const std::size_t shown = std::min<std::size_t>(16, end - start);
        std::memcpy(place->before, before, shown);
        std::memcpy(place->after, reinterpret_cast<const void*>(start), shown);
    }
}

// After a replay: what differs from the copy, in 8-byte words, ranges joined
// across gaps under 32 bytes.
void CompareAfterReplay(unsigned frame, int depth)
{
    State& s = *sState;
    ++s.compares;
    const std::uint64_t bytesBefore = s.compareDiffBytes;
    const int placesBefore = s.placeCount;
    const std::uint8_t* copy = s.compareCopy;
    for (int r = 0; r < s.regionCount + s.watchedCount; ++r)
    {
        const Range* watched = r < s.regionCount ? nullptr : &s.watched[r - s.regionCount];
        const auto* live = watched ? reinterpret_cast<const std::uint8_t*>(watched->start)
                                   : static_cast<const std::uint8_t*>(s.regions[r].base);
        const std::size_t bytes = watched ? watched->end - watched->start : s.regions[r].bytes;
        constexpr std::size_t kPage = 4096;
        std::uintptr_t open = 0, last = 0;
        const std::uint8_t* openBefore = nullptr;
        for (std::size_t page = 0; page < bytes; page += kPage)
        {
            const std::size_t length = std::min(kPage, bytes - page);
            if (!std::memcmp(live + page, copy + page, length)) continue;
            for (std::size_t at = page; at + 8 <= page + length; at += 8)
            {
                if (!std::memcmp(live + at, copy + at, 8)) continue;
                const auto address = reinterpret_cast<std::uintptr_t>(live + at);
                if (open && address - last < 32)
                {
                    last = address + 8;
                    continue;
                }
                if (open) NoteDifference(open, last, openBefore, watched);
                open = address;
                last = address + 8;
                openBefore = copy + at;
            }
        }
        if (open) NoteDifference(open, last, openBefore, watched);
        copy += bytes;
    }
    const bool differs = s.compareDiffBytes != bytesBefore;
    if (differs) ++s.comparesDiffering;
    if (!differs || s.comparesDiffering > kDetailedCompares) return;
    std::fprintf(s.out, "COMPARED rollback of %d steps from frame %u: %llu bytes differ; places first seen now:\n",
                 depth, frame, static_cast<unsigned long long>(s.compareDiffBytes - bytesBefore));
    for (int i = placesBefore; i < s.placeCount && i < placesBefore + kListedPlaces; ++i)
    {
        std::fprintf(s.out, "  ");
        PrintPlace(s.out, s.places[i]);
        std::fprintf(s.out, "\n");
        PrintBytes(s.out, "before:", s.places[i].before);
        PrintBytes(s.out, "after: ", s.places[i].after);
    }
    std::fflush(s.out);
}

StepInputs& InputsFor(unsigned frame)
{
    return sState->inputs[frame % kInputRing];
}

bool AnyPredicted(const StepInputs& inputs)
{
    for (int machine = 0; machine < inputs.machineCount; ++machine)
        if (inputs.predicted[machine]) return true;
    return false;
}

// RECHARGED_ROLLBACK: a replayed step changed the match's phase (the inputs
// that arrived made a goal, say): the replay ends with it, and the steps after
// it run again when their inputs have all arrived. A step with inputs still
// predicted does not change the phase either, and is undone as well.
void StopReplayAt(unsigned frame)
{
    State& s = *sState;
    if (!AnyPredicted(InputsFor(frame)))
    {
        s.replayEnd = frame;
        ++s.truncated;
        return;
    }
    const Step& before = s.steps[(frame - 1) % kRing];
    if (!mscharged::platform::RestoreRollbackSnapshot(before.snapshot))
    {
        ++s.failedRestores;
        std::fprintf(s.out, "ROLLBACK IMPOSSIBLE: the save after frame %u was gone\n", frame - 1);
        s.replayEnd = frame;
        return;
    }
    s.lossyFrees += mscharged::platform::RollbackGameAllocationCheckpointTo(before.mark, Excluded);
    s.replayEnd = frame - 1;
    ++s.undone;
}

// Replays frames `first` to `last` as one, their tasks from `firstTask` as the
// task manager ran them, each with the frames' time added up, and the steps in
// them from `next` (the steps with their inputs, a save after each). Only the
// last frame has steps. The time scale's transition goes frame by frame: the
// steps read it, and it changes with itself.
void ReplayFrames(std::uint64_t first, std::uint64_t last, int firstTask, unsigned& next,
                  int (*replay)(const MschargedRollbackInputs*), unsigned (*checksum)(void), int depth)
{
    State& s = *sState;
    const FrameRecord& record = Frame(last);
    nlTaskManager* manager = nlTaskManager::m_pInstance;
    for (int i = firstTask; i < s.taskCount; ++i)
    {
        if (s.replays[i] == kSkip) continue;
        float dt = 0, real = 0;
        bool ran = false;
        for (std::uint64_t f = first; f <= last; ++f)
            if (Frame(f).dt[i] >= 0)
            {
                dt += Frame(f).dt[i];
                real += Frame(f).real[i];
                ran = true;
            }
        if (!ran) continue;
        manager->mCurrentTimeDelta = dt;
        manager->mRealTimeDelta = real;
        const std::uint64_t taskStart = NowNs();
        switch (s.replays[i])
        {
        case kRun:
            s.tasks[i]->Run(dt);
            break;
        case kEnd:
            RollbackReplayEndFrame();
            break;
        case kFixed:
        {
            if (firstTask == 0) // done before the first frame's target step
                for (std::uint64_t f = first; f <= last; ++f)
                    if (Frame(f).dt[i] >= 0) RollbackReplayFixedFrame(Frame(f).dt[i]);
            // The steps as in their frame.
            if (record.dt[i] >= 0)
            {
                manager->mCurrentTimeDelta = record.dt[i];
                manager->mRealTimeDelta = record.real[i];
            }
            for (; next <= s.replayEnd && s.steps[next % kRing].hostFrame <= last; ++next)
            {
                Step& step = s.steps[next % kRing];
                const std::uint64_t stepStart = NowNs();
                RollbackRedeliverMegaStrikeMeter(step.meter, step.meterCount);
                s.meterRedelivered += step.meterCount;
                if (replay(&step.inputs) < 0) ++s.nisInReplay;
                const unsigned now = checksum();
                const std::uint64_t saveStart = NowNs();
                s.stepNs += saveStart - stepStart;
                if (s.netplay)
                {
                    // Other inputs, another match: what it is now.
                    step.checksum = now;
                    step.seed = GetNetworkRandomSeed();
                    RecordFrame(step);
                }
                else if (now != step.checksum)
                {
                    if (!s.mismatches) s.firstMismatchFrame = next;
                    if (s.mismatches++ < kListedMismatches)
                        std::fprintf(s.out, "REPLAYED FRAME %u (%d back from %u): checksum %08x, first run %08x\n",
                                     next, depth, s.latest, now, step.checksum);
                }
                step.snapshot = mscharged::platform::SaveRollbackSnapshot();
                step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
                s.saveNs += NowNs() - saveStart;
                ++s.replayed;
                if (s.netplay && !(step.phases == s.steps[(next - 1) % kRing].phases)) StopReplayAt(next);
            }
            auto* fixed = static_cast<FixedUpdateTask*>(s.tasks[i]);
            fixed->mAccumulatedDeltaT = record.accumulated;
            fixed->mInterpolationDeltaT = record.interpolation;
            break;
        }
        case kSkip:
            break;
        }
        s.taskNs[i] += NowNs() - taskStart;
    }
    ++s.framesReplayed;
    s.framesMerged += last - first;
}

// Why a rollback cannot go back to `target`, or null when it can: a disc
// read in flight or started since (its completion went into memory a restore
// takes back), more frames than are kept, or (in the test) a change of the
// game's or tasks' state, or more events than a step keeps.
const char* CannotRollBack(unsigned target)
{
    State& s = *sState;
    // The end of the last step's frame counts as well.
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    if (producers.pending)
    {
        ++s.skippedReads;
        return "a disc read in flight";
    }
    if (producers.started != s.steps[target % kRing].readsStarted)
    {
        ++s.skippedStarts;
        return "a disc read started";
    }
    if (!s.netplay && !(CurrentPhases() == s.steps[s.latest % kRing].phases))
    {
        ++s.skippedPhases;
        return "a state change";
    }
    if (s.receivedOverflowed || s.hostFrame - s.steps[target % kRing].hostFrame >= kFrameRing)
    {
        ++s.skippedEvents;
        return "too many events or frames";
    }
    for (unsigned f = target; f <= s.latest; ++f)
    {
        const Step& step = s.steps[f % kRing];
        if (step.reading)
        {
            ++s.skippedReads;
            return "a disc read in flight";
        }
        if (step.readsStarted != s.steps[target % kRing].readsStarted)
        {
            ++s.skippedStarts;
            return "a disc read started";
        }
        if (!s.netplay && !(step.phases == s.steps[s.latest % kRing].phases))
        {
            ++s.skippedPhases;
            return "a state change";
        }
        if (f > target && (step.inputs.overflowed || step.meterOverflowed))
        {
            ++s.skippedEvents;
            return "too many events or frames";
        }
    }
    return nullptr;
}

// At the start of a frame: back to the save after step `target`, then
// forward again through the end of the frame that just ended, running the
// steps through `replayEnd` with the inputs their records hold. False when
// it cannot (CannotRollBack).
bool RollBack(unsigned target, unsigned replayEnd, int (*replay)(const MschargedRollbackInputs*),
              unsigned (*checksum)(void))
{
    State& s = *sState;
    const int depth = static_cast<int>(s.latest - target);
    if (const char* why = CannotRollBack(target))
    {
        if (s.netplay)
        {
            ++s.impossible;
            std::fprintf(s.out, "ROLLBACK IMPOSSIBLE to frame %u from %u: %s\n", target, s.latest, why);
        }
        return false;
    }
    const std::uint64_t firstFrame = s.steps[target % kRing].hostFrame;

    const bool compare = s.compareEvery && s.places && (s.rollbacks + 1) % s.compareEvery == 0 && CopyForCompare();
    const std::uint64_t start = NowNs();
    const Step& from = s.steps[target % kRing];
    if (!mscharged::platform::RestoreRollbackSnapshot(from.snapshot))
    {
        ++s.failedRestores;
        Stop("a save to roll back to was gone");
        return false;
    }
    s.lossyFrees += mscharged::platform::RollbackGameAllocationCheckpointTo(from.mark, Excluded);
    Record(NowNs() - start, s.restoreNs, s.maxRestoreNs);

    // The frames from the target step's on, the first from the task after the
    // fixed update (its later steps included), through the end of the frame
    // that just ended. The frame rate option's state is put back for each
    // frame's start, and after the target's start for the first. Frames
    // without steps go as one with the next frame (up to the task manager's
    // longest frame): above the original's frame rate, a step spans several.
    MschargedTweakFrameState now{};
    mscharged_tweak_save_frame_state(&now);
    auto* fixed = static_cast<FixedUpdateTask*>(s.tasks[fixedIndexOf()]);
    const float accumulated = fixed->mAccumulatedDeltaT, interpolation = fixed->mInterpolationDeltaT;
    // In a network match, the steps whose inputs were applied (some after the
    // last saved, when a step's presentation stopped the hook).
    const unsigned ran = s.netplay ? std::max(s.lastInputFrame, s.latest) : s.latest;
    s.replayEnd = replayEnd;
    s.resimulating = true;
    unsigned next = target + 1;
    const int fixedIndex = fixedIndexOf();
    mscharged_tweak_load_frame_state(firstFrame + 1 <= s.hostFrame ? &Frame(firstFrame + 1).tweak : &now);
    ReplayFrames(firstFrame, firstFrame, fixedIndex, next, replay, checksum, depth);
    float time = 0;
    for (std::uint64_t frame = firstFrame + 1, from = frame; frame <= s.hostFrame; ++frame)
    {
        time += std::max(Frame(frame).dt[fixedIndex], 0.0f);
        const bool steps = next <= s.replayEnd && s.steps[next % kRing].hostFrame <= frame;
        if (s.merge && !steps && frame < s.hostFrame && time + Frame(frame + 1).dt[fixedIndex] <= kLongestFrame)
            continue;
        mscharged_tweak_load_frame_state(&Frame(from).tweak);
        ReplayFrames(from, frame, 0, next, replay, checksum, depth);
        from = frame + 1;
        time = 0;
    }
    mscharged_tweak_load_frame_state(&now);
    fixed->mAccumulatedDeltaT = accumulated;
    fixed->mInterpolationDeltaT = interpolation;
    // And what the network gave the match after the last step.
    RollbackRedeliverMegaStrikeMeter(s.received, s.receivedCount);
    s.meterRedelivered += s.receivedCount;
    s.resimulating = false;
    if (next != s.replayEnd + 1) std::fprintf(s.out, "REPLAY MISSED STEPS %u to %u\n", next, s.replayEnd);
    if (s.netplay && s.replayEnd < ran)
    {
        // Steps undone: they run again when their inputs have all arrived (the
        // inputs that did arrive are kept), and their time goes back to the
        // fixed update, to catch up.
        s.latest = s.replayEnd;
        s.lastInputFrame = s.replayEnd;
        s.waitThrough = std::max(s.waitThrough, ran);
        for (unsigned& first : s.firstPredicted)
            if (first > s.replayEnd) first = 0;
        fixed->mAccumulatedDeltaT += static_cast<float>(ran - s.replayEnd) * g_fFixedUpdateTick;
    }
    ++s.rollbacks;
    ++s.byDepth[std::min(depth, kMaxDepth)];
    Record(NowNs() - start, s.rollbackNs, s.maxRollbackNs);
    if (compare) CompareAfterReplay(s.latest, depth);
    return true;
}

// RECHARGED_ROLLBACK: network matches that predict. The network router's
// queues hold each machine's input messages in the order it sent them, one
// per step after the input delay; a step whose message has not arrived runs
// with that machine's last inputs that did, and when its message arrives (at
// the start of a frame) and says otherwise, the match goes back before the
// first step it got wrong and runs again with what arrived.

NetworkInputRouter* Router()
{
    return gNetworkInputRouter;
}

int MachineCount()
{
    return std::min<int>(g_pNetworkSessionBase->GetNumMachines(), kMachines);
}

// The fixed update's frame: the last step that ran.
unsigned CurrentStep()
{
    return static_cast<unsigned>(GetFixedUpdateTask()->GetFrame());
}

// A machine's inputs as the router holds them (before any arrived, and while
// the router repeats them for the input delay).
MachineInput FromRouter(int machine)
{
    NetworkInputRouter* router = Router();
    MachineInput input{};
    input.players = static_cast<unsigned char>(
        std::min<int>(g_pNetworkSessionBase->GetPeer(static_cast<s8>(machine))->mPlayerCount, 4));
    input.remap = router->mRemapAngles[machine];
    for (int player = 0; player < input.players; ++player)
    {
        const s8 id = GetNetworkPlayerId(static_cast<s8>(player), static_cast<s8>(machine));
        std::memcpy(input.records[player], &router->mInputRecords[id], sizeof(PackedDetInput));
        input.states[player] = router->mInputStates[id];
    }
    return input;
}

// A machine's inputs from its message: what changed since `previous`, its
// previous message's.
MachineInput FromMessage(NetMessageInput& message, const MachineInput& previous)
{
    MachineInput input = previous;
    message.GetNetworkInputMessageRemapAngle(&input.remap);
    for (int player = 0; player < input.players; ++player)
    {
        PackedDetInput record;
        std::memcpy(&record, input.records[player], sizeof(record));
        message.ApplyNetworkInputMessageRecord(static_cast<s8>(player), &record);
        std::memcpy(input.records[player], &record, sizeof(record));
        input.states[player] = message.GetNetworkInputMessagePlayerState(static_cast<s8>(player));
    }
    input.eventCount = std::min<int>(message.mEventCount, kMachineEvents);
    for (int event = 0; event < input.eventCount; ++event)
        std::memcpy(input.events[event], message.GetNetworkInputMessageEvent(event), sizeof(input.events[event]));
    return input;
}

MachineInput Predicted(int machine)
{
    MachineInput input = sState->lastReal[machine];
    input.eventCount = 0;
    return input;
}

bool Same(const MachineInput& a, const MachineInput& b)
{
    if (a.players != b.players || a.remap != b.remap || a.eventCount != b.eventCount) return false;
    for (int player = 0; player < a.players; ++player)
        if (std::memcmp(a.records[player], b.records[player], sizeof(a.records[player]))
            || a.states[player] != b.states[player])
            return false;
    return !std::memcmp(a.events, b.events, sizeof(a.events[0]) * static_cast<std::size_t>(a.eventCount));
}

// What the router applies, in its order: machine by machine, its events, then
// its players' inputs.
void Compose(const StepInputs& step, MschargedRollbackInputs& out)
{
    out = {};
    out.machineCount = step.machineCount;
    for (int machine = 0; machine < step.machineCount; ++machine)
    {
        const MachineInput& input = step.machines[machine];
        out.machines[machine] = static_cast<signed char>(machine);
        out.players[machine] = input.players;
        out.remapAngles[machine] = input.remap;
        for (int player = 0; player < input.players; ++player)
        {
            const s8 id = GetNetworkPlayerId(static_cast<s8>(player), static_cast<s8>(machine));
            std::memcpy(out.records[id], input.records[player], sizeof(out.records[id]));
            out.states[id] = input.states[player];
        }
        for (int event = 0; event < input.eventCount; ++event)
        {
            if (out.eventCount == MSCHARGED_ROLLBACK_MAX_EVENTS)
            {
                out.overflowed = 1;
                break;
            }
            out.eventMachines[out.eventCount] = static_cast<signed char>(machine);
            std::memcpy(out.events[out.eventCount++], input.events[event], sizeof(out.events[0]));
        }
    }
}

// The router's records follow the inputs that arrived: the next message
// changes them, and the original router reads them.
void Mirror(int machine, const MachineInput& input)
{
    NetworkInputRouter* router = Router();
    router->mRemapAngles[machine] = input.remap;
    for (int player = 0; player < input.players; ++player)
    {
        const s8 id = GetNetworkPlayerId(static_cast<s8>(player), static_cast<s8>(machine));
        std::memcpy(&router->mInputRecords[id], input.records[player], sizeof(PackedDetInput));
        router->mInputStates[id] = input.states[player];
    }
}

// The checksum another machine sends: that of the last step whose inputs had
// all arrived there, compared when ours are in too.
void NoteSync(int machine, const NetMessageInput& message)
{
    State& s = *sState;
    if (machine == g_pNetworkSessionBase->GetLocalMachineId()) return;
    if ((message.mFlags & 6) == 2)
    {
        s.reportedAhead[machine] = static_cast<float>(static_cast<std::int32_t>(message.mFrame)) / 256.0f;
        s.haveReported[machine] = true;
        return;
    }
    if (!(message.mFlags & 4)) return;
    if (s.remoteCount == kRemoteChecks)
    {
        std::memmove(s.remote, s.remote + 1, sizeof(s.remote[0]) * (kRemoteChecks - 1));
        --s.remoteCount;
    }
    s.remote[s.remoteCount++] = {message.mFrame, message.mChecksum, message.mRandomSeed, machine};
}

MachineInput Arrived(int machine, NetMessageInput& message)
{
    State& s = *sState;
    NoteSync(machine, message);
    MachineInput input = FromMessage(message, s.lastReal[machine]);
    s.lastReal[machine] = input;
    Mirror(machine, input);
    return input;
}

// The phases a step may predict in: the match being played, nothing paused.
bool PredictablePhases(const Phases& phases)
{
    return (phases.game == GS_KICKOFF || phases.game == GS_GAMEPLAY || phases.game == GS_OVERTIME)
        && phases.current == TASK_GAMEPLAY && phases.pending == TASK_GAMEPLAY
        && !g_pNetworkSession->GetPausedMachineMask();
}

unsigned OldestPredicted()
{
    const State& s = *sState;
    unsigned oldest = 0;
    for (const unsigned first : s.firstPredicted)
        if (first && (!oldest || first < oldest)) oldest = first;
    return oldest;
}

// Whether step `frame` may run with predicted inputs: the rollback follows the
// match and could go back before the oldest step predicted, through as many
// frames as it keeps and without a disc read in between.
bool CanPredict(unsigned frame)
{
    State& s = *sState;
    if (!s.active || s.latest + 1 != frame || frame <= s.waitThrough) return false;
    const Phases phases = CurrentPhases();
    if (!PredictablePhases(phases) || !(phases == s.steps[s.latest % kRing].phases)) return false;
    const unsigned oldest = OldestPredicted();
    const unsigned target = oldest ? oldest - 1 : s.latest;
    if (frame - target > static_cast<unsigned>(kMaxDepth)) return false;
    const Step& from = s.steps[target % kRing];
    if (from.frame != target || s.hostFrame - from.hostFrame >= kFrameRing - 2) return false;
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    return !producers.pending && producers.started == from.readsStarted;
}

// The steps whose inputs have all arrived: their checksums are the match's,
// sent to the other machines and compared with theirs.
void UpdateConfirmed()
{
    State& s = *sState;
    if (!s.active) return;
    unsigned through = std::min(s.lastInputFrame, s.latest);
    if (const unsigned oldest = OldestPredicted()) through = std::min(through, oldest - 1);
    const unsigned from = s.haveConfirmed && s.confirmed < through ? s.confirmed + 1 : through;
    for (unsigned f = from; f <= through && f + kRing > s.latest; ++f)
    {
        const Step& step = s.steps[f % kRing];
        if (step.frame == f) s.history[f % kConfirmedRing] = {f, step.checksum, step.seed, -1};
    }
    if (!s.haveConfirmed || through > s.confirmed)
    {
        s.confirmed = through;
        s.haveConfirmed = true;
    }
    int kept = 0;
    for (int i = 0; i < s.remoteCount; ++i)
    {
        const SyncPoint& remote = s.remote[i];
        if (remote.frame > s.confirmed && remote.frame < s.confirmed + kConfirmedRing)
        {
            s.remote[kept++] = remote; // ours is not in yet
            continue;
        }
        const SyncPoint& ours = s.history[remote.frame % kConfirmedRing];
        if (ours.frame != remote.frame) continue; // not kept (or a step the hook did not see)
        ++s.remoteChecks;
        if (ours.checksum != remote.checksum || ours.seed != remote.seed)
        {
            if (!s.desyncs) s.firstDesyncFrame = remote.frame;
            if (s.desyncs++ < kListedMismatches)
                std::fprintf(s.out, "DESYNC at frame %u: checksum %08x seed %08x here, %08x %08x on machine %d\n",
                             remote.frame, ours.checksum, ours.seed, remote.checksum, remote.seed, remote.machine);
            std::fflush(s.out);
        }
    }
    s.remoteCount = kept;
}

// At the start of a frame: the messages that arrived for the steps predicted.
// Each one replaces its prediction, the steps still predicted are predicted
// again from the last that arrived, and when any changed the match goes back
// before the first.
void Resolve()
{
    State& s = *sState;
    NetworkInputRouter* router = Router();
    unsigned earliest = 0;
    const auto changed = [&](unsigned frame) { earliest = earliest ? std::min(earliest, frame) : frame; };
    for (int machine = 0; machine < MachineCount(); ++machine)
    {
        unsigned frame = s.firstPredicted[machine];
        if (!frame) continue;
        NetworkInputMessageQueue& queue = router->m_InputQueue[machine];
        for (; frame <= s.lastInputFrame && queue.GetCount() > 0; ++frame)
        {
            StepInputs& step = InputsFor(frame);
            const MachineInput input = Arrived(machine, queue.Pop());
            if (!Same(step.machines[machine], input))
            {
                changed(frame);
                ++s.mispredictions[machine];
            }
            step.machines[machine] = input;
            step.predicted[machine] = false;
        }
        for (unsigned later = frame; later <= s.lastInputFrame; ++later)
        {
            const MachineInput input = Predicted(machine);
            if (!Same(InputsFor(later).machines[machine], input)) changed(later);
            InputsFor(later).machines[machine] = input;
        }
        s.firstPredicted[machine] = frame <= s.lastInputFrame ? frame : 0;
    }
    if (!earliest || earliest > s.latest) return;
    for (unsigned f = earliest; f <= s.latest; ++f) Compose(InputsFor(f), s.steps[f % kRing].inputs);
    ++s.correctionRollbacks;
    RollBack(earliest - 1, s.latest, s.replay, s.checksum);
}

// The time sync. A machine that runs ahead predicts the others' inputs for
// longer, and rolls back further, than one behind it, which gets their inputs
// early. Each machine measures how far ahead of it each other machine's
// inputs come (messages waiting less steps predicted: the input delay less
// the latency less how far it runs ahead), and sends what it measures; half
// the difference between the two measures is how far it runs ahead, the
// latency cancelled. The one ahead slows its steps by a tenth until even.
void SyncTime()
{
    State& s = *sState;
    NetworkInputRouter* router = Router();
    const int local = g_pNetworkSessionBase->GetLocalMachineId();
    s.lead = 0;
    for (int machine = 0; machine < MachineCount(); ++machine)
    {
        if (machine == local) continue;
        const unsigned first = s.firstPredicted[machine];
        const float ahead = first ? -static_cast<float>(s.lastInputFrame - first + 1)
                                  : static_cast<float>(router->m_InputQueue[machine].GetCount());
        s.queuedAhead[machine] += (ahead - s.queuedAhead[machine]) * 0.05f;
        if (s.haveReported[machine])
            s.lead = std::max(s.lead, (s.reportedAhead[machine] - s.queuedAhead[machine]) / 2);
    }
    if (!s.active || !PredictablePhases(CurrentPhases())) return;
    s.leadSum += s.lead;
    s.maxLead = std::max(s.maxLead, static_cast<double>(s.lead));
    ++s.leadSamples;
    if (s.lead > 0.5f)
        s.slowing = true;
    else if (s.lead < 0.2f)
        s.slowing = false;
    if (!s.slowing) return;
    auto* fixed = static_cast<FixedUpdateTask*>(s.tasks[fixedIndexOf()]);
    const float removed = 0.1f * std::max(Frame(s.hostFrame).dt[fixedIndexOf()], 0.0f);
    fixed->mAccumulatedDeltaT -= removed;
    s.slowedSeconds += removed;
}

// RECHARGED_ROLLBACK, at the start of a frame, before any task sets up its
// cameras and views.
void NetplayFrameStart()
{
    State& s = *sState;
    // Steps the hook between steps did not see (one whose presentation stopped
    // the simulation): undone when they predicted, the match goes on without
    // the rollback otherwise.
    const unsigned now = CurrentStep();
    if (now != s.latest)
    {
        bool predicted = false;
        for (unsigned f = s.latest + 1; f <= now; ++f)
            predicted = predicted || (InputsFor(f).frame == f && AnyPredicted(InputsFor(f)));
        if (!predicted || now < s.latest)
        {
            Stop("a step the rollback did not see");
            return;
        }
        ++s.undone;
        RollBack(s.latest, s.latest, s.replay, s.checksum);
    }
    // A step that predicted and changed the match's phase: it runs again when
    // its inputs have all arrived.
    if (s.undoFrom)
    {
        const unsigned from = s.undoFrom;
        s.undoFrom = 0;
        if (from <= s.latest && from > 0)
        {
            ++s.undone;
            RollBack(from - 1, from - 1, s.replay, s.checksum);
        }
    }
    Resolve();
    UpdateConfirmed();
    SyncTime();
}

void* Watchdog(void*)
{
    for (;;)
    {
        timespec pause{0, 50'000'000};
        nanosleep(&pause, nullptr);
        std::lock_guard lock(sMutex);
        if (!sState->out) return nullptr;
        if (sState->active && NowNs() - sState->lastHookNs > kIdleNs && !OldestPredicted())
            Stop("no step for 250 ms");
    }
}

// The game module's own code that throws, logged while the test runs: the
// runtime only reports what() when the game stops.
constexpr int kListedThrows = 8;
int sThrows;

void LogThrow(const std::type_info* type)
{
    State& s = *sState;
    void* frames[24];
    const int depth = backtrace(frames, 24);
    Dl_info self{};
    dladdr(reinterpret_cast<void*>(&LogThrow), &self);
    std::fprintf(s.out, "THROW %s at frame %u, call stack in the game module:", type ? type->name() : "?",
                 s.latest);
    for (int i = 2; i < depth; ++i)
    {
        Dl_info info{};
        if (dladdr(frames[i], &info) && info.dli_fbase == self.dli_fbase)
            std::fprintf(s.out, " +0x%lx",
                         static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(frames[i])
                                                    - reinterpret_cast<std::uintptr_t>(self.dli_fbase) - 1));
    }
    std::fprintf(s.out, "\n");
    std::fflush(s.out);
}

struct Install
{
    Install()
    {
        const char* path = std::getenv("RECHARGED_ROLLBACK_TEST");
        const char* network = std::getenv("RECHARGED_ROLLBACK");
        const bool netplay = network && *network;
        if (netplay && path && *path)
        {
            std::fprintf(stderr, "RECHARGED_ROLLBACK: not with RECHARGED_ROLLBACK_TEST\n");
            return;
        }
        if (netplay) path = network;
        if (!path || !*path) return;
        if (const char* synctest = std::getenv("RECHARGED_SYNCTEST"); synctest && *synctest)
        {
            std::fprintf(stderr, "RECHARGED_ROLLBACK_TEST: not with RECHARGED_SYNCTEST\n");
            return;
        }
        auto* state = static_cast<State*>(std::calloc(1, sizeof(State)));
        if (!state) return;
        const std::size_t length = std::strlen(path);
        state->summaryPath = static_cast<char*>(std::malloc(length + sizeof(".summary")));
        state->out = std::fopen(path, "w");
        if (!state->summaryPath || !state->out)
        {
            std::fprintf(stderr, "RECHARGED_ROLLBACK_TEST: cannot prepare %s\n", path);
            return;
        }
        std::memcpy(state->summaryPath, path, length);
        std::memcpy(state->summaryPath + length, ".summary", sizeof(".summary"));
        state->maxDepth = EnvInt("RECHARGED_ROLLBACK_TEST_FRAMES", kMaxDepth, 1, kMaxDepth);
        state->interval = EnvInt("RECHARGED_ROLLBACK_TEST_INTERVAL", 1, 1, 1 << 20);
        state->compareEvery = EnvInt("RECHARGED_ROLLBACK_TEST_COMPARE", 0, 0, 1 << 20);
        state->merge = EnvInt("RECHARGED_ROLLBACK_TEST_MERGE", 1, 0, 1) != 0;
        state->netplay = netplay;
        state->inputDelay = EnvInt("RECHARGED_ROLLBACK_DELAY", 1, 0, 8);
        if (netplay) state->compareEvery = 0;
        if (state->compareEvery)
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&mscharged_rollback_active), &info))
                state->moduleBase = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
            state->places = static_cast<Place*>(std::calloc(kMaxPlaces, sizeof(Place)));
        }
        if (netplay)
            std::fprintf(state->out, "rollback: network matches predict, input delay %d steps; summary in %s\n",
                         state->inputDelay, state->summaryPath);
        else
            std::fprintf(state->out, "rollback test: up to %d steps back every %d steps; summary in %s\n",
                         state->maxDepth, state->interval, state->summaryPath);
        std::fflush(state->out);
        sState = state;
        pthread_t watchdog;
        if (pthread_create(&watchdog, nullptr, Watchdog, nullptr) == 0)
            pthread_detach(watchdog); // a pthread: std::thread would allocate its state with nlMalloc
    }
    ~Install()
    {
        if (!sState) return;
        std::lock_guard lock(sMutex);
        if (!sState->out) return;
        Stop("exit");
        WriteSummary();
        std::fclose(sState->out);
        sState->out = nullptr;
    }
} sInstall;
}

extern "C" bool mscharged_rollback_active(void)
{
    return sState && sState->out;
}

extern "C" bool mscharged_rollback_resimulating(void)
{
    return sState && sState->resimulating;
}

extern "C" void mscharged_rollback_inputs_applied(const MschargedRollbackInputs* inputs)
{
    if (!sState || !sState->out || sState->resimulating) return;
    std::lock_guard lock(sMutex);
    sState->pending = *inputs;
    sState->haveInputs = true;
}

extern "C" void mscharged_rollback_task_time(const void* task, float dt)
{
    if (!sState || !sState->out || sState->resimulating) return;
    std::lock_guard lock(sMutex);
    State& s = *sState;
    if (!s.active) return;
    RestoreSeeds();
    FrameRecord& record = Frame(s.hostFrame);
    for (int i = 0; i < s.taskCount; ++i)
        if (task == s.tasks[i])
        {
            record.dt[i] = dt;
            record.real[i] = nlTaskManager::m_pInstance->mRealTimeDelta;
            if (s.replays[i] == kSkip)
            {
                s.seedsSaved = true;
                s.defaultSeed = nlDefaultSeed;
                s.effectsSeed = gEffectsRandomSeed;
            }
            return;
        }
}

extern "C" void mscharged_rollback_meter_received(const bool* values, int count)
{
    if (!sState || !sState->out) return;
    std::lock_guard lock(sMutex);
    State& s = *sState;
    s.meterReceived += count;
    for (int i = 0; i < count; ++i)
    {
        if (s.receivedCount == kMaxMeter)
        {
            s.receivedOverflowed = true;
            return;
        }
        s.received[s.receivedCount++] = values[i];
    }
}

extern "C" void mscharged_rollback_between_steps(int (*replay)(const MschargedRollbackInputs* inputs),
                                                 unsigned (*checksum)(void), void* task, unsigned long taskBytes)
{
    if (!sState || !sState->out) return;
    if (sState->netplay && !mscharged_rollback_predicting()) return; // offline: nothing to predict
    std::lock_guard lock(sMutex);
    State& s = *sState;
    s.lastHookNs = NowNs();
    auto* fixed = static_cast<FixedUpdateTask*>(task);
    const unsigned frame = static_cast<unsigned>(fixed->GetFrame());
    if (s.active && (frame != s.latest + 1 || !s.haveInputs)) Stop("a step the rollback did not see");
    if (!s.active)
    {
        if (!Start(fixed, taskBytes, checksum))
        {
            std::fclose(s.out);
            s.out = nullptr;
        }
        else if (s.netplay)
        {
            s.replay = replay;
            s.checksum = checksum;
            UpdateConfirmed();
        }
        return;
    }

    ++s.stepCount;
    if (s.stepCount % kUntrackedEvery == 0)
    {
        std::uintptr_t first = 0;
        s.untracked += mscharged::platform::FindUntrackedRollbackWrites(&first);
    }
    Step& step = s.steps[frame % kRing];
    step.frame = frame;
    step.snapshot = mscharged::platform::SaveRollbackSnapshot();
    step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
    step.inputs = s.pending;
    std::copy_n(s.received, s.receivedCount, step.meter);
    step.meterCount = s.receivedCount;
    step.meterOverflowed = s.receivedOverflowed;
    s.receivedCount = 0;
    s.receivedOverflowed = false;
    step.hostFrame = s.hostFrame;
    step.checksum = checksum();
    step.seed = GetNetworkRandomSeed();
    RecordFrame(step);
    // A step that predicted must leave the match's phase alone: undone at the
    // frame start, it runs again when its inputs have all arrived.
    if (s.netplay && AnyPredicted(InputsFor(frame)) && !s.undoFrom
        && !(step.phases == s.steps[(frame - 1) % kRing].phases))
        s.undoFrom = frame;
    s.haveInputs = false;
    s.latest = frame;
    ++s.stored;
    // No rollback returns past the oldest step kept: the registry forgets the journal before it.
    const unsigned oldest = frame - static_cast<unsigned>(std::min(s.stored, kRing) - 1);
    mscharged::platform::TrimGameAllocationCheckpoint(s.steps[oldest % kRing].mark);

    // The rollback waits for the start of the next frame: here the frame has
    // set up its cameras and views already, which a restore would take back.
    s.replay = replay;
    s.checksum = checksum;
    s.pendingDepth = 0;
    if (s.netplay)
    {
        UpdateConfirmed();
        if (s.stepCount % kSummaryEvery == 0) WriteSummary();
        return;
    }
    if (s.stepCount % s.interval == 0)
    {
        const int depth = s.nextDepth % s.maxDepth + 1;
        if (depth < s.stored)
        {
            ++s.nextDepth;
            s.pendingDepth = depth;
        }
        else
            ++s.skippedShort;
    }
    if (s.stepCount % kSummaryEvery == 0) WriteSummary();
}

extern "C" void mscharged_rollback_frame_start(void)
{
    if (!sState || !sState->out) return;
    std::lock_guard lock(sMutex);
    State& s = *sState;
    if (!s.active) return;
    RestoreSeeds(); // after the frame's last task
    // The frame that ended, then the rollback it asked for, then the frame
    // that starts now.
    const auto* fixed = static_cast<const FixedUpdateTask*>(s.tasks[fixedIndexOf()]);
    Frame(s.hostFrame).accumulated = fixed->mAccumulatedDeltaT;
    Frame(s.hostFrame).interpolation = fixed->mInterpolationDeltaT;
    if (s.netplay)
        NetplayFrameStart();
    else if (s.pendingDepth)
    {
        const int depth = s.pendingDepth;
        s.pendingDepth = 0;
        RollBack(s.latest - static_cast<unsigned>(depth), s.latest, s.replay, s.checksum);
    }
    ++s.hostFrame;
    BeginFrameRecord();
}
extern "C" bool mscharged_rollback_predicting(void)
{
    if (!sState || !sState->out || !sState->netplay || !g_pNetworkSessionBase || !gNetworkInputRouter) return false;
    return g_pNetworkSessionBase->GetSessionMode() != NET_MODE_LOCAL && !gNetworkInputRecording->mPlaybackReady;
}

extern "C" int mscharged_rollback_network_reset(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    if (s.active) Stop("a new match");
    for (StepInputs& inputs : s.inputs) inputs.frame = ~0u;
    s.lastInputFrame = 0;
    std::fill(std::begin(s.firstPredicted), std::end(s.firstPredicted), 0u);
    s.haveLastReal = false;
    s.waitThrough = s.undoFrom = 0;
    s.captured = s.haveConfirmed = false;
    s.remoteCount = 0;
    std::fill(std::begin(s.queuedAhead), std::end(s.queuedAhead), 0.0f);
    std::fill(std::begin(s.haveReported), std::end(s.haveReported), false);
    s.lead = 0;
    s.slowing = false;
    std::fprintf(s.out, "network match: input delay %d steps\n", s.inputDelay);
    std::fflush(s.out);
    return s.inputDelay;
}

extern "C" bool mscharged_rollback_has_input(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    NetworkInputRouter* router = Router();
    if (router->mQueueCursor < router->mQueueLimit) return true; // the input delay's first steps
    const unsigned frame = CurrentStep() + 1;
    if (!CanPredict(frame))
    {
        // As the original router: every machine's inputs for the step, and
        // none still predicted before it.
        bool ready = !OldestPredicted();
        for (int machine = 0; ready && machine < MachineCount(); ++machine)
        {
            const StepInputs& kept = InputsFor(frame);
            ready = (kept.frame == frame && !kept.predicted[machine]) || router->m_InputQueue[machine].GetCount() > 0;
        }
        if (!ready) ++s.lockstepWaits;
        return ready;
    }
    if (const unsigned oldest = OldestPredicted(); oldest && frame - oldest + 1 >= static_cast<unsigned>(kMaxDepth))
    {
        ++s.windowWaits;
        return false;
    }
    return true;
}

extern "C" void mscharged_rollback_input_ready(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    NetworkInputRouter* router = Router();
    const int machines = MachineCount();
    if (!s.haveLastReal)
    {
        for (int machine = 0; machine < machines; ++machine) s.lastReal[machine] = FromRouter(machine);
        s.haveLastReal = true;
    }
    const unsigned frame = CurrentStep() + 1;
    StepInputs& kept = InputsFor(frame);
    // A step undone and run again: the inputs that arrived for it then.
    const bool again = kept.frame == frame;
    StepInputs step{};
    step.frame = frame;
    step.machineCount = machines;
    if (router->mQueueCursor < router->mQueueLimit)
    {
        ++router->mQueueCursor;
        for (int machine = 0; machine < machines; ++machine) step.machines[machine] = Predicted(machine);
    }
    else
        for (int machine = 0; machine < machines; ++machine)
        {
            NetworkInputMessageQueue& queue = router->m_InputQueue[machine];
            if (again && !kept.predicted[machine])
            {
                step.machines[machine] = kept.machines[machine];
                s.lastReal[machine] = kept.machines[machine];
                Mirror(machine, kept.machines[machine]);
            }
            else if (!s.firstPredicted[machine] && queue.GetCount() > 0)
                step.machines[machine] = Arrived(machine, queue.Pop());
            else
            {
                step.machines[machine] = Predicted(machine);
                step.predicted[machine] = true;
                if (!s.firstPredicted[machine]) s.firstPredicted[machine] = frame;
            }
        }
    kept = step;
    if (AnyPredicted(step))
    {
        ++s.predictedSteps;
        ++s.predictedDepth[std::min<unsigned>(frame - OldestPredicted() + 1, kMaxDepth)];
    }
    Compose(step, s.pending);
    ApplyRollbackInputs(&s.pending);
    s.haveInputs = true;
    s.lastInputFrame = frame;
}

extern "C" bool mscharged_rollback_can_capture(void)
{
    std::lock_guard lock(sMutex);
    State& s = *sState;
    const unsigned frame = CurrentStep();
    if (s.captured && frame <= s.capturedFrame) return false;
    s.captured = true;
    s.capturedFrame = frame;
    return true;
}

extern "C" int mscharged_rollback_sync_data(unsigned* checksum, unsigned* frame, unsigned* seed)
{
    std::lock_guard lock(sMutex);
    const State& s = *sState;
    // Every other step the checksum, the others the time sync's measure (the
    // other machines' average, by 256ths of a step).
    if (CurrentStep() % 2 == 0 && s.haveConfirmed)
    {
        const SyncPoint& point = s.history[s.confirmed % kConfirmedRing];
        if (point.frame != s.confirmed) return 0;
        *checksum = point.checksum;
        *frame = point.frame;
        *seed = point.seed;
        return 1;
    }
    float ahead = 0;
    int others = 0;
    for (int machine = 0; machine < MachineCount(); ++machine)
        if (machine != g_pNetworkSessionBase->GetLocalMachineId())
        {
            ahead += s.queuedAhead[machine];
            ++others;
        }
    *frame = static_cast<unsigned>(static_cast<std::int32_t>(std::lround(ahead / std::max(others, 1) * 256.0f)));
    return 2;
}

// Calls to __cxa_throw from the game module bind to this one (hidden), which
// logs the throw and goes on to the C++ runtime's.
extern "C" __attribute__((visibility("hidden"), noreturn)) void __cxa_throw(void* thrown, std::type_info* type,
                                                                           void (*destroy)(void*))
{
    if (sState && sState->out && sState->active && sThrows++ < kListedThrows) LogThrow(type);
    using Throw = void (*)(void*, std::type_info*, void (*)(void*));
    static const Throw next = reinterpret_cast<Throw>(dlsym(RTLD_DEFAULT, "__cxa_throw"));
    next(thrown, type, destroy);
    __builtin_unreachable();
}
#endif
