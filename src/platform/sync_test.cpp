// RECHARGED_SYNCTEST=<file> runs the rollback sync test during matches and
// writes its results to <file>. Every RECHARGED_SYNCTEST_INTERVAL fixed steps
// (default 250, five seconds) it saves the match, runs RECHARGED_SYNCTEST_FRAMES
// steps (default 8), restores the match, runs the same steps again and compares
// the game's checksum after each step and the memory at the end. Whatever
// differs is match state the save did not capture. Unset, it changes nothing.
// Diagnostic only (rollback study, tweak patch 0028).
//
// <file> logs every test (in full for the first RECHARGED_SYNCTEST_DETAILS
// failing ones, default 3) and every match with its captains, sidekicks and
// stadium. <file>.summary, rewritten after each test, aggregates the session:
// every global and allocation that ever differed and in how many tests, the
// matches, and which captains, sidekicks and stadiums no test covered yet.
//
// The save is the two original game arenas (MEM1 and MEM2 heaps) and the
// simulation's globals, which cmake/rollback_sections.ld gathers in the
// module's rbk_data and rbk_bss sections (without them, as on other linkers,
// only the allocator globals and the fixed update task). The game's audio
// memory and its AudioBackend are kept as they are, as Slippi keeps the audio
// regions: the host is still playing what they describe. The rest of the
// module's globals is compared, never restored.
//
// RECHARGED_SYNCTEST_HISTORY=1 saves and restores through the incremental
// snapshots (platform/rollback_snapshot.h) instead of full copies: a save after
// every step of the first run, then a rollback to one of those saves, a rerun,
// and a second rollback of another depth and a rerun again, before the same
// comparisons.
//
// RECHARGED_SYNCTEST_AUDIT=1 runs no test: through a whole match the
// incremental snapshots track the saved memory and, at the start and at the
// end of every simulation step (CallFixedUpdateTasks), note what changed since
// the other. <file>.summary then lists what the rest of the frame writes (the
// network task, drawing, the front end, the next step's pads and inputs, other
// threads): a rollback that restores it undoes work that is not simulation.
// Each place says whether the simulation writes it too; and the allocations
// and frees made outside the steps and on other threads, by allocator and
// call site, which a rollback would undo under code that still uses them.
// The game plays as
// usual, sounds and all, offline and in network matches; tracking stops when
// no step comes for a quarter of a second (pause, end of the match).
//
// Matches under the test use the deterministic AI clocks of network games
// (tweak patch 0030); offline the AI otherwise skips scripts by real time.
//
// Audio is not part of the rollback yet: under the test the simulation makes
// no sounds (tweak patch 0029) and interrupts stay off during a test, so no
// audio state changes between a save and its restore. A test whose first run
// starts a disc read, or changes the game's or the task manager's state (a
// goal, a NIS), is not rolled back: no rollback can undo the read, nor the
// presentation and loading such a change starts.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The sync test belongs to the isolated original game module
#endif
#include "mscharged_synctest.h"
#include "mscharged_rollback.h"

#if defined(_WIN32)
extern "C" void mscharged_synctest_between_steps(int (*)(void), unsigned (*)(void), void*, unsigned long) {}
extern "C" void mscharged_synctest_fixed_step(int) {}
extern "C" bool mscharged_synctest_silences_sounds(void) { return false; }
extern "C" bool mscharged_synctest_active(void) { return false; }
#else
#include "Game/Audio/AudioBackend.h"
#include "Game/Audio/AudioGlobals.h"
#include "Game/DB/BasicGameInfo.h"
#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/Task/FixedUpdateTask.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlTask.h"
#include "platform/game_allocation_ownership.h"
#include "platform/rollback_snapshot.h"
#include <dolphin/os.h>

#include <dlfcn.h>
#include <execinfo.h>
#if defined(__linux__)
#include <link.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <time.h>

