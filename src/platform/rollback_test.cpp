// RECHARGED_ROLLBACK_TEST=<file> rolls matches back while they are played,
// offline and in network matches, to test the rollback before it predicts
// anything: after every fixed step it saves the match (platform/rollback_snapshot.h),
// and every RECHARGED_ROLLBACK_TEST_INTERVAL steps (default 1) it goes back 1
// to RECHARGED_ROLLBACK_TEST_FRAMES steps (default 8, in turn) and runs them
// again with the inputs they first had (tweak patch 0032). The game's checksum
// after each replayed step must be the one the step first gave; in a network
// match the machines also compare their checksums as the original game does.
// <file> logs the session; <file>.summary, rewritten every ten seconds, holds
// its counts and costs. Unset, it changes nothing. Diagnostic only.
//
// The rollback restores the original game's arenas and globals, as the sync
// test saves them, except what lives on outside the simulation: the network
// session (but its players' inputs), sockets and lobby, the input routers and
// manager, the pads, the tasks' clocks and the fixed update task's
// accumulated time, the drawing's memory (frame memory and resource pools,
// whose suballocations the port tracks for the host) with the bookkeeping of
// the pools drawing fills every frame (the goal nets' in
// cmake/rollback_sections.ld, the crowd impostors' here), and the audio
// memory. Everything else, the drawing's objects included, goes back with the
// match.
//
// A replayed step reads no pads, captures and sends nothing, and queues no
// deterministic data (it went out the first time). Around it the replay runs
// what the frame's other tasks do to the match and its presentation (cameras,
// NIS, particles, HUD, scenes, stadium, events), without drawing, with the
// time step each frame first had (tweak patch 0033). One presentation update
// per step: at a frame rate above the steps' the replayed presentation runs
// fewer, longer updates.
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
extern "C" void mscharged_rollback_between_steps(int (*)(const MschargedRollbackInputs*), unsigned (*)(void), void*,
                                                 unsigned long)
{
}
#else
#include "Game/Audio/AudioBackend.h"
#include "Game/Game.h"
#include "Game/InputManager.h"
#include "Game/InputRouter.h"
#include "Game/NetworkSession.h"
#include "Game/NetworkSync.h"
#include "Game/Render/ImpostorManager.h"
#include "Game/Task/FixedUpdateTask.h"
#include "NL/MemAlloc.h"
#include "NL/globalpad.h"
#include "NL/nlMemory.h"
#include "NL/nlTask.h"
#include "NL/platpad.h"
#include "platform/game_allocation_ownership.h"
#include "platform/rollback_snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <time.h>