// Bounds of the simulation's globals (cmake/rollback_sections.ld); weak, so a
// module linked without the script has none.
extern "C" char __rbk_data_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_data_end[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_bss_end[] __attribute__((weak, visibility("hidden")));
// The audio code's globals, compared but never restored.
extern "C" char __rbk_audio_data_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_audio_data_end[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_audio_bss_start[] __attribute__((weak, visibility("hidden")));
extern "C" char __rbk_audio_bss_end[] __attribute__((weak, visibility("hidden")));

// No std containers: in the game module the global operator new is the
// original nlMalloc, and the test must not allocate in the arenas it saves.
namespace
{
constexpr int kMaxFrames = 64;
constexpr int kMaxRanges = 16;
constexpr int kMaxPreserved = 4;
constexpr int kMaxModuleRanges = 8;
constexpr std::size_t kMaxDiffs = 8192;
constexpr std::size_t kMaxOwners = 512;
constexpr std::size_t kChunk = 4096;
constexpr std::size_t kMergeGap = 16;
constexpr std::size_t kMaxAggregates = 4096;
constexpr std::size_t kAggregateSlots = 2 * kMaxAggregates; // the aggregates' hash index
constexpr int kMaxMatches = 256;
constexpr int kMaxListedTests = 64;
constexpr int kCaptains = 12;
constexpr int kSidekicks = 8;
constexpr int kStadiums = 18;
constexpr std::size_t kListedOwners = 60;
constexpr std::size_t kListedUnowned = 30;
constexpr std::size_t kListedModule = 60;

struct Range
{
    std::uint8_t* base;
    std::size_t bytes;
    const char* name;
    std::size_t offset; // in the copy buffers
};
struct Diff
{
    std::uintptr_t start;
    std::size_t bytes;
    const char* region;
};
struct Owner
{
    std::uintptr_t base;
    std::size_t bytes;
    std::size_t diffBytes;
    std::size_t diffs;
    std::uintptr_t first;
    const char* region;
};

// One kind of difference over the whole session, so a session of many tests
// ends with every place that ever differed and how often.
enum AggregateKind
{
    kRestoredGlobal, // a game global the test restores (rbk sections)
    kModuleGlobal,   // a module global the test leaves alone
    kAllocation,     // an original allocation, by class (first word) and size
    kUnowned,        // arena bytes without a live allocation, by region
    kAudioTouched,   // an audio global the steps changed (the test was not rolled back)
    kAggregateKinds
};
const char* const kAggregateNames[kAggregateKinds] = {
    "restored game globals (differ after the replay: not deterministic or not saved)",
    "module globals outside the save (state the simulation changes and the save misses, or host noise)",
    "allocations (by first word, the vtable of a class, and size)",
    "arena memory without a live allocation (freed blocks, allocator internals)",
    "audio globals the simulation changed (those tests were not rolled back)",
};
// Where each original allocation of kTrackedBytes or more was made, recorded
// from the module's start (allocator observer), so a block that differs can
// be named by its call site even when it predates every test.
constexpr std::size_t kTrackedBytes = 256;
constexpr int kSiteDepth = 10;
constexpr int kSiteBits = 17;
constexpr std::size_t kSiteSlots = std::size_t{1} << kSiteBits;
constexpr std::uintptr_t kSiteTombstone = 1;
struct Callsite
{
    std::uintptr_t pointer; // 0: empty, kSiteTombstone: freed
    int depth;
    void* frames[kSiteDepth];
};
Callsite* sCallsites;
std::mutex sCallsiteMutex;
thread_local bool tRecordingCallsite;

std::size_t CallsiteSlot(std::uintptr_t pointer)
{
    return static_cast<std::size_t>(((pointer >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kSiteBits));
}

bool AuditingAllocations();
void AuditAllocation(bool allocate, unsigned long bytes, const MemoryAllocator* owner, void* const* frames, int depth);

void RecordAllocation(bool allocate, const void* pointer, unsigned long bytes, const MemoryAllocator* owner)
{
    if (tRecordingCallsite) return;
    const bool audit = AuditingAllocations();
    if (!audit && (bytes < kTrackedBytes || !sCallsites)) return;
    tRecordingCallsite = true;
    void* frames[kSiteDepth + 2];
    const int depth = allocate || audit ? backtrace(frames, kSiteDepth + 2) : 0;
    // Without this function and the registry's notifier.
    const int siteDepth = depth > 2 ? std::min(depth - 2, kSiteDepth) : 0;
    if (audit) AuditAllocation(allocate, bytes, owner, frames + 2, siteDepth);
    if (bytes < kTrackedBytes || !sCallsites)
    {
        tRecordingCallsite = false;
        return;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    {
        std::lock_guard lock(sCallsiteMutex);
        std::size_t free = kSiteSlots;
        for (std::size_t i = CallsiteSlot(address), n = 0; n < kSiteSlots; i = (i + 1) & (kSiteSlots - 1), ++n)
        {
            Callsite& site = sCallsites[i];
            if (site.pointer == address)
            {
                free = i;
                break;
            }
            if (site.pointer == kSiteTombstone && free == kSiteSlots) free = i;
            if (!site.pointer)
            {
                if (free == kSiteSlots) free = i;
                break;
            }
        }
        if (free != kSiteSlots)
        {
            Callsite& site = sCallsites[free];
            if (allocate)
            {
                site.pointer = address;
                site.depth = siteDepth;
                std::memcpy(site.frames, frames + 2, site.depth * sizeof(void*));
            }
            else if (site.pointer == address)
                site.pointer = kSiteTombstone;
        }
    }
    tRecordingCallsite = false;
}

bool FindCallsite(std::uintptr_t address, Callsite& result)
{
    if (!sCallsites) return false;
    std::lock_guard lock(sCallsiteMutex);
    for (std::size_t i = CallsiteSlot(address), n = 0; n < kSiteSlots; i = (i + 1) & (kSiteSlots - 1), ++n)
    {
        if (sCallsites[i].pointer == address)
        {
            result = sCallsites[i];
            return true;
        }
        if (!sCallsites[i].pointer) return false;
    }
    return false;
}

struct Aggregate
{
    int kind;
    std::uintptr_t key;    // module offset (globals, an allocation's first word) or range index
    std::size_t size;      // allocations: their size
    int tests;             // tests in which it differed
    int firstTest;
    int lastTest;
    std::size_t bytes;     // differing bytes over all those tests
    std::uintptr_t offset; // allocations: first differing offset, as first seen
    int depth;             // allocations: call site of the first one seen
    void* frames[kSiteDepth];
    int window;            // a test's differences, or an audit window (tests count steps there)
};
// Audit mode: a place the steps write, in a hash table (there are many: the
// simulation writes thousands of globals), only to tell whether a place
// written outside the steps is written inside too.
struct InsidePlace
{
    int kind; // 0: empty slot (kRestoredGlobal is stored as kAggregateKinds)
    std::uintptr_t key;
    std::size_t size;
    int steps;
    int lastStep;
};
constexpr std::size_t kInsideSlots = std::size_t{1} << 16;
// Audit mode: where a change was made.
enum AuditWindow
{
    kTestDiffs,    // not an audit: differences of a sync test
    kOutsideSteps, // from the end of a step to the start of the next
    kInsideSteps,  // during a step (CallFixedUpdateTasks)
    kOtherThreads, // allocations only: not the thread that runs the steps
    kAuditWindows
};
// Audit mode: allocations and frees while the match is tracked, by window,
// allocator and call site. Outside the steps they are what a rollback would
// undo under the code that still uses them (or redo, for a free).
constexpr std::size_t kMaxAllocSites = 1024;
struct AllocSite
{
    int window;
    bool allocate;
    const MemoryAllocator* owner;
    int depth;
    void* frames[kSiteDepth];
    std::uint64_t count, bytes;
    int steps, lastStep;
};
struct Match
{
    int teams[2];
    int sidekicks[2][3];
    int stadium;
    int firstTest;
    int tests;
    int failures;
};

const char* const kCaptainNames[kCaptains] = {"Mario", "Bowser", "Daisy", "Donkey Kong", "Luigi", "Peach",
                                              "Waluigi", "Wario", "Yoshi", "Bowser Jr.", "Diddy Kong", "Petey"};
const char* const kSidekickNames[kSidekicks] = {"Toad", "Koopa", "Hammer Bro", "Birdo",
                                                "Boo", "Dry Bones", "Monty Mole", "Shy Guy"};
const char* const kStadiumNames[kStadiums] = {
    "Battle Dome", "Bowser Stadium", "Crater Field", "Crystal Canyon", "The Dump", "Galactic Stadium",
    "Konga Coliseum", "Lava Pit", "Pipeline Central", "Storm Ship", "The Palace", "Thunder Island",
    "Underground", "The Vice", "Wastelands", "Sand Tomb", "Tutorial Field", "Low Poly Stadium"};

template<int N> const char* NameOf(const char* const (&names)[N], int index)
{
    return index >= 0 && index < N ? names[index] : "?";
}

struct State
{
    FILE* out;
    char* summaryPath; // <file>.summary, rewritten after every test
    int interval;
    int frames;
    int details; // failing tests reported in full
    int steps;
    int tests;
    int failures;
    int skipped;   // the first run started a disc read
    int stateSkipped; // the first run changed the game or task state
    int audioSkipped; // the first run changed audio globals
    // RECHARGED_SYNCTEST_PROFILE: the cost of each step and the 4 KB chunks it
    // writes, measured during first runs (what an incremental save would copy).
    bool profile;
    std::uint8_t* profileCopy; // the saved memory as of the previous step
    // RECHARGED_SYNCTEST_HISTORY: the saves and restores go through the
    // incremental snapshots (platform/rollback_snapshot), one save after every
    // step of the first run, and each test rolls back twice to different saves.
    bool history;
    std::uint64_t snapshotIds[kMaxFrames + 1]; // the save before step i
    mscharged::platform::GameAllocationMark marks[kMaxFrames + 1];
    std::uint64_t untracked; // pages written behind the tracking's back
    std::uintptr_t untrackedAt;
    std::uint64_t rollbackDepths[kMaxFrames + 1]; // rollbacks by steps undone
    std::uint64_t lostSnapshots; // rollbacks to a save no longer kept
    std::uint64_t trackedSteps, trackedStepNs, maxTrackedStepNs; // steps with tracking (faults included)
    // RECHARGED_SYNCTEST_AUDIT: tracking through the match, changes noted per
    // window (AuditWindow) into the aggregates; no test runs.
    bool audit;
    bool auditTracking;    // the snapshots track the match
    bool auditStepEnded;   // a step ended and the hook between steps followed it
    int noteWindow;        // the window Note files differences under
    int auditSteps;        // steps audited, the "test" number of their notes
    int auditStarts;       // times tracking started
    std::uint64_t auditBytes[kAuditWindows], auditMaxBytes[kAuditWindows]; // per window
    std::uint64_t auditNs, auditMaxNs;             // noting a window's changes
    std::uint64_t auditDropped;                    // ranges beyond kMaxDiffs, not named
    InsidePlace* inside;                           // kInsideSlots
    std::size_t insideCount;
    pthread_t auditThread;                         // the thread that runs the steps
    AllocSite* allocSites;                         // kMaxAllocSites
    std::size_t allocSiteCount;
    std::uint64_t allocCounts[kAuditWindows][2];   // per window: frees, allocations
    std::uint64_t allocDropped;                    // sites beyond kMaxAllocSites
    struct Profile
    {
        std::uint64_t steps, stepNs, maxStepNs;
        std::uint64_t chunks, maxChunks, copyNs, maxCopyNs;
        std::uint64_t over[3]; // steps writing more than 256 KB, 1 MB, 4 MB
        std::uint64_t saves, saveNs, maxSaveNs, restores, restoreNs, maxRestoreNs;
    } prof;
    Range audio[2];
    int audioCount;
    std::size_t audioTotal;
    std::uint8_t* audioSaved;
    int postponed; // steps waiting for disc reads in flight
    int checksumFailures;
    int checksumTests[kMaxListedTests];
    int lossyTests;
    Aggregate* aggregates;
    std::size_t aggregateCount;
    std::uint32_t* aggregateIndex; // kAggregateSlots, aggregate index + 1 (0: empty)
    Match matches[kMaxMatches];
    int matchCount;
    unsigned lastFrame;
    Range ranges[kMaxRanges];
    int rangeCount;
    std::size_t total;
    std::uint8_t* saved;  // the match before the test
    std::uint8_t* after;  // the match after the first run
    Range preserved[kMaxPreserved];
    int preservedCount;
    std::uint8_t* preservedCopy;
    std::size_t preservedCapacity;
    Range module[kMaxModuleRanges];
    int moduleCount;
    std::size_t moduleTotal;
    std::uint8_t* moduleAfter;
    std::uintptr_t moduleBase;
    std::uintptr_t moduleEnd; // past the last loaded segment (0: unknown)
    const char* moduleName;
    unsigned first[kMaxFrames];
    unsigned second[kMaxFrames];
    int firstResult[kMaxFrames];
    int secondResult[kMaxFrames];
    Diff* diffs;
    std::size_t diffCount;
    std::size_t diffTotal;
    Owner* owners;
    std::size_t ownerCount;
};
State* sState;
bool sInFixedStep; // inside a fixed simulation step (main thread only)
std::mutex sAllocSiteMutex;

bool AuditingAllocations()
{
    return sState && sState->auditTracking && sState->allocSites;
}

void AuditAllocation(bool allocate, unsigned long bytes, const MemoryAllocator* owner, void* const* frames, int depth)
{
    State& s = *sState;
    const int window = !pthread_equal(pthread_self(), s.auditThread) ? kOtherThreads
        : sInFixedStep                                                ? kInsideSteps
                                                                      : kOutsideSteps;
    std::lock_guard lock(sAllocSiteMutex);
    ++s.allocCounts[window][allocate];
    AllocSite* site = nullptr;
    for (std::size_t i = 0; i < s.allocSiteCount && !site; ++i)
    {
        AllocSite& candidate = s.allocSites[i];
        if (candidate.window == window && candidate.allocate == allocate && candidate.owner == owner
            && candidate.depth == depth && !std::memcmp(candidate.frames, frames, depth * sizeof(void*)))
            site = &candidate;
    }
    if (!site)
    {
        if (s.allocSiteCount == kMaxAllocSites)
        {
            ++s.allocDropped;
            return;
        }
        site = &s.allocSites[s.allocSiteCount++];
        *site = {window, allocate, owner, depth, {}, 0, 0, 0, 0};
        std::memcpy(site->frames, frames, depth * sizeof(void*));
    }
    ++site->count;
    site->bytes += bytes;
    if (site->lastStep != s.auditSteps)
    {
        ++site->steps;
        site->lastStep = s.auditSteps;
    }
}

int EnvInt(const char* name, int fallback, int low, int high)
{
    const char* text = std::getenv(name);
    if (!text || !*text) return fallback;
    return std::clamp(std::atoi(text), low, high);
}

// Saved ranges never overlap: one inside an earlier range is already saved.
void AddRange(void* base, std::size_t bytes, const char* name)
{
    State& s = *sState;
    if (!base || !bytes || s.rangeCount == kMaxRanges) return;
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    for (int i = 0; i < s.rangeCount; ++i)
    {
        const auto saved = reinterpret_cast<std::uintptr_t>(s.ranges[i].base);
        if (start >= saved && start - saved + bytes <= s.ranges[i].bytes) return;
    }
    s.ranges[s.rangeCount++] = {static_cast<std::uint8_t*>(base), bytes, name, 0};
}

bool Saved(std::uintptr_t address)
{
    const State& s = *sState;
    for (int i = 0; i < s.rangeCount; ++i)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(s.ranges[i].base);
        if (address >= base && address - base < s.ranges[i].bytes) return true;
    }
    return false;
}

bool InGameGlobals(const Range& range)
{
    return range.base == reinterpret_cast<std::uint8_t*>(__rbk_data_start)
        || range.base == reinterpret_cast<std::uint8_t*>(__rbk_bss_start);
}

void AddPreserved(void* base, std::size_t bytes, const char* name)
{
    State& s = *sState;
    if (!base || !bytes || s.preservedCount == kMaxPreserved) return;
    s.preserved[s.preservedCount++] = {static_cast<std::uint8_t*>(base), bytes, name, 0};
}

bool Preserved(std::uintptr_t address)
{
    const State& s = *sState;
    for (int i = 0; i < s.preservedCount; ++i)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(s.preserved[i].base);
        if (address >= base && address - base < s.preserved[i].bytes) return true;
    }
    return false;
}

#if defined(__linux__)
int FindModuleSegments(dl_phdr_info* info, std::size_t, void*)
{
    State& s = *sState;
    if (info->dlpi_addr != s.moduleBase) return 0;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD) continue;
        s.moduleEnd = std::max<std::uintptr_t>(s.moduleEnd, info->dlpi_addr + header.p_vaddr + header.p_memsz);
        if (!(header.p_flags & PF_W) || s.moduleCount == kMaxModuleRanges) continue;
        s.module[s.moduleCount++] = {reinterpret_cast<std::uint8_t*>(info->dlpi_addr + header.p_vaddr),
                                     header.p_memsz, "module", s.moduleTotal};
        s.moduleTotal += header.p_memsz;
    }
    return 1;
}
#endif

// The save, fixed after the game set up its arenas; false when it cannot run.
bool Prepare(void* task, unsigned long taskBytes)
{
    State& s = *sState;
    s.rangeCount = 0;
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
    std::size_t total = 0;
    for (int i = 0; i < s.rangeCount; ++i)
    {
        s.ranges[i].offset = total;
        total += s.ranges[i].bytes;
    }
    if (total != s.total)
    {
        std::free(s.saved);
        std::free(s.after);
        s.saved = static_cast<std::uint8_t*>(std::malloc(total));
        s.after = static_cast<std::uint8_t*>(std::malloc(total));
        if (s.profile)
        {
            std::free(s.profileCopy);
            s.profileCopy = static_cast<std::uint8_t*>(std::malloc(total));
            if (!s.profileCopy) s.profile = false;
        }
        s.total = total;
        if (!s.saved || !s.after)
        {
            std::fprintf(s.out, "sync test: cannot reserve %zu bytes twice; stopped\n", total);
            return false;
        }
    }

    s.preservedCount = 0;
    if (g_pAudioBackend)
    {
        AddPreserved(g_pAudioBackend->m_AudioAllocator.m_memory, g_pAudioBackend->m_AudioAllocator.m_memory_size,
                     "audio memory");
        AddPreserved(g_pAudioBackend, sizeof(AudioBackend), "AudioBackend");
    }
    std::size_t preservedBytes = 0;
    for (int i = 0; i < s.preservedCount; ++i)
    {
        s.preserved[i].offset = preservedBytes;
        preservedBytes += s.preserved[i].bytes;
    }
    if (preservedBytes > s.preservedCapacity)
    {
        std::free(s.preservedCopy);
        s.preservedCopy = static_cast<std::uint8_t*>(std::malloc(preservedBytes));
        s.preservedCapacity = s.preservedCopy ? preservedBytes : 0;
        if (!s.preservedCopy) return false;
    }

    if (!s.moduleName)
    {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&mscharged_synctest_between_steps), &info) && info.dli_fname)
        {
            const char* name = std::strrchr(info.dli_fname, '/');
            s.moduleName = name ? name + 1 : info.dli_fname;
            s.moduleBase = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
#if defined(__linux__)
            dl_iterate_phdr(FindModuleSegments, nullptr);
#endif
            s.moduleAfter = s.moduleTotal ? static_cast<std::uint8_t*>(std::malloc(s.moduleTotal)) : nullptr;
            if (!s.moduleAfter) s.moduleCount = 0;
        }
        else
            s.moduleName = "?";
    }
    if (!s.audioSaved)
    {
        s.audioCount = 0;
        s.audioTotal = 0;
        if (__rbk_audio_data_start && +__rbk_audio_data_end > +__rbk_audio_data_start)
            s.audio[s.audioCount++] = {reinterpret_cast<std::uint8_t*>(__rbk_audio_data_start),
                                       static_cast<std::size_t>(__rbk_audio_data_end - __rbk_audio_data_start),
                                       "audio data", 0};
        if (__rbk_audio_bss_start && +__rbk_audio_bss_end > +__rbk_audio_bss_start)
            s.audio[s.audioCount++] = {reinterpret_cast<std::uint8_t*>(__rbk_audio_bss_start),
                                       static_cast<std::size_t>(__rbk_audio_bss_end - __rbk_audio_bss_start),
                                       "audio bss", 0};
        for (int i = 0; i < s.audioCount; ++i)
        {
            s.audio[i].offset = s.audioTotal;
            s.audioTotal += s.audio[i].bytes;
        }
        s.audioSaved = static_cast<std::uint8_t*>(std::malloc(s.audioTotal ? s.audioTotal : 1));
        if (!s.audioSaved) return false;
    }
    return true;
}

bool SkipNothing(std::uintptr_t) { return false; }

void Copy(std::uint8_t* buffer, const Range* ranges, int count)
{
    for (int i = 0; i < count; ++i) std::memcpy(buffer + ranges[i].offset, ranges[i].base, ranges[i].bytes);
}
void Paste(const std::uint8_t* buffer, const Range* ranges, int count)
{
    for (int i = 0; i < count; ++i) std::memcpy(ranges[i].base, buffer + ranges[i].offset, ranges[i].bytes);
}

void AddDiff(std::uintptr_t address, const char* region)
{
    State& s = *sState;
    ++s.diffTotal;
    if (s.diffCount)
    {
        Diff& last = s.diffs[s.diffCount - 1];
        if (last.region == region && address <= last.start + last.bytes + kMergeGap)
        {
            last.bytes = address + 1 - last.start;
            return;
        }
    }
    if (s.diffCount < kMaxDiffs) s.diffs[s.diffCount++] = {address, 1, region};
}

// Bytes of `ranges` that differ from `copy`, outside the preserved regions.
void CollectDiffs(const std::uint8_t* copy, const Range* ranges, int count, bool (*skip)(std::uintptr_t))
{
    for (int r = 0; r < count; ++r)
    {
        const Range& range = ranges[r];
        const std::uint8_t* before = copy + range.offset;
        for (std::size_t chunk = 0; chunk < range.bytes; chunk += kChunk)
        {
            const std::size_t length = std::min(kChunk, range.bytes - chunk);
            if (!std::memcmp(range.base + chunk, before + chunk, length)) continue;
            for (std::size_t i = chunk; i < chunk + length; ++i)
            {
                if (range.base[i] == before[i]) continue;
                const auto address = reinterpret_cast<std::uintptr_t>(range.base + i);
                if (skip(address)) continue;
                AddDiff(address, range.name);
            }
        }
    }
}

const std::uint8_t* Before(std::uintptr_t address, const std::uint8_t* copy, const Range* ranges, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(ranges[i].base);
        if (address >= base && address - base < ranges[i].bytes) return copy + ranges[i].offset + (address - base);
    }
    return nullptr;
}

void PrintBytes(const char* label, const std::uint8_t* bytes, std::size_t count)
{
    std::fprintf(sState->out, "      %s", label);
    for (std::size_t i = 0; i < count; ++i) std::fprintf(sState->out, " %02x", bytes[i]);
    std::fprintf(sState->out, "\n");
}

void PrintCompare(std::uintptr_t address, std::size_t bytes, const std::uint8_t* copy, const Range* ranges,
                  int count)
{
    const std::uint8_t* before = Before(address, copy, ranges, count);
    if (!before) return;
    const std::size_t shown = std::min<std::size_t>(bytes, 24);
    PrintBytes("first run: ", before, shown);
    PrintBytes("second run:", reinterpret_cast<const std::uint8_t*>(address), shown);
}

// The module offset of a pointer into this module (a vtable names the class).
bool ModuleOffset(std::uintptr_t value, std::uintptr_t& offset)
{
    if (sState->moduleEnd) // dladdr reads symbol tables: too slow for every block of an audit
    {
        if (value < sState->moduleBase || value >= sState->moduleEnd) return false;
        offset = value - sState->moduleBase;
        return true;
    }
    Dl_info info{};
    if (!value || !dladdr(reinterpret_cast<void*>(value), &info)) return false;
    if (reinterpret_cast<std::uintptr_t>(info.dli_fbase) != sState->moduleBase) return false;
    offset = value - sState->moduleBase;
    return true;
}

// ", allocated at A < B < C": the call site's frames inside the game module,
// innermost first, as module offsets (one byte back: inside the call).
void PrintCallsite(FILE* file, void* const* frames, int depth, const char* label = "allocated at")
{
    if (!depth) return;
    std::fprintf(file, ", %s", label);
    bool first = true;
    for (int i = 0; i < depth; ++i)
    {
        std::uintptr_t offset = 0;
        if (!ModuleOffset(reinterpret_cast<std::uintptr_t>(frames[i]), offset)) continue;
        std::fprintf(file, "%s %s+0x%lx", first ? "" : " <", sState->moduleName, static_cast<unsigned long>(offset - 1));
        first = false;
    }
}

InsidePlace* InsideSlot(int kind, std::uintptr_t key, std::size_t size, bool add)
{
    State& s = *sState;
    if (!s.inside) return nullptr;
    const int stored = kind ? kind : kAggregateKinds;
    const std::uint64_t hash = (key * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full) ^ stored;
    for (std::size_t i = (hash >> 48) & (kInsideSlots - 1), n = 0; n < kInsideSlots; i = (i + 1) & (kInsideSlots - 1), ++n)
    {
        InsidePlace& place = s.inside[i];
        if (place.kind == stored && place.key == key && place.size == size) return &place;
        if (place.kind) continue;
        if (!add || s.insideCount == kInsideSlots / 2) return nullptr; // half full at most: probes stay short
        ++s.insideCount;
        place = {stored, key, size, 0, 0};
        return &place;
    }
    return nullptr;
}
const InsidePlace* FindInside(int kind, std::uintptr_t key, std::size_t size)
{
    return InsideSlot(kind, key, size, false);
}

// Counts one difference of test `test` in the session's aggregates.
void Note(int kind, std::uintptr_t key, std::size_t size, std::size_t bytes, std::uintptr_t offset, int test,
          const Callsite* site = nullptr)
{
    State& s = *sState;
    if (s.noteWindow == kInsideSteps)
    {
        if (InsidePlace* place = InsideSlot(kind, key, size, true); place && place->lastStep != test)
        {
            ++place->steps;
            place->lastStep = test;
        }
        return;
    }
    Aggregate* entry = nullptr;
    const std::uint64_t hash = (key * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full)
        ^ (static_cast<std::uint64_t>(kind) << 8) ^ s.noteWindow;
    std::size_t slot = (hash >> 40) & (kAggregateSlots - 1);
    for (; s.aggregateIndex[slot]; slot = (slot + 1) & (kAggregateSlots - 1))
    {
        Aggregate& candidate = s.aggregates[s.aggregateIndex[slot] - 1];
        if (candidate.kind == kind && candidate.key == key && candidate.size == size
            && candidate.window == s.noteWindow)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        if (s.aggregateCount == kMaxAggregates) return;
        s.aggregateIndex[slot] = static_cast<std::uint32_t>(s.aggregateCount + 1);
        entry = &s.aggregates[s.aggregateCount++];
        *entry = {kind, key, size, 0, test, 0, 0, offset, 0, {}, s.noteWindow};
        if (site)
        {
            entry->depth = site->depth;
            std::memcpy(entry->frames, site->frames, site->depth * sizeof(void*));
        }
    }
    if (entry->lastTest != test)
    {
        ++entry->tests;
        entry->lastTest = test;
    }
    entry->bytes += bytes;
}