// Bounds of the simulation's globals (cmake/rollback_sections.ld).
extern "C" char __rbk_data_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_data_end[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_end[] __attribute__((weak, visibility("hidden")));
// The routers (InputRouter.cpp).
extern InputRouter* gSimpleInputRouter;
extern NetworkInputRouter* gNetworkInputRouter;

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
constexpr int kSummaryEvery = 600;       // steps (ten seconds)
constexpr int kUntrackedEvery = 600;     // steps between checks for writes behind the tracking (reads everything)
constexpr int kListedMismatches = 20;

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
    unsigned checksum;              // after it, as it first ran
    Phases phases;                  // after it
    bool reading;                   // a disc read in flight after it
    std::uint64_t readsStarted;     // disc reads started until then
    float frameTime;                // the time step of the tasks in its frame
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
    Step steps[kRing];
    unsigned latest;  // frame of the last step saved
    int stored;       // consecutive steps saved since tracking started
    int nextDepth;
    std::uint64_t lastHookNs;

    // Counts and costs.
    int starts;
    std::uint64_t stepCount, rollbacks, byDepth[kMaxDepth + 1], replayed;
    std::uint64_t skippedReads, skippedStarts, skippedPhases, skippedEvents, skippedShort;
    std::uint64_t mismatches, nisInReplay, lossyFrees, failedRestores, untracked;
    unsigned firstMismatchFrame;
    std::uint64_t rollbackNs, maxRollbackNs, restoreNs, maxRestoreNs;
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
    // The drawing's memory: the port tracks what it holds for the host (graphics
    // suballocations), as it is now; restoring a pool's top would allocate over them.
    const void* graphics[kMaxExcluded];
    const int graphicsCount = mscharged_rollback_graphics_memory(graphics, kMaxExcluded);
    for (int i = 0; i < graphicsCount; ++i) ExcludeAllocation(graphics[i], 0, "drawing memory");
    // The crowd impostors fill one of two of those pools every frame and release the
    // other to a mark: their pools, marks, turn and capture state stay together.
    if (ImpostorManager* impostors = ImpostorManager::GetInstance())
        Exclude(&impostors->mResources, sizeof(ImpostorManager) - offsetof(ImpostorManager, mResources),
                "crowd impostors' pools and capture");
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

// What a rollback past this step must check, and the time its frame's tasks ran with.
void RecordFrame(Step& step)
{
    step.phases = CurrentPhases();
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    step.reading = producers.pending != 0;
    step.readsStarted = producers.started;
    step.frameTime = nlTaskManager::m_pInstance ? nlTaskManager::m_pInstance->mCurrentTimeDelta : 1.0f / 60.0f;
}

void Record(std::uint64_t value, std::uint64_t& total, std::uint64_t& max)
{
    total += value;
    max = std::max(max, value);
}

void WriteSummary()
{
    const State& s = *sState;
    FILE* file = std::fopen(s.summaryPath, "w");
    if (!file) return;
    const auto& h = mscharged::platform::RollbackSnapshotStatistics();
    std::fprintf(file, "rollback test: up to %d steps back every %d steps\n", s.maxDepth, s.interval);
    std::fprintf(file, "%llu steps, tracking started %d times; %d regions, %zu pages tracked, %zu hot\n",
                 static_cast<unsigned long long>(s.stepCount), s.starts, s.regionCount, h.trackedPages, h.hotPages);
    std::fprintf(file, "rollbacks: %llu, steps replayed %llu; by steps undone:",
                 static_cast<unsigned long long>(s.rollbacks), static_cast<unsigned long long>(s.replayed));
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
    std::fprintf(file, "faults %llu, cold pages turned hot %llu, hot turned cold %llu, saves dropped early %llu\n",
                 static_cast<unsigned long long>(h.faults), static_cast<unsigned long long>(h.promoted),
                 static_cast<unsigned long long>(h.demoted), static_cast<unsigned long long>(h.evicted));
    std::fprintf(file, "\nnot restored:\n");
    for (int i = 0; i < s.excludedCount; ++i)
        std::fprintf(file, "  %s: %zu bytes\n", s.excluded[i].name,
                     static_cast<std::size_t>(s.excluded[i].end - s.excluded[i].start));
    std::fprintf(file, "restored inside those: %d ranges (the network players' inputs)\n", s.keptCount);
    std::fclose(file);
}

bool Start(FixedUpdateTask* task, unsigned long taskBytes, unsigned (*checksum)(void))
{
    State& s = *sState;
    FindRanges(task, taskBytes);
    if (!BuildRegions()
        || !mscharged::platform::ConfigureRollbackSnapshots(s.regions, s.regionCount, kMaxDepth + 1,
                                                            static_cast<std::size_t>(kMaxDepth + 1) * 512 + 2048))
    {
        std::fprintf(s.out, "rollback test: cannot track the match; stopped\n");
        std::fflush(s.out);
        return false;
    }
    const unsigned frame = static_cast<unsigned>(task->GetFrame());
    mscharged::platform::BeginGameAllocationCheckpoint();
    Step& step = s.steps[frame % kRing];
    step.frame = frame;
    step.snapshot = mscharged::platform::BeginRollbackSnapshots();
    step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
    step.checksum = checksum();
    RecordFrame(step);
    s.latest = frame;
    s.stored = 1;
    s.haveInputs = false;
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
    std::fprintf(s.out, "tracking stopped after frame %u: %s\n", s.latest, why);
    std::fflush(s.out);
    WriteSummary();
}

// Back `depth` steps and forward again with the same inputs.
void RollBack(int depth, int (*replay)(const MschargedRollbackInputs*), unsigned (*checksum)(void))
{
    State& s = *sState;
    const unsigned target = s.latest - depth;
    for (unsigned f = target; f <= s.latest; ++f)
    {
        const Step& step = s.steps[f % kRing];
        if (step.reading)
        {
            ++s.skippedReads;
            return;
        }
        // A read started and done in these steps: its completion went into memory this restores.
        if (step.readsStarted != s.steps[target % kRing].readsStarted)
        {
            ++s.skippedStarts;
            return;
        }
        if (!(step.phases == s.steps[s.latest % kRing].phases))
        {
            ++s.skippedPhases;
            return;
        }
        if (f > target && step.inputs.overflowed)
        {
            ++s.skippedEvents;
            return;
        }
    }

    const std::uint64_t start = NowNs();
    const Step& from = s.steps[target % kRing];
    if (!mscharged::platform::RestoreRollbackSnapshot(from.snapshot))
    {
        ++s.failedRestores;
        Stop("a save to roll back to was gone");
        return;
    }
    s.lossyFrees += mscharged::platform::RollbackGameAllocationCheckpointTo(from.mark, Excluded);
    Record(NowNs() - start, s.restoreNs, s.maxRestoreNs);

    s.resimulating = true;
    for (unsigned f = target + 1; f <= s.latest; ++f)
    {
        Step& step = s.steps[f % kRing];
        step.inputs.previousFrameTime = s.steps[(f - 1) % kRing].frameTime;
        step.inputs.frameTime = step.frameTime;
        if (replay(&step.inputs) < 0) ++s.nisInReplay;
        const unsigned now = checksum();
        if (now != step.checksum)
        {
            if (!s.mismatches) s.firstMismatchFrame = f;
            if (s.mismatches++ < kListedMismatches)
                std::fprintf(s.out, "REPLAYED FRAME %u (%d back from %u): checksum %08x, first run %08x\n", f, depth,
                             s.latest, now, step.checksum);
        }
        step.snapshot = mscharged::platform::SaveRollbackSnapshot();
        step.mark = mscharged::platform::MarkGameAllocationCheckpoint();
        ++s.replayed;
    }
    s.resimulating = false;
    ++s.rollbacks;
    ++s.byDepth[depth];
    Record(NowNs() - start, s.rollbackNs, s.maxRollbackNs);
}

void* Watchdog(void*)
{
    for (;;)
    {
        timespec pause{0, 50'000'000};
        nanosleep(&pause, nullptr);
        std::lock_guard lock(sMutex);
        if (!sState->out) return nullptr;
        if (sState->active && NowNs() - sState->lastHookNs > kIdleNs) Stop("no step for 250 ms");
    }
}

struct Install
{
    Install()
    {
        const char* path = std::getenv("RECHARGED_ROLLBACK_TEST");
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

extern "C" void mscharged_rollback_between_steps(int (*replay)(const MschargedRollbackInputs* inputs),
                                                 unsigned (*checksum)(void), void* task, unsigned long taskBytes)
{
    if (!sState || !sState->out) return;
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
    step.checksum = checksum();
    RecordFrame(step);
    s.haveInputs = false;
    s.latest = frame;
    ++s.stored;

    if (s.stepCount % s.interval == 0)
    {
        const int depth = s.nextDepth % s.maxDepth + 1;
        if (depth < s.stored)
        {
            ++s.nextDepth;
            RollBack(depth, replay, checksum);
        }
        else
            ++s.skippedShort;
    }
    if (s.stepCount % kSummaryEvery == 0) WriteSummary();
}
#endif