// Globals are aggregated per aligned word, so a value whose differing bytes
// vary from test to test stays one entry.
std::uintptr_t GlobalKey(std::uintptr_t address)
{
    return (address - sState->moduleBase) & ~std::uintptr_t{7};
}

// Classifies the arena and restored-global differences of a test into the
// aggregates; prints them as well for a detailed test.
void ClassifyArenaDiffs(bool print, int test)
{
    State& s = *sState;
    s.ownerCount = 0;
    std::size_t unowned = 0;
    for (std::size_t i = 0; i < s.diffCount; ++i)
    {
        const Diff& diff = s.diffs[i];
        mscharged::platform::GameAllocationSpan span{};
        if (!mscharged::platform::FindGameAllocationSpan(reinterpret_cast<void*>(diff.start), 1, span))
        {
            std::uintptr_t base = diff.start;
            bool global = false;
            int index = -1;
            for (int r = 0; r < s.rangeCount; ++r)
            {
                const auto start = reinterpret_cast<std::uintptr_t>(s.ranges[r].base);
                if (diff.start >= start && diff.start - start < s.ranges[r].bytes)
                {
                    base = start;
                    global = InGameGlobals(s.ranges[r]);
                    index = r;
                }
            }
            if (global)
                Note(kRestoredGlobal, GlobalKey(diff.start), 0, diff.bytes, 0, test);
            else
                Note(kUnowned, static_cast<std::uintptr_t>(index), 0, diff.bytes, 0, test);
            if (print && unowned++ < kListedUnowned)
            {
                if (global) // a restored game global: name it as a module offset
                    std::fprintf(s.out, "  restored global %s+0x%lx, %zu bytes\n", s.moduleName,
                                 static_cast<unsigned long>(diff.start - s.moduleBase), diff.bytes);
                else
                    std::fprintf(s.out, "  no live allocation: %s+0x%lx, %zu bytes\n", diff.region,
                                 static_cast<unsigned long>(diff.start - base), diff.bytes);
                PrintCompare(diff.start, diff.bytes, s.after, s.ranges, s.rangeCount);
            }
            continue;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(span.base);
        Owner* owner = nullptr;
        if (s.ownerCount && s.owners[s.ownerCount - 1].base == base) owner = &s.owners[s.ownerCount - 1];
        for (std::size_t o = 0; o < s.ownerCount && !owner; ++o)
            if (s.owners[o].base == base) owner = &s.owners[o];
        if (!owner && s.ownerCount < kMaxOwners)
        {
            owner = &s.owners[s.ownerCount++];
            *owner = {base, span.bytes, 0, 0, diff.start, diff.region};
        }
        if (!owner) continue;
        owner->diffBytes += diff.bytes;
        ++owner->diffs;
    }
    if (print && unowned > kListedUnowned)
        std::fprintf(s.out, "  ... %zu more ranges without a live allocation\n", unowned - kListedUnowned);

    std::sort(s.owners, s.owners + s.ownerCount,
              [](const Owner& a, const Owner& b) { return a.diffBytes > b.diffBytes; });
    for (std::size_t o = 0; o < s.ownerCount; ++o)
    {
        const Owner& owner = s.owners[o];
        std::uintptr_t vtable = 0, offset = 0;
        if (owner.bytes >= sizeof(vtable))
        {
            // An audit keeps no copy: the block as it is now.
            const std::uint8_t* before = s.audit ? reinterpret_cast<const std::uint8_t*>(owner.base)
                                                 : Before(owner.base, s.after, s.ranges, s.rangeCount);
            if (before) std::memcpy(&vtable, before, sizeof(vtable));
        }
        const bool named = ModuleOffset(vtable, offset);
        Callsite site{};
        const bool sited = FindCallsite(owner.base, site);
        Note(kAllocation, named ? offset : 0, owner.bytes, owner.diffBytes, owner.first - owner.base, test,
             sited ? &site : nullptr);
        if (!print || o >= kListedOwners) continue;
        std::fprintf(s.out, "  allocation %s 0x%lx, %zu bytes: %zu bytes differ in %zu ranges, first at +0x%lx",
                     owner.region, static_cast<unsigned long>(owner.base), owner.bytes, owner.diffBytes,
                     owner.diffs, static_cast<unsigned long>(owner.first - owner.base));
        if (named) std::fprintf(s.out, ", first word %s+0x%lx", s.moduleName, static_cast<unsigned long>(offset));
        if (sited) PrintCallsite(s.out, site.frames, site.depth);
        std::fprintf(s.out, "\n");
        PrintCompare(owner.first, 24, s.after, s.ranges, s.rangeCount);
    }
    if (print && s.ownerCount > kListedOwners)
        std::fprintf(s.out, "  ... %zu more allocations\n", s.ownerCount - kListedOwners);
}

void ClassifyModuleDiffs(bool print, int test)
{
    State& s = *sState;
    for (std::size_t i = 0; i < s.diffCount; ++i)
    {
        const Diff& diff = s.diffs[i];
        Note(kModuleGlobal, GlobalKey(diff.start), 0, diff.bytes, 0, test);
        if (!print || i >= kListedModule) continue;
        std::fprintf(s.out, "  module global %s+0x%lx, %zu bytes\n", s.moduleName,
                     static_cast<unsigned long>(diff.start - s.moduleBase), diff.bytes);
        PrintCompare(diff.start, diff.bytes, s.moduleAfter, s.module, s.moduleCount);
    }
    if (print && s.diffCount > kListedModule)
        std::fprintf(s.out, "  ... %zu more ranges\n", s.diffCount - kListedModule);
}

// The match a test runs in: captains, sidekicks and stadium from the game's
// match descriptor. A new match starts when they change or the frame count
// goes back (the fixed update task counts from each kickoff).
Match* CurrentMatch(unsigned frame)
{
    State& s = *sState;
    Match now{{-1, -1}, {{-1, -1, -1}, {-1, -1, -1}}, -1, 0, 0, 0};
    if (const GameInfoManager* info = nlSingleton<GameInfoManager>::Instance())
        if (info->mCurrentMode >= 0 && info->mCurrentMode < GameInfoManager::GM_NUM_MODES)
            if (const BasicGameInfo* game = info->mGameInfo[info->mCurrentMode])
            {
                for (short side = 0; side < 2; ++side)
                {
                    now.teams[side] = game->GetTeam(side);
                    for (int slot = 0; slot < 3; ++slot) now.sidekicks[side][slot] = game->GetSidekick(side, slot);
                }
                now.stadium = game->mStadiumIndex;
            }
    Match* last = s.matchCount ? &s.matches[s.matchCount - 1] : nullptr;
    const bool same = last && frame >= s.lastFrame && !std::memcmp(last->teams, now.teams, sizeof(now.teams))
        && !std::memcmp(last->sidekicks, now.sidekicks, sizeof(now.sidekicks)) && last->stadium == now.stadium;
    s.lastFrame = frame;
    if (same) return last;
    if (s.matchCount == kMaxMatches) return last;
    Match& match = s.matches[s.matchCount++];
    match = now;
    match.firstTest = s.tests + 1;
    std::fprintf(s.out, "##### match %d: %s (%s, %s, %s) vs %s (%s, %s, %s) at %s\n", s.matchCount,
                 NameOf(kCaptainNames, match.teams[0]), NameOf(kSidekickNames, match.sidekicks[0][0]),
                 NameOf(kSidekickNames, match.sidekicks[0][1]), NameOf(kSidekickNames, match.sidekicks[0][2]),
                 NameOf(kCaptainNames, match.teams[1]), NameOf(kSidekickNames, match.sidekicks[1][0]),
                 NameOf(kSidekickNames, match.sidekicks[1][1]), NameOf(kSidekickNames, match.sidekicks[1][2]),
                 NameOf(kStadiumNames, match.stadium));
    return &match;
}

// The place an aggregate names, without a line break.
void PrintAggregatePlace(FILE* file, const Aggregate& entry)
{
    const State& s = *sState;
    switch (entry.kind)
    {
    case kRestoredGlobal:
    case kModuleGlobal:
    case kAudioTouched:
        std::fprintf(file, "%s+0x%lx", s.moduleName, static_cast<unsigned long>(entry.key));
        break;
    case kAllocation:
        if (entry.key)
            std::fprintf(file, "%zu-byte allocation, first changed +0x%lx, first word %s+0x%lx", entry.size,
                         static_cast<unsigned long>(entry.offset), s.moduleName, static_cast<unsigned long>(entry.key));
        else
            std::fprintf(file, "%zu-byte allocation, first changed +0x%lx, no vtable", entry.size,
                         static_cast<unsigned long>(entry.offset));
        PrintCallsite(file, entry.frames, entry.depth);
        break;
    default:
        std::fprintf(file, "%s", entry.key < static_cast<std::uintptr_t>(s.rangeCount) ? s.ranges[entry.key].name : "?");
        break;
    }
}

const char* RegionOf(std::uintptr_t address);

void PrintAllocator(FILE* file, const MemoryAllocator* owner)
{
    if (owner == &StandardAllocator)
        std::fprintf(file, "MEM1 heap");
    else if (owner == &VirtualAllocator)
        std::fprintf(file, "MEM2 heap");
    else if (g_pAudioBackend && owner == &g_pAudioBackend->m_AudioAllocator)
        std::fprintf(file, "audio heap (kept out of the rollback)");
    else if (owner)
        std::fprintf(file, "pool at %p in %s", static_cast<void*>(owner->m_memory),
                     RegionOf(reinterpret_cast<std::uintptr_t>(owner->m_memory)));
    else
        std::fprintf(file, "no allocator");
}

// Audit mode: the allocations and frees of each window; the sites outside
// the steps and on other threads in full.
void WriteAuditAllocations(FILE* file)
{
    const State& s = *sState;
    std::lock_guard lock(sAllocSiteMutex);
    const char* const windows[kAuditWindows] = {"", "outside the steps", "inside the steps", "on other threads"};
    for (int w = kOutsideSteps; w < kAuditWindows; ++w)
        std::fprintf(file, "%s: %llu allocations, %llu frees\n", windows[w],
                     static_cast<unsigned long long>(s.allocCounts[w][1]),
                     static_cast<unsigned long long>(s.allocCounts[w][0]));
    if (s.allocDropped)
        std::fprintf(file, "allocations and frees at sites beyond the %zu listed: %llu\n", kMaxAllocSites,
                     static_cast<unsigned long long>(s.allocDropped));
    for (int w : {kOutsideSteps, kOtherThreads})
    {
        std::fprintf(file, "\n== allocations and frees %s\n", windows[w]);
        const AllocSite* listed[kMaxAllocSites];
        std::size_t count = 0;
        for (std::size_t i = 0; i < s.allocSiteCount; ++i)
            if (s.allocSites[i].window == w) listed[count++] = &s.allocSites[i];
        std::sort(listed, listed + count,
                  [](const AllocSite* a, const AllocSite* b) { return a->count > b->count; });
        if (!count) std::fprintf(file, "none\n");
        for (std::size_t i = 0; i < count; ++i)
        {
            const AllocSite& site = *listed[i];
            std::fprintf(file, "%7llu %s in %d steps, %llu bytes, ", static_cast<unsigned long long>(site.count),
                         site.allocate ? "allocations" : "frees", site.steps,
                         static_cast<unsigned long long>(site.bytes));
            PrintAllocator(file, site.owner);
            PrintCallsite(file, site.frames, site.depth, site.allocate ? "allocated at" : "freed at");
            std::fprintf(file, "\n");
        }
    }
}

// Audit mode: what each window wrote; outside the steps first, each place
// with how often the steps write it too.
void WriteAuditSummary(FILE* file)
{
    const State& s = *sState;
    const auto& h = mscharged::platform::RollbackSnapshotStatistics();
    std::fprintf(file, "sync test audit: what the frame writes outside the simulation steps, module %s\n",
                 s.moduleName ? s.moduleName : "?");
    std::fprintf(file, "%d steps audited, tracking started %d times; %zu-byte pages, %zu tracked\n", s.auditSteps,
                 s.auditStarts, h.pageSize, h.trackedPages);
    const char* const windows[3] = {"", "outside the steps", "inside the steps"};
    for (int w = kOutsideSteps; w <= kInsideSteps; ++w)
        if (s.auditSteps)
            std::fprintf(file, "changed %s: %llu bytes avg / %llu max per step\n", windows[w],
                         static_cast<unsigned long long>(s.auditBytes[w] / s.auditSteps),
                         static_cast<unsigned long long>(s.auditMaxBytes[w]));
    if (h.saves)
        std::fprintf(file, "noting a window: %.1f us avg / %.1f us max; save %.1f us avg / %.1f us max; faults %llu\n",
                     s.auditNs / 1e3 / std::max(1, 2 * s.auditSteps), s.auditMaxNs / 1e3, h.saveNs / 1e3 / h.saves, h.maxSaveNs / 1e3,
                     static_cast<unsigned long long>(h.faults));
    std::fprintf(file, "changed ranges not named (over %zu in a window): %llu\n", kMaxDiffs,
                 static_cast<unsigned long long>(s.auditDropped));
    std::fprintf(file, "pages written behind the tracking (missed): %llu\n",
                 static_cast<unsigned long long>(s.untracked));
    WriteAuditAllocations(file);

    std::fprintf(file, "\n== matches\n");
    for (int m = 0; m < s.matchCount; ++m)
    {
        const Match& match = s.matches[m];
        std::fprintf(file, "match %d: %s vs %s at %s\n", m + 1, NameOf(kCaptainNames, match.teams[0]),
                     NameOf(kCaptainNames, match.teams[1]), NameOf(kStadiumNames, match.stadium));
    }

    std::fprintf(file, "places the steps write: %zu%s\n", s.insideCount,
                 s.insideCount == kInsideSlots / 2 ? " (table full: more not counted)" : "");

    const char* const kinds[kAggregateKinds] = {"game globals (rbk sections), by 8-byte word", "",
                                                "allocations (by first word, the vtable of a class, and size)",
                                                "arena memory without a live allocation, and the allocator's globals",
                                                ""};
    for (int kind : {kRestoredGlobal, kAllocation, kUnowned})
    {
        std::fprintf(file, "\n== %s, written outside the steps\n", kinds[kind]);
        const Aggregate* listed[kMaxAggregates];
        std::size_t count = 0;
        for (std::size_t i = 0; i < s.aggregateCount; ++i)
            if (s.aggregates[i].kind == kind && s.aggregates[i].window == kOutsideSteps)
                listed[count++] = &s.aggregates[i];
        std::sort(listed, listed + count, [](const Aggregate* a, const Aggregate* b) {
            return a->tests != b->tests ? a->tests > b->tests : a->bytes > b->bytes;
        });
        if (!count) std::fprintf(file, "none\n");
        for (std::size_t i = 0; i < count; ++i)
        {
            const Aggregate& entry = *listed[i];
            std::fprintf(file, "%6d steps (%d..%d), %zu bytes", entry.tests, entry.firstTest, entry.lastTest,
                         entry.bytes);
            if (const InsidePlace* inside = FindInside(entry.kind, entry.key, entry.size))
                std::fprintf(file, ", ALSO INSIDE in %d steps: ", inside->steps);
            else
                std::fprintf(file, ", outside only: ");
            PrintAggregatePlace(file, entry);
            std::fprintf(file, "\n");
        }
    }
}

// The session so far, rewritten after every test so that it survives a crash.
void WriteSummary()
{
    State& s = *sState;
    FILE* file = std::fopen(s.summaryPath, "w");
    if (!file) return;
    if (s.audit)
    {
        WriteAuditSummary(file);
        std::fclose(file);
        return;
    }
    std::fprintf(file, "sync test summary: every %d steps, %d steps rolled back, module %s\n", s.interval, s.frames,
                 s.moduleName ? s.moduleName : "?");
    std::fprintf(file, "%d tests, %d with differences, %d with different checksums, %d with frees not undone, "
                       "%d not rolled back for disc reads, %d not rolled back for a game or task state change, "
                       "%d not rolled back for audio state, %d steps waited for reads in flight\n",
                 s.tests, s.failures, s.checksumFailures, s.lossyTests, s.skipped, s.stateSkipped, s.audioSkipped,
                 s.postponed);
    if (s.checksumFailures)
    {
        std::fprintf(file, "tests with different checksums:");
        for (int i = 0; i < std::min(s.checksumFailures, kMaxListedTests); ++i)
            std::fprintf(file, " %d", s.checksumTests[i]);
        std::fprintf(file, "\n");
    }

    const auto& p = s.prof;
    std::fprintf(file, "\n== snapshot cost\n");
    std::fprintf(file, "saved memory: %.1f MB in %d ranges\n", s.total / 1048576.0, s.rangeCount);
    if (p.saves)
        std::fprintf(file, "full save: %.2f ms avg / %.2f ms max over %llu saves\n", p.saveNs / 1e6 / p.saves,
                     p.maxSaveNs / 1e6, static_cast<unsigned long long>(p.saves));
    if (p.restores)
        std::fprintf(file, "full restore: %.2f ms avg / %.2f ms max over %llu restores\n",
                     p.restoreNs / 1e6 / p.restores, p.maxRestoreNs / 1e6,
                     static_cast<unsigned long long>(p.restores));
    if (s.history)
    {
        const auto& h = mscharged::platform::RollbackSnapshotStatistics();
        std::fprintf(file, "\n== incremental snapshots (%zu-byte pages)\n", h.pageSize);
        std::fprintf(file, "tracked: %zu pages, %zu hot after the last save\n", h.trackedPages, h.hotPages);
        if (s.trackedSteps)
            std::fprintf(file, "step with tracking (faults included): %.2f ms avg / %.2f ms max over %llu steps\n",
                         s.trackedStepNs / 1e6 / s.trackedSteps, s.maxTrackedStepNs / 1e6,
                         static_cast<unsigned long long>(s.trackedSteps));
        if (h.saves)
            std::fprintf(file, "save: %.1f us avg / %.1f us max over %llu saves; %llu pages avg / %llu max\n",
                         h.saveNs / 1e3 / h.saves, h.maxSaveNs / 1e3, static_cast<unsigned long long>(h.saves),
                         static_cast<unsigned long long>(h.savedPages / h.saves),
                         static_cast<unsigned long long>(h.maxSavedPages));
        if (h.restores)
            std::fprintf(file, "restore: %.1f us avg / %.1f us max over %llu restores; %llu pages avg / %llu max\n",
                         h.restoreNs / 1e3 / h.restores, h.maxRestoreNs / 1e3,
                         static_cast<unsigned long long>(h.restores),
                         static_cast<unsigned long long>(h.restoredPages / h.restores),
                         static_cast<unsigned long long>(h.maxRestoredPages));
        std::fprintf(file, "faults %llu; cold pages turned hot %llu, hot turned cold %llu; saves dropped early %llu\n",
                     static_cast<unsigned long long>(h.faults), static_cast<unsigned long long>(h.promoted),
                     static_cast<unsigned long long>(h.demoted), static_cast<unsigned long long>(h.evicted));
        std::fprintf(file, "rollbacks by steps undone:");
        for (int d = 1; d <= kMaxFrames; ++d)
            if (s.rollbackDepths[d])
                std::fprintf(file, " %d: %llu", d, static_cast<unsigned long long>(s.rollbackDepths[d]));
        std::fprintf(file, "\nrollbacks to a save no longer kept: %llu\n",
                     static_cast<unsigned long long>(s.lostSnapshots));
        std::fprintf(file, "pages written behind the tracking (missed): %llu",
                     static_cast<unsigned long long>(s.untracked));
        std::uintptr_t offset = 0;
        if (s.untracked && ModuleOffset(s.untrackedAt, offset))
            std::fprintf(file, ", first at %s+0x%lx", s.moduleName, static_cast<unsigned long>(offset));
        else if (s.untracked)
            std::fprintf(file, ", first at 0x%lx", static_cast<unsigned long>(s.untrackedAt));
        std::fprintf(file, "\n");
    }
    if (p.steps)
    {
        std::fprintf(file, "step (one fixed tick, what a resimulation repeats): %.2f ms avg / %.2f ms max over %llu "
                           "steps\n",
                     p.stepNs / 1e6 / p.steps, p.maxStepNs / 1e6, static_cast<unsigned long long>(p.steps));
        std::fprintf(file, "memory a step writes (4 KB chunks): %llu KB avg / %llu KB max; steps over 256 KB: %llu, "
                           "over 1 MB: %llu, over 4 MB: %llu\n",
                     static_cast<unsigned long long>(p.chunks * kChunk / 1024 / p.steps),
                     static_cast<unsigned long long>(p.maxChunks * kChunk / 1024),
                     static_cast<unsigned long long>(p.over[0]), static_cast<unsigned long long>(p.over[1]),
                     static_cast<unsigned long long>(p.over[2]));
        std::fprintf(file, "copying those chunks (an incremental save, without finding them): %.1f us avg / %.1f us "
                           "max\n",
                     p.copyNs / 1e3 / p.steps, p.maxCopyNs / 1e3);
    }
    else
        std::fprintf(file, "steps not profiled (RECHARGED_SYNCTEST_PROFILE=1 measures them)\n");

    std::fprintf(file, "\n== matches\n");
    bool captains[kCaptains] = {}, sidekicks[kSidekicks] = {}, stadiums[kStadiums] = {};
    for (int m = 0; m < s.matchCount; ++m)
    {
        const Match& match = s.matches[m];
        std::fprintf(file, "match %d: %s (%s, %s, %s) vs %s (%s, %s, %s) at %s: %d tests from test %d, %d with "
                           "differences\n",
                     m + 1, NameOf(kCaptainNames, match.teams[0]), NameOf(kSidekickNames, match.sidekicks[0][0]),
                     NameOf(kSidekickNames, match.sidekicks[0][1]), NameOf(kSidekickNames, match.sidekicks[0][2]),
                     NameOf(kCaptainNames, match.teams[1]), NameOf(kSidekickNames, match.sidekicks[1][0]),
                     NameOf(kSidekickNames, match.sidekicks[1][1]), NameOf(kSidekickNames, match.sidekicks[1][2]),
                     NameOf(kStadiumNames, match.stadium), match.tests, match.firstTest, match.failures);
        if (!match.tests) continue;
        for (int side = 0; side < 2; ++side)
        {
            if (match.teams[side] >= 0 && match.teams[side] < kCaptains) captains[match.teams[side]] = true;
            for (int slot = 0; slot < 3; ++slot)
                if (match.sidekicks[side][slot] >= 0 && match.sidekicks[side][slot] < kSidekicks)
                    sidekicks[match.sidekicks[side][slot]] = true;
        }
        if (match.stadium >= 0 && match.stadium < kStadiums) stadiums[match.stadium] = true;
    }
    std::fprintf(file, "not tested yet: captains");
    for (int i = 0; i < kCaptains; ++i)
        if (!captains[i]) std::fprintf(file, " [%s]", kCaptainNames[i]);
    std::fprintf(file, "; sidekicks");
    for (int i = 0; i < kSidekicks; ++i)
        if (!sidekicks[i]) std::fprintf(file, " [%s]", kSidekickNames[i]);
    std::fprintf(file, "; stadiums");
    for (int i = 0; i < 16; ++i) // the match stadiums; the tutorial and low poly fields are no match grounds
        if (!stadiums[i]) std::fprintf(file, " [%s]", kStadiumNames[i]);
    std::fprintf(file, "\n");

    for (int kind = 0; kind < kAggregateKinds; ++kind)
    {
        std::fprintf(file, "\n== %s\n", kAggregateNames[kind]);
        // Most frequent first; a selection sort over pointers keeps the array.
        const Aggregate* listed[kMaxAggregates];
        std::size_t count = 0;
        for (std::size_t i = 0; i < s.aggregateCount; ++i)
            if (s.aggregates[i].kind == kind && s.aggregates[i].window == kTestDiffs) listed[count++] = &s.aggregates[i];
        std::sort(listed, listed + count, [](const Aggregate* a, const Aggregate* b) {
            return a->tests != b->tests ? a->tests > b->tests : a->bytes > b->bytes;
        });
        if (!count) std::fprintf(file, "none\n");
        for (std::size_t i = 0; i < count; ++i)
        {
            const Aggregate& entry = *listed[i];
            std::fprintf(file, "%4d tests (%d..%d), %zu bytes: ", entry.tests, entry.firstTest, entry.lastTest,
                         entry.bytes);
            switch (kind)
            {
            case kRestoredGlobal:
            case kModuleGlobal:
            case kAudioTouched:
                std::fprintf(file, "%s+0x%lx\n", s.moduleName, static_cast<unsigned long>(entry.key));
                break;
            case kAllocation:
                if (entry.key)
                    std::fprintf(file, "%zu-byte allocation, first differing +0x%lx, first word %s+0x%lx",
                                 entry.size, static_cast<unsigned long>(entry.offset), s.moduleName,
                                 static_cast<unsigned long>(entry.key));
                else
                    std::fprintf(file, "%zu-byte allocation, first differing +0x%lx, no vtable", entry.size,
                                 static_cast<unsigned long>(entry.offset));
                PrintCallsite(file, entry.frames, entry.depth);
                std::fprintf(file, "\n");
                break;
            default:
                std::fprintf(file, "%s\n",
                             entry.key < static_cast<std::uintptr_t>(s.rangeCount) ? s.ranges[entry.key].name : "?");
                break;
            }
        }
    }
    std::fclose(file);
}

// Progress written at once, so a test that never ends shows where it stopped.
void Phase(int number, const char* phase)
{
    std::fprintf(sState->out, "--- test %d: %s\n", number, phase);
    std::fflush(sState->out);
}

std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Chunks of the saved memory a step wrote, copied into profileCopy as an
// incremental save would; returns how many, and adds the copy time to `copyNs`.
std::uint64_t CopyDirtyChunks(std::uint64_t& copyNs)
{
    State& s = *sState;
    std::uint64_t dirty = 0;
    for (int r = 0; r < s.rangeCount; ++r)
    {
        const Range& range = s.ranges[r];
        std::uint8_t* copy = s.profileCopy + range.offset;
        for (std::size_t chunk = 0; chunk < range.bytes; chunk += kChunk)
        {
            const std::size_t length = std::min(kChunk, range.bytes - chunk);
            if (!std::memcmp(range.base + chunk, copy + chunk, length)) continue;
            ++dirty;
            const std::uint64_t start = NowNs();
            std::memcpy(copy + chunk, range.base + chunk, length);
            copyNs += NowNs() - start;
        }
    }
    return dirty;
}

// History mode: the incremental save after step `index` (snapshot index + 1),
// checked first for pages written behind the tracking's back.
void SaveSnapshot(int index)
{
    State& s = *sState;
    std::uintptr_t first = 0;
    if (const std::size_t untracked = mscharged::platform::FindUntrackedRollbackWrites(&first))
    {
        if (!s.untracked) s.untrackedAt = first;
        s.untracked += untracked;
    }
    s.snapshotIds[index + 1] = mscharged::platform::SaveRollbackSnapshot();
    s.marks[index + 1] = mscharged::platform::MarkGameAllocationCheckpoint();
}

// Steps from index `from` of the run (checksums and results there too); with
// save, a snapshot after each.
int RunSteps(int (*step)(void), unsigned (*checksum)(void), int from, int count, unsigned* checksums, int* results,
             bool profile = false, bool save = false)
{
    checksums += from;
    results += from;
    State& s = *sState;
    if (profile) std::memcpy(s.profileCopy, s.saved, s.total); // the live memory, just saved
    std::uint64_t testSteps = 0, testStepNs = 0, testMaxStepNs = 0, testChunks = 0, testMaxChunks = 0,
                  testCopyNs = 0;
    int ran = 0;
    while (ran < count)
    {
        std::fprintf(s.out, "    step %d\n", from + ran + 1);
        std::fflush(s.out);
        const std::uint64_t start = NowNs();
        results[ran] = step();
        const std::uint64_t stepNs = NowNs() - start;
        checksums[ran] = checksum();
        if (save)
        {
            ++s.trackedSteps;
            s.trackedStepNs += stepNs;
            s.maxTrackedStepNs = std::max(s.maxTrackedStepNs, stepNs);
            SaveSnapshot(from + ran);
        }
        if (profile)
        {
            std::uint64_t copyNs = 0;
            const std::uint64_t chunks = CopyDirtyChunks(copyNs);
            auto& p = s.prof;
            ++p.steps;
            p.stepNs += stepNs;
            p.maxStepNs = std::max(p.maxStepNs, stepNs);
            p.chunks += chunks;
            p.maxChunks = std::max(p.maxChunks, chunks);
            p.copyNs += copyNs;
            p.maxCopyNs = std::max(p.maxCopyNs, copyNs);
            const std::uint64_t bytes = chunks * kChunk;
            p.over[0] += bytes > (256u << 10);
            p.over[1] += bytes > (1u << 20);
            p.over[2] += bytes > (4u << 20);
            ++testSteps;
            testStepNs += stepNs;
            testMaxStepNs = std::max(testMaxStepNs, stepNs);
            testChunks += chunks;
            testMaxChunks = std::max(testMaxChunks, chunks);
            testCopyNs += copyNs;
        }
        if (results[ran++] < 0) break; // a NIS stops the simulation
    }
    if (profile && testSteps)
        std::fprintf(s.out,
                     "  profile: step %.2f ms avg / %.2f ms max; written %llu KB avg / %llu KB max per step, "
                     "copied in %.1f us avg\n",
                     testStepNs / 1e6 / testSteps, testMaxStepNs / 1e6,
                     static_cast<unsigned long long>(testChunks * kChunk / 1024 / testSteps),
                     static_cast<unsigned long long>(testMaxChunks * kChunk / 1024), testCopyNs / 1e3 / testSteps);
    return ran;
}

// What a rollback must not cross: the game's own state (a goal, the end of a
// half) and the task manager's (NIS, replay, pause). Their changes start
// presentation, loading and audio outside the save.
struct Phases
{
    int game;
    unsigned current;
    unsigned pending;
    bool operator==(const Phases&) const = default;
};
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

// History and audit modes: the saved ranges without the preserved ones are
// the snapshot regions, with depth saves kept and historyPages pages of undo.
bool ConfigureSnapshots(int depth, std::size_t historyPages)
{
    State& s = *sState;
    mscharged::platform::RollbackRegion regions[kMaxRanges + kMaxPreserved];
    int count = 0;
    for (int r = 0; r < s.rangeCount; ++r)
    {
        auto start = reinterpret_cast<std::uintptr_t>(s.ranges[r].base);
        const std::uintptr_t end = start + s.ranges[r].bytes;
        while (start < end)
        {
            // Up to the first preserved range in [start, end), then past it.
            std::uintptr_t cut = end, resume = end;
            for (int p = 0; p < s.preservedCount; ++p)
            {
                const auto base = reinterpret_cast<std::uintptr_t>(s.preserved[p].base);
                const std::uintptr_t top = base + s.preserved[p].bytes;
                if (top <= start || base >= end || std::max(base, start) >= cut) continue;
                cut = std::max(base, start);
                resume = std::min(top, end);
            }
            if (cut > start)
            {
                if (count == static_cast<int>(std::size(regions))) return false;
                regions[count++] = {reinterpret_cast<void*>(start), cut - start};
            }
            start = resume;
        }
    }
    return mscharged::platform::ConfigureRollbackSnapshots(regions, count, depth, historyPages);
}

void RunTest(int (*step)(void), unsigned (*checksum)(void), void* task, unsigned long taskBytes)
{
    State& s = *sState;
    if (!Prepare(task, taskBytes))
    {
        std::fclose(s.out);
        s.out = nullptr;
        return;
    }
    // Depth and room for every step of a test.
    if (s.history && !ConfigureSnapshots(s.frames, static_cast<std::size_t>(s.frames) * 512 + 2048))
    {
        std::fprintf(s.out, "sync test: incremental snapshots unavailable; full copies instead\n");
        s.history = false;
    }
    // A rollback cannot cross a disc read: the host completes it whatever the
    // game's memory says. Wait for a step with none in flight.
    const auto producers = mscharged::platform::CurrentGameByteProducers();
    if (producers.pending)
    {
        ++s.postponed;
        s.steps = s.interval - 1;
        return;
    }
    const unsigned frame = static_cast<unsigned>(static_cast<FixedUpdateTask*>(task)->GetFrame());
    Match* match = CurrentMatch(frame);
    const int number = ++s.tests;
    if (match) ++match->tests;
    std::fprintf(s.out, "--- test %d: frame %u\n", number, frame);
    Phase(number, "save");

    // Save (interrupts are already off for the whole test; these sections nest).
    BOOL level = OSDisableInterrupts();
    if (s.history)
    {
        mscharged::platform::BeginGameAllocationCheckpoint();
        s.snapshotIds[0] = mscharged::platform::BeginRollbackSnapshots();
        s.marks[0] = mscharged::platform::MarkGameAllocationCheckpoint();
    }
    else
    {
        const std::uint64_t saveStart = NowNs();
        Copy(s.saved, s.ranges, s.rangeCount);
        const std::uint64_t saveNs = NowNs() - saveStart;
        ++s.prof.saves;
        s.prof.saveNs += saveNs;
        s.prof.maxSaveNs = std::max(s.prof.maxSaveNs, saveNs);
        mscharged::platform::BeginGameAllocationCheckpoint();
    }
    Copy(s.audioSaved, s.audio, s.audioCount);
    OSRestoreInterrupts(level);

    const Phases phases = CurrentPhases();
    Phase(number, "first run");
    const int firstSteps = RunSteps(step, checksum, 0, s.frames, s.first, s.firstResult, s.profile && !s.history,
                                    s.history);
    // Not rolled back: the snapshots stop with the checkpoint.
    auto keepFirstRun = [&] {
        if (s.history) mscharged::platform::EndRollbackSnapshots();
        mscharged::platform::EndGameAllocationCheckpoint();
    };

    // The first run changed audio globals, which the save leaves alone: the
    // simulation reached audio code through a way that is not silenced yet.
    // Keep its result and name the globals.
    s.diffCount = s.diffTotal = 0;
    CollectDiffs(s.audioSaved, s.audio, s.audioCount, SkipNothing);
    if (s.diffTotal)
    {
        keepFirstRun();
        ++s.audioSkipped;
        std::fprintf(s.out, "=== test %d: %d steps, not rolled back: the first run changed %zu bytes of audio globals:",
                     number, firstSteps, s.diffTotal);
        for (std::size_t i = 0; i < s.diffCount; ++i)
        {
            Note(kAudioTouched, GlobalKey(s.diffs[i].start), 0, s.diffs[i].bytes, 0, number);
            if (i < 8)
                std::fprintf(s.out, " %s+0x%lx", s.moduleName,
                             static_cast<unsigned long>(s.diffs[i].start - s.moduleBase));
        }
        std::fprintf(s.out, "\n");
        std::fflush(s.out);
        WriteSummary();
        return;
    }

    // The first run changed the game's phase: keep its result, do not roll back.
    const Phases changed = CurrentPhases();
    if (!(changed == phases))
    {
        keepFirstRun();
        ++s.stateSkipped;
        std::fprintf(s.out,
                     "=== test %d: %d steps, not rolled back: the first run changed the game state %d -> %d, task "
                     "state %u/%u -> %u/%u\n",
                     number, firstSteps, phases.game, changed.game, phases.current, phases.pending, changed.current,
                     changed.pending);
        std::fflush(s.out);
        WriteSummary();
        return;
    }

    // The first run started a disc read: keep its result and do not roll back.
    const auto started = mscharged::platform::CurrentGameByteProducers().started - producers.started;
    if (started)
    {
        keepFirstRun();
        ++s.skipped;
        std::fprintf(s.out, "=== test %d: %d steps, not rolled back: the first run started %llu disc reads\n", number,
                     firstSteps, static_cast<unsigned long long>(started));
        std::fflush(s.out);
        WriteSummary();
        return;
    }

    Phase(number, "restore");
    level = OSDisableInterrupts();
    Copy(s.after, s.ranges, s.rangeCount);
    Copy(s.moduleAfter, s.module, s.moduleCount);
    std::size_t lossy = 0;
    int secondSteps = 0;
    bool lost = false;
    if (s.history)
    {
        OSRestoreInterrupts(level);
        // Back to the save before step `to` (the preserved regions are not in
        // the snapshots), from the save after step `at`.
        auto rollBack = [&](int to, int at) {
            const BOOL inner = OSDisableInterrupts();
            if (!mscharged::platform::RestoreRollbackSnapshot(s.snapshotIds[to]))
            {
                lost = true;
                ++s.lostSnapshots;
                std::fprintf(s.out, "  the save before step %d is no longer kept\n", to + 1);
            }
            lossy += mscharged::platform::RollbackGameAllocationCheckpointTo(s.marks[to], Preserved);
            ++s.rollbackDepths[at - to];
            OSRestoreInterrupts(inner);
        };
        // Two rollbacks of different depths: the second one goes back further
        // than the first (through saves of the first run) or, from the start,
        // just one step (into a save of the second run).
        const int to = (number - 1) % firstSteps;
        std::copy(s.first, s.first + to, s.second);
        std::copy(s.firstResult, s.firstResult + to, s.secondResult);
        rollBack(to, firstSteps);
        Phase(number, "second run");
        secondSteps = to + RunSteps(step, checksum, to, firstSteps - to, s.second, s.secondResult, false, true);
        if (secondSteps == firstSteps && !lost)
        {
            const int again = to ? to / 2 : firstSteps - 1;
            rollBack(again, firstSteps);
            Phase(number, "third run");
            secondSteps = again + RunSteps(step, checksum, again, firstSteps - again, s.second, s.secondResult, false,
                                           true);
        }
        mscharged::platform::EndRollbackSnapshots();
        mscharged::platform::EndGameAllocationCheckpoint();
    }
    else
    {
        // Restore, keeping the preserved regions as they are now.
        Copy(s.preservedCopy, s.preserved, s.preservedCount);
        const std::uint64_t restoreStart = NowNs();
        Paste(s.saved, s.ranges, s.rangeCount);
        const std::uint64_t restoreNs = NowNs() - restoreStart;
        ++s.prof.restores;
        s.prof.restoreNs += restoreNs;
        s.prof.maxRestoreNs = std::max(s.prof.maxRestoreNs, restoreNs);
        Paste(s.preservedCopy, s.preserved, s.preservedCount);
        lossy = mscharged::platform::RollbackGameAllocationCheckpoint(Preserved);
        OSRestoreInterrupts(level);

        Phase(number, "second run");
        secondSteps = RunSteps(step, checksum, 0, firstSteps, s.second, s.secondResult);
    }
    Phase(number, "compare");

    int mismatch = -1;
    for (int i = 0; i < std::min(firstSteps, secondSteps) && mismatch < 0; ++i)
        if (s.first[i] != s.second[i] || s.firstResult[i] != s.secondResult[i]) mismatch = i;
    if (mismatch < 0 && firstSteps != secondSteps) mismatch = std::min(firstSteps, secondSteps);

    level = OSDisableInterrupts();
    s.diffCount = s.diffTotal = 0;
    CollectDiffs(s.after, s.ranges, s.rangeCount, Preserved);
    OSRestoreInterrupts(level);
    const std::size_t arenaRanges = s.diffCount, arenaBytes = s.diffTotal;
    const bool failed = mismatch >= 0 || arenaBytes || lossy || lost;
    if (failed)
    {
        ++s.failures;
        if (match) ++match->failures;
    }
    if (mismatch >= 0 && s.checksumFailures++ < kMaxListedTests) s.checksumTests[s.checksumFailures - 1] = number;
    if (lossy) ++s.lossyTests;
    const bool detailed = failed && s.failures <= s.details;

    std::fprintf(s.out, "=== test %d: %d steps, checksums %s", number, firstSteps, mismatch < 0 ? "equal" : "DIFFER");
    if (mismatch >= 0) std::fprintf(s.out, " from step %d", mismatch + 1);
    std::fprintf(s.out, ", arenas: %zu bytes differ in %zu ranges", arenaBytes, arenaRanges);
    if (lossy) std::fprintf(s.out, ", %zu frees not undone", lossy);
    if (detailed)
    {
        std::fprintf(s.out, "\n  checksums:");
        for (int i = 0; i < std::max(firstSteps, secondSteps); ++i)
            std::fprintf(s.out, " %08x/%08x", i < firstSteps ? s.first[i] : 0u, i < secondSteps ? s.second[i] : 0u);
        std::fprintf(s.out, "\n");
    }
    ClassifyArenaDiffs(detailed, number);
    // Module globals are compared, never restored: they show what the save misses.
    level = OSDisableInterrupts();
    s.diffCount = s.diffTotal = 0;
    CollectDiffs(s.moduleAfter, s.module, s.moduleCount, Saved); // restored ones are reported above
    OSRestoreInterrupts(level);
    std::fprintf(s.out, "%smodule globals: %zu bytes differ in %zu ranges\n", detailed ? "  " : ", ", s.diffTotal,
                 s.diffCount);
    ClassifyModuleDiffs(detailed, number);
    std::fflush(s.out);
    WriteSummary();
}

// Audit mode. The hooks run on the main thread, the watchdog on its own; both
// hold sAuditMutex.
std::mutex sAuditMutex;
std::uint64_t sAuditLastHookNs;
constexpr std::uint64_t kAuditIdleNs = 250'000'000; // no step for this long stops the tracking
constexpr int kAuditSummaryEvery = 600;             // steps (ten seconds)
constexpr int kAuditUntrackedEvery = 300;           // steps between checks for untracked writes (reads everything)

const char* RegionOf(std::uintptr_t address)
{
    const State& s = *sState;
    for (int i = 0; i < s.rangeCount; ++i)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(s.ranges[i].base);
        if (address >= base && address - base < s.ranges[i].bytes) return s.ranges[i].name;
    }
    return "?";
}

// A run of changed bytes, into the differences (merged with the last one when close).
void NoteChange(std::uintptr_t address, std::size_t bytes, void*)
{
    State& s = *sState;
    s.diffTotal += bytes;
    const char* region = RegionOf(address);
    if (s.diffCount)
    {
        Diff& last = s.diffs[s.diffCount - 1];
        if (last.region == region && address >= last.start && address <= last.start + last.bytes + kMergeGap)
        {
            last.bytes = std::max(last.bytes, address + bytes - last.start);
            return;
        }
    }
    if (s.diffCount < kMaxDiffs)
        s.diffs[s.diffCount++] = {address, bytes, region};
    else
        ++s.auditDropped;
}

// What changed since the last save, noted under window; then a save.
void AuditWindow(int window)
{
    State& s = *sState;
    const std::uint64_t start = NowNs();
    std::uintptr_t first = 0;
    if (window == kOutsideSteps && s.auditSteps % kAuditUntrackedEvery == 0)
        if (const std::size_t untracked = mscharged::platform::FindUntrackedRollbackWrites(&first))
        {
            if (!s.untracked) s.untrackedAt = first;
            s.untracked += untracked;
        }
    s.diffCount = s.diffTotal = 0;
    mscharged::platform::VisitRollbackChanges(NoteChange, nullptr);
    s.noteWindow = window;
    ClassifyArenaDiffs(false, s.auditSteps);
    s.noteWindow = kTestDiffs;
    s.auditBytes[window] += s.diffTotal;
    s.auditMaxBytes[window] = std::max<std::uint64_t>(s.auditMaxBytes[window], s.diffTotal);
    mscharged::platform::SaveRollbackSnapshot();
    const std::uint64_t ns = NowNs() - start;
    s.auditNs += ns;
    s.auditMaxNs = std::max(s.auditMaxNs, ns);
}

void AuditStart(void* task, unsigned long taskBytes)
{
    State& s = *sState;
    if (!Prepare(task, taskBytes) || !ConfigureSnapshots(1, 4096))
    {
        std::fprintf(s.out, "sync test audit: cannot track the match; stopped\n");
        std::fclose(s.out);
        s.out = nullptr;
        return;
    }
    const unsigned frame = static_cast<unsigned>(static_cast<FixedUpdateTask*>(task)->GetFrame());
    CurrentMatch(frame);
    mscharged::platform::BeginRollbackSnapshots();
    s.auditThread = pthread_self();
    s.auditTracking = true;
    ++s.auditStarts;
    std::fprintf(s.out, "audit: tracking from frame %u (step %d)\n", frame, s.auditSteps + 1);
    std::fflush(s.out);
}

void AuditStop(const char* why)
{
    State& s = *sState;
    mscharged::platform::EndRollbackSnapshots();
    s.auditTracking = false;
    std::fprintf(s.out, "audit: tracking stopped after step %d: %s\n", s.auditSteps, why);
    std::fflush(s.out);
    WriteSummary();
}

// Stops the tracking when the steps stop (a pause, the end of a match), so
// menus and loading never run with the match's memory protected.
void* AuditWatchdog(void*)
{
    for (;;)
    {
        timespec pause{0, 50'000'000};
        nanosleep(&pause, nullptr);
        std::lock_guard lock(sAuditMutex);
        if (!sState->out) return nullptr;
        if (sState->auditTracking && NowNs() - sAuditLastHookNs > kAuditIdleNs) AuditStop("no step for 250 ms");
    }
}

struct Install
{
    Install()
    {
        const char* path = std::getenv("RECHARGED_SYNCTEST");
        if (!path || !*path) return;
        auto* state = static_cast<State*>(std::calloc(1, sizeof(State)));
        if (!state) return;
        state->diffs = static_cast<Diff*>(std::calloc(kMaxDiffs, sizeof(Diff)));
        state->owners = static_cast<Owner*>(std::calloc(kMaxOwners, sizeof(Owner)));
        state->aggregates = static_cast<Aggregate*>(std::calloc(kMaxAggregates, sizeof(Aggregate)));
        state->aggregateIndex = static_cast<std::uint32_t*>(std::calloc(kAggregateSlots, sizeof(std::uint32_t)));
        const std::size_t length = std::strlen(path);
        state->summaryPath = static_cast<char*>(std::malloc(length + sizeof(".summary")));
        if (state->summaryPath)
        {
            std::memcpy(state->summaryPath, path, length);
            std::memcpy(state->summaryPath + length, ".summary", sizeof(".summary"));
        }
        state->out = std::fopen(path, "w");
        if (!state->diffs || !state->owners || !state->aggregates || !state->aggregateIndex || !state->summaryPath
            || !state->out)
        {
            std::fprintf(stderr, "RECHARGED_SYNCTEST: cannot prepare %s\n", path);
            return;
        }
        state->interval = EnvInt("RECHARGED_SYNCTEST_INTERVAL", 250, 1, 1 << 20);
        state->frames = EnvInt("RECHARGED_SYNCTEST_FRAMES", 8, 1, kMaxFrames);
        state->details = EnvInt("RECHARGED_SYNCTEST_DETAILS", 3, 0, 1 << 20);
        state->profile = EnvInt("RECHARGED_SYNCTEST_PROFILE", 0, 0, 1) != 0;
        state->history = EnvInt("RECHARGED_SYNCTEST_HISTORY", 0, 0, 1) != 0;
        state->audit = EnvInt("RECHARGED_SYNCTEST_AUDIT", 0, 0, 1) != 0;
        if (state->audit)
        {
            state->inside = static_cast<InsidePlace*>(std::calloc(kInsideSlots, sizeof(InsidePlace)));
            state->allocSites = static_cast<AllocSite*>(std::calloc(kMaxAllocSites, sizeof(AllocSite)));
            std::fprintf(state->out, "sync test audit: no tests, what the frame writes outside the steps; summary in "
                         "%s\n", state->summaryPath);
        }
        else
            std::fprintf(state->out, "sync test: every %d steps, %d steps rolled back, %d failing tests in full; "
                         "summary in %s\n", state->interval, state->frames, state->details, state->summaryPath);
        std::fflush(state->out);
        sState = state;
        pthread_t watchdog;
        if (state->audit && pthread_create(&watchdog, nullptr, AuditWatchdog, nullptr) == 0)
            pthread_detach(watchdog); // a pthread: std::thread would allocate its state with nlMalloc
        // Call sites of large allocations from now on, the game's boot included.
        sCallsites = static_cast<Callsite*>(std::calloc(kSiteSlots, sizeof(Callsite)));
        if (sCallsites)
        {
            void* warm[1];
            backtrace(warm, 1); // load the unwinder now, not inside an allocation
            mscharged::platform::SetGameAllocationObserver(RecordAllocation);
        }
    }
    ~Install()
    {
        if (!sState) return;
        std::lock_guard lock(sAuditMutex);
        if (!sState->out) return;
        if (sState->auditTracking) AuditStop("exit");
        std::fprintf(sState->out, "sync test: %d tests, %d failed, %d not rolled back for disc reads, %d steps waited for "
                     "reads in flight\n", sState->tests, sState->failures, sState->skipped, sState->postponed);
        WriteSummary();
        std::fclose(sState->out);
        sState->out = nullptr;
    }
} sInstall;
}

extern "C" void mscharged_synctest_between_steps(int (*step)(void), unsigned (*checksum)(void), void* task,
                                                 unsigned long taskBytes)
{
    if (!sState || !sState->out) return;
    if (sState->audit)
    {
        std::lock_guard lock(sAuditMutex);
        sAuditLastHookNs = NowNs();
        if (!sState->auditTracking) AuditStart(task, taskBytes);
        sState->auditStepEnded = true;
        return;
    }
    if (++sState->steps < sState->interval) return;
    sState->steps = 0;
    // Interrupts stay off for the whole test: no SDK callback (audio frames,
    // alarms, disc completions) and no audio device job runs between the save
    // and the restore, so the audio state the simulation does not own stays
    // exactly as saved. The steps' own critical sections nest inside.
    const BOOL level = OSDisableInterrupts();
    RunTest(step, checksum, task, taskBytes);
    OSRestoreInterrupts(level);
}

extern "C" void mscharged_synctest_fixed_step(int running)
{
    sInFixedStep = running != 0;
    if (!sState || !sState->audit) return;
    std::lock_guard lock(sAuditMutex);
    State& s = *sState;
    if (!s.out || !s.auditTracking) return;
    sAuditLastHookNs = NowNs();
    if (!running)
    {
        AuditWindow(kInsideSteps);
        if (s.auditSteps % kAuditSummaryEvery == 0) WriteSummary();
        return;
    }
    // A step whose predecessor ended without the hook between steps (a NIS,
    // a pause): the frames between them are no match frames.
    if (!s.auditStepEnded)
    {
        AuditStop("a step not following the hook between steps");
        return;
    }
    s.auditStepEnded = false;
    ++s.auditSteps;
    AuditWindow(kOutsideSteps);
}

// The rollback test (rollback_test.cpp) keeps the whole game silent, menus
// included, and uses the deterministic AI clocks.
extern "C" bool mscharged_synctest_silences_sounds(void)
{
    return (sState && sState->out && !sState->audit && sInFixedStep) || mscharged_rollback_active();
}

extern "C" bool mscharged_synctest_active(void)
{
    return (sState && sState->out) || mscharged_rollback_active();
}
#endif
