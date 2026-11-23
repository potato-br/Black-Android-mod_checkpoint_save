// The last checkpoint kept on disk. The game copies the player's loadout and the mission's state into itself each
// time a checkpoint is reached (sub_001C8D20); this copies the same bytes into checkpoint.sav, one file the next
// checkpoint, of any mission, replaces. CONTINUE FROM LAST CHECKPOINT then starts that mission from the level
// select, and once the level is up puts the bytes back and loads the level again the way the game's own CONTINUE
// does after a death (the game's own restore, sub_001BEF10, reads them when the load finds a checkpoint set)
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "game/checkpoint.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

// ---- checkpoint_crash.log: when the process dies of a signal, where. The port installs no handler of its own (the
// guest memory is committed and decommitted with mmap and mprotect, never by faults), so any fault here is fatal:
// this writes the fault address, the guest address it stands for, the code that faulted and the stack, then hands
// the signal on so Android still makes its own report
namespace crashlog {
int fd = -1;
uintptr_t guest_base = 0;
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
struct sigaction olds[5];

void Put(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    if (fd >= 0) (void)!write(fd, s, n);
}

void Hex(uint64_t v) {
    char out[19] = "0x";
    int at = 2;
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = unsigned((v >> shift) & 0xF);
        if (digit || started || shift == 0) {
            out[at++] = char(digit < 10 ? '0' + digit : 'a' + digit - 10);
            started = true;
        }
    }
    out[at] = 0;
    Put(out);
}

void Dec(unsigned v) {
    char out[12];
    int at = 11;
    out[at] = 0;
    do {
        out[--at] = char('0' + v % 10);
        v /= 10;
    } while (v);
    Put(out + at);
}

void Where(uintptr_t pc) {
    Dl_info info{};
    if (pc && dladdr(reinterpret_cast<void*>(pc), &info)) {
        Put(" ");
        Put(info.dli_sname ? info.dli_sname : "?");
        Put("+");
        Hex(info.dli_saddr ? pc - reinterpret_cast<uintptr_t>(info.dli_saddr) : 0);
        Put(" (");
        const char* name = info.dli_fname ? info.dli_fname : "?";
        const char* slash = name;
        for (const char* p = name; *p; p++)
            if (*p == '/') slash = p + 1;
        Put(slash);
        Put(")");
    }
}

_Unwind_Reason_Code Frame(struct _Unwind_Context* context, void* arg) {
    int* n = static_cast<int*>(arg);
    uintptr_t pc = _Unwind_GetIP(context);
    if (!pc) return _URC_END_OF_STACK;
    Put("  #");
    Dec(unsigned(*n));
    Put(" ");
    Hex(pc);
    Where(pc);
    Put("\n");
    return ++*n >= 24 ? _URC_END_OF_STACK : _URC_NO_REASON;
}

void Handler(int sig, siginfo_t* info, void* context) {
    Put("\n==== the process got signal ");
    Dec(unsigned(sig));
    Put(" ====\n");
    uintptr_t fault = reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr);
    Put("fault address ");
    Hex(fault);
    if (fault >= guest_base && fault - guest_base < 0x100000000ull) {
        Put(" = guest address ");
        Hex(fault - guest_base);
    }
    Put(", si_code ");
    Dec(unsigned(info ? info->si_code : 0));
    Put("\n");
#if defined(__aarch64__)
    auto* uc = static_cast<ucontext_t*>(context);
    Put("pc ");
    Hex(uc->uc_mcontext.pc);
    Where(uc->uc_mcontext.pc);
    Put("\nlr ");
    Hex(uc->uc_mcontext.regs[30]);
    Where(uc->uc_mcontext.regs[30]);
    Put("\nsp ");
    Hex(uc->uc_mcontext.sp);
    Put("\n");
    for (int i = 0; i < 31; i++) {
        Put(i % 4 == 0 ? "x" : " x");
        Dec(unsigned(i));
        Put("=");
        Hex(uc->uc_mcontext.regs[i]);
        if (i % 4 == 3) Put("\n");
    }
    Put("\n");
#else
    (void)context;
#endif
    Put("stack:\n");
    int n = 0;
    _Unwind_Backtrace(Frame, &n);
    // what the checkpoint code knew
    uint32_t obj = R32(kGame);
    Put("game object ");
    Hex(obj);
    Put(", checkpoint flag ");
    Dec(unsigned(G<uint8_t>(0x2F3BF8)));
    Put(", id ");
    Hex(R32(0x2A7EDC));
    Put(", valid ");
    Dec(R32(0x2EC464));
    if (obj) {
        Put(", start-at ");
        Dec(unsigned(G<uint8_t>(obj + 0x2020D)));
        Put(", play state step ");
        Dec(R32(obj + 0x20F78 + 4));
    }
    uint32_t world = R32(0x2D45D4);
    if (world) {
        Put(", world load step ");
        Dec(R32(world + 0x5AE0));
    }
    Put("\n==== end ====\n");
    // on to the handler that was there: Android's own, or the default that ends the process
    for (size_t i = 0; i < 5; i++)
        if (kSignals[i] == sig) sigaction(sig, &olds[i], nullptr);
    raise(sig);
}

void Install() {
    guest_base = reinterpret_cast<uintptr_t>(HostPtr(0));
    std::string path = (app::ExeDir() / "checkpoint_crash.log").string();
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    struct sigaction action {};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < 5; i++) sigaction(kSignals[i], &action, &olds[i]);
}
}  // namespace crashlog

namespace {

// game object, the pointer at kGame
constexpr uint32_t kLevel = 0x2020C;       // byte: the mission the level select picked
constexpr uint32_t kStartAt = 0x2020D;     // byte: the checkpoint a load starts from (every mission start of the game writes 1, never 0)
constexpr uint32_t kPart = 0x2020E;        // byte: written next to kStartAt by the menus, the world load copies it into the resource name
constexpr uint32_t kDifficulty = 0x2014C;  // dword, 0 easy to 3 black ops
constexpr uint32_t kPlayState = 0x20F78;   // the in game state: kGameState points here while a level plays
constexpr uint32_t kReloadDword = 0x21088; // set by the game's CONTINUE before it loads the level again
constexpr uint32_t kReloadByte = 0x210CB;
constexpr uint32_t kLoadWorld = 0x2D45D4;    // pointer: the world the play state's load works on (sub_001EDC90)
constexpr uint32_t kLoadWorldStep = 0x5AE0;  // in it: the step of the world's load, 1 to 0x37, the number the load waits on
constexpr uint32_t kPlayStep = 4;            // in the play state: which step of its load it is on (sub_0020CE00)
constexpr uint32_t kPendingState = 0x21074;  // the state the game is changing to (set by sub_0020DCA0)
constexpr uint32_t kStateStage = 0x21084;    // 2 while a state change is under way

// world container, the pointer at kWorld
constexpr uint32_t kSnapshot = 0x3F0;       // the checkpoint's copy of the player's loadout
constexpr uint32_t kSnapshotSize = 0x50;    // its own fields, up to the pointer
constexpr uint32_t kSnapshotExtra = 0x50;   // pointer to the loadout's bytes, the game allocates it per level
constexpr uint32_t kRecordCount = 0x934;    // records of 12 bytes the mission keeps, a copy goes in a checkpoint
constexpr uint32_t kRecordSize = 12;
constexpr uint32_t kMaxRecords = 1 << 16;

// the checkpoint's globals: a float (the clock), the set flag, a pointer to the records' copy, the stats
constexpr uint32_t kGlobals = 0x2F3BF4;
constexpr uint32_t kGlobalsSize = 0x1C;
constexpr uint32_t kGlobalsPointer = 8;  // offset of the records' pointer, left as the game set it
constexpr uint32_t kFlag = 0x2F3BF8;     // byte: a checkpoint is set
constexpr uint32_t kCheckpointId = 0x2A7EDC;
constexpr uint32_t kSnapshotValid = 0x2EC464;
constexpr uint32_t kLoadoutTable = 0x2D12D0;  // pointer to a pointer to a table, its byte 1 the loadout's size
constexpr uint32_t kMissionTable = 0x288648;  // 8 bytes: the level byte of each mission
constexpr uint32_t kMissions = 8;
constexpr uint32_t kUnlockTable = 0x2D0D9E;   // profile: a byte per (difficulty * 8 + slot), stride 0x20, 1 when unlocked
constexpr uint32_t kFrontEndUi = 0x2D199C;    // pointer to the front end's object, its flags are read by the menu tick
constexpr uint32_t kUiRequestA = 0x3884;
constexpr uint32_t kUiRequestB = 0x388C;
constexpr uint32_t kUiNextPage = 0x3820;      // string: the Flash page the menu goes to ("FMVPlayer" after the start)
constexpr uint32_t kUiLoadTimer = 0x3948;    // float: the loading screen's timer
constexpr uint32_t kUiLoadRunning = 0x394C;  // byte
constexpr uint32_t kUiPage = 0x3840;          // string: the Flash page it is on
// the start only asks the menu to go to the mission's movie page and the menu waits on its Flash screens, which a
// call from the menu tick never advances: the level is entered the way the game's own code does, with sub_0020DCA0
constexpr bool kEnterDirectly = true;

// the game's own code
constexpr uint32_t kStartLevel = 0x124CF0;  // the level select's start: the game object's mission and difficulty
constexpr uint32_t kSetState = 0x20DCA0;    // edi the game object, [esp+4] the state to go to (kPlayState: a level); ret 4
constexpr uint32_t kRestartMission = 0x124190;  // RESTART MISSION: no arguments, resets the UI pages, then sub_0020DCA0, then start-at 1 / checkpoint cleared
constexpr bool kUseGameRestart = true;          // Finish restarts through the game's own RESTART MISSION instead of calling sub_0020DCA0 bare
constexpr bool kAutoRetry = true;               // a load stuck on one step for 20s is started again (twice at most)
constexpr uint32_t kReload = 0x20DBC0;      // edi the game object: loads the play state again WITHOUT the teardown (a level already down)

// the player, as cheats.cpp has it
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharDying = 0x384;
constexpr uint32_t kPlayerKind = 2;

using Clock = std::chrono::steady_clock;

struct Saved {
    uint32_t level = 0;
    uint32_t difficulty = 0;
    uint32_t id = 0;
    uint32_t mission = 0;  // 1 to 8, 0 when the level byte is in no slot
    std::vector<uint8_t> globals, snapshot, loadout, records;
};

constexpr char kFile[] = "checkpoint.sav";

struct FileHeader {
    char magic[4];
    uint32_t version;
    uint32_t level, difficulty, id, mission;
    uint32_t globals, snapshot, loadout, records;  // sizes
    uint32_t hash;                                 // of the bytes after the header
};
static_assert(sizeof(FileHeader) == 44);
constexpr char kMagic[4] = {'B', 'C', 'K', 'P'};
constexpr uint32_t kVersion = 1;

std::mutex g_mutex;
std::shared_ptr<const Saved> g_saved;
bool g_loaded = false;
std::string g_status;
Clock::time_point g_status_at;

std::atomic<bool> g_request{false};
std::atomic<int> g_auto_retries{0};
enum class Phase { Idle, Waiting };
std::atomic<Phase> g_phase{Phase::Idle};
// game thread only: the bytes to put back once the load asks for the checkpoint
std::shared_ptr<const Saved> g_apply;
Clock::time_point g_apply_at;
// game thread only: no saves before it, the load that just put a checkpoint back makes ones of its own
Clock::time_point g_quiet_until;

uint32_t Hash(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

std::filesystem::path Path() { return app::ExeDir() / kFile; }

// hex of the first bytes, for the logs
std::string Hex(const uint8_t* p, size_t n, size_t max = 64) {
    static const char* d = "0123456789abcdef";
    std::string out;
    size_t m = std::min(n, max);
    for (size_t i = 0; i < m; i++) {
        out += d[p[i] >> 4];
        out += d[p[i] & 15];
        if (i % 4 == 3 && i + 1 < m) out += ' ';
    }
    if (n > m) out += "...";
    return out.empty() ? "(empty)" : out;
}

// what a saved checkpoint holds, byte for byte (the first ones of each part)
void LogSaved(const Saved& s, const char* tag) {
    LOGR("checkpoint: [%s] level %u diff %u id %u mission %u", tag, s.level, s.difficulty, s.id, s.mission);
    LOGR("checkpoint: [%s] globals (%zu): %s", tag, s.globals.size(), Hex(s.globals.data(), s.globals.size()).c_str());
    LOGR("checkpoint: [%s] snapshot (%zu): %s", tag, s.snapshot.size(), Hex(s.snapshot.data(), s.snapshot.size(), 80).c_str());
    LOGR("checkpoint: [%s] loadout (%zu): %s", tag, s.loadout.size(), Hex(s.loadout.data(), s.loadout.size(), 64).c_str());
    LOGR("checkpoint: [%s] records (%zu bytes) hash %08x: %s", tag, s.records.size(),
         Hash(s.records.data(), s.records.size()), Hex(s.records.data(), s.records.size(), 48).c_str());
}

// a change of value is logged at once, a value that stays is logged now and then (a heartbeat)
struct EdgeLog {
    uint64_t last = ~0ull;
    Clock::time_point at{};
    bool Hit(uint64_t v, int every_ms = 2000) {
        auto now = Clock::now();
        if (v != last || now - at > std::chrono::milliseconds(every_ms)) {
            last = v;
            at = now;
            return true;
        }
        return false;
    }
};

void SetStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = text;
    g_status_at = Clock::now();
}

std::vector<uint8_t> Build(const Saved& s) {
    FileHeader h{};
    std::memcpy(h.magic, kMagic, 4);
    h.version = kVersion;
    h.level = s.level;
    h.difficulty = s.difficulty;
    h.id = s.id;
    h.mission = s.mission;
    h.globals = uint32_t(s.globals.size());
    h.snapshot = uint32_t(s.snapshot.size());
    h.loadout = uint32_t(s.loadout.size());
    h.records = uint32_t(s.records.size());
    std::vector<uint8_t> body;
    for (const auto* part : {&s.globals, &s.snapshot, &s.loadout, &s.records})
        body.insert(body.end(), part->begin(), part->end());
    h.hash = Hash(body.data(), body.size());
    std::vector<uint8_t> out(sizeof(h));
    std::memcpy(out.data(), &h, sizeof(h));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::shared_ptr<const Saved> Parse(const std::vector<uint8_t>& bytes) {
    FileHeader h;
    if (bytes.size() < sizeof(h)) return nullptr;
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (std::memcmp(h.magic, kMagic, 4) != 0 || h.version != kVersion) return nullptr;
    if (h.globals != kGlobalsSize || h.snapshot != kSnapshotSize || h.loadout > 0x10000 ||
        h.records > kMaxRecords * kRecordSize)
        return nullptr;
    size_t body = size_t(h.globals) + h.snapshot + h.loadout + h.records;
    if (bytes.size() != sizeof(h) + body || Hash(bytes.data() + sizeof(h), body) != h.hash) return nullptr;
    auto s = std::make_shared<Saved>();
    s->level = h.level;
    s->difficulty = h.difficulty;
    s->id = h.id;
    s->mission = h.mission;
    const uint8_t* at = bytes.data() + sizeof(h);
    for (auto [part, size] : {std::pair{&s->globals, h.globals}, std::pair{&s->snapshot, h.snapshot},
                              std::pair{&s->loadout, h.loadout}, std::pair{&s->records, h.records}}) {
        part->assign(at, at + size);
        at += size;
    }
    return s;
}

// a file written to the side and renamed, so a crash in the middle leaves the last one whole
bool Write(const std::vector<uint8_t>& bytes) {
    std::filesystem::path path = Path();
    std::filesystem::path side = path;
    side += ".tmp";
    {
        std::ofstream out(side, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(side, path, ec);
    return !ec;
}

// under g_mutex: the file, read the first time it is asked for
void Ensure() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(Path(), std::ios::binary);
    if (!in) return;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    g_saved = Parse(bytes);
    if (g_saved) {
        LOGR("checkpoint: file read, mission %u, checkpoint %u", g_saved->mission, g_saved->id);
        LogSaved(*g_saved, "file");
    }
    else LOGE("checkpoint: %s is not one this version reads", kFile);
}

std::shared_ptr<const Saved> Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Ensure();
    return g_saved;
}

std::vector<uint8_t> Copy(uint32_t va, uint32_t size) {
    std::vector<uint8_t> out(size);
    if (size) std::memcpy(out.data(), HostPtr(va), size);
    return out;
}

std::string GuestHex(uint32_t va, uint32_t n, size_t max = 32) {
    if (!va) return "null";
    auto v = Copy(va, n);
    return Hex(v.data(), v.size(), max);
}

void Paste(uint32_t va, const uint8_t* from, size_t size) {
    if (size) std::memcpy(HostPtr(va), from, size);
}

uint32_t LoadoutSize() {
    uint32_t table = R32(kLoadoutTable);
    uint32_t p = table ? R32(table) : 0;
    return p ? G<uint8_t>(p + 1) : 0;
}

// the game's own copy of a checkpoint is in the globals and the world: this is what it just wrote
void Save(uint32_t snapshot, uint32_t id) {
    // a load that puts a checkpoint back may make one of its own: the file is the player's, not the level's start
    if (g_phase.load() != Phase::Idle || g_apply) {
        LOGR("checkpoint: checkpoint %u not kept, a load of the saved one is under way", id);
        return;
    }
    if (Clock::now() < g_quiet_until) {
        LOGR("checkpoint: checkpoint %u not kept, the level is still putting the saved one back", id);
        return;
    }
    uint32_t obj = R32(kGame);
    uint32_t world = R32(kWorld);
    if (!obj || !world || !snapshot) {
        LOGR("checkpoint: checkpoint %u not kept: game %08x world %08x snapshot %08x", id, obj, world, snapshot);
        return;
    }
    auto s = std::make_shared<Saved>();
    s->level = G<uint8_t>(obj + kLevel);
    s->difficulty = R32(obj + kDifficulty);
    s->id = id;
    for (uint32_t i = 0; i < kMissions; i++)
        if (G<uint8_t>(kMissionTable + i) == s->level) s->mission = i + 1;
    s->globals = Copy(kGlobals, kGlobalsSize);
    s->snapshot = Copy(snapshot, kSnapshotSize);
    uint32_t extra = R32(snapshot + kSnapshotExtra);
    uint32_t loadout = LoadoutSize();
    if (extra && loadout) s->loadout = Copy(extra, loadout);
    uint32_t records = R32(kGlobals + kGlobalsPointer);
    uint32_t count = R32(world + kRecordCount);
    if (records && count > 0 && count <= kMaxRecords) s->records = Copy(records, count * kRecordSize);
    if (!Write(Build(*s))) {
        LOGE("checkpoint: could not write %s", Path().string().c_str());
        SetStatus("COULD NOT SAVE THE CHECKPOINT");
        return;
    }
    LOGR("checkpoint: saved, mission %u (level %u), difficulty %u, checkpoint %u, loadout %zu bytes, %zu records",
         s->mission, s->level, s->difficulty, s->id, s->loadout.size(), s->records.size() / kRecordSize);
    LogSaved(*s, "saved");
    LOGR("checkpoint: saved: game start-at %u part %u, world %08x snapshot %08x extra %08x records %08x count %u",
         unsigned(G<uint8_t>(obj + kStartAt)), unsigned(G<uint8_t>(obj + kPart)), world, snapshot, extra, records, count);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = true;
    g_saved = s;
}

// the saved bytes into the guest, on the game thread. The buffers the game allocated for this level are written
// where they are now; false when the level is not the saved one's
bool Apply(const Saved& s) {
    uint32_t world = R32(kWorld);
    if (!world) {
        LOGE("checkpoint: nothing to put the save in, no world");
        return false;
    }
    uint32_t snapshot = world + kSnapshot;
    uint32_t records = R32(kGlobals + kGlobalsPointer);
    uint32_t count = R32(world + kRecordCount);
    if (uint64_t(count) * kRecordSize != s.records.size()) {
        LOGE("checkpoint: the level has %u records, the save %zu", count, s.records.size() / kRecordSize);
        return false;
    }
    LOGR("checkpoint: apply: before: flag %u id %u valid %u | world %08x snapshot %08x records ptr %08x count %u",
         unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId), R32(kSnapshotValid), world, snapshot, records, count);
    LOGR("checkpoint: apply: before: globals %s | snapshot %s | records %s", GuestHex(kGlobals, kGlobalsSize).c_str(),
         GuestHex(snapshot, kSnapshotSize, 48).c_str(), GuestHex(records, 12, 12).c_str());
    if (records && !s.records.empty()) Paste(records, s.records.data(), s.records.size());
    Paste(snapshot, s.snapshot.data(), s.snapshot.size());
    uint32_t extra = R32(snapshot + kSnapshotExtra);
    uint32_t loadout = std::min<uint32_t>(LoadoutSize(), uint32_t(s.loadout.size()));
    if (extra && loadout) Paste(extra, s.loadout.data(), loadout);
    // the globals but the pointer between the flag and the stats
    Paste(kGlobals, s.globals.data(), kGlobalsPointer);
    Paste(kGlobals + kGlobalsPointer + 4, s.globals.data() + kGlobalsPointer + 4, s.globals.size() - kGlobalsPointer - 4);
    G<uint8_t>(kFlag) = 1;
    W32(kCheckpointId, s.id);
    W32(kSnapshotValid, 1);
    LOGR("checkpoint: apply: after: flag %u id %u valid %u | globals %s | snapshot %s | records %s",
         unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId), R32(kSnapshotValid), GuestHex(kGlobals, kGlobalsSize).c_str(),
         GuestHex(snapshot, kSnapshotSize, 48).c_str(), GuestHex(records, 12, 12).c_str());
    LOGR("checkpoint: applied: checkpoint %u, %zu records, loadout %u of %zu bytes, records buffer %08x, loadout buffer %08x",
         s.id, s.records.size() / kRecordSize, loadout, s.loadout.size(), records, extra);
    return true;
}

// the live level is the saved one's (same record count), nothing is written
bool Matches(const Saved& s) {
    uint32_t world = R32(kWorld);
    if (!world) {
        LOGE("checkpoint: no world to compare the save with");
        return false;
    }
    uint32_t count = R32(world + kRecordCount);
    if (uint64_t(count) * kRecordSize != s.records.size()) {
        LOGE("checkpoint: the level has %u records, the save %zu", count, s.records.size() / kRecordSize);
        return false;
    }
    return true;
}

// a level is running with the player in it, the HUD up
bool LevelReady() {
    uint32_t obj = R32(kGame);
    uint32_t world = R32(kWorld);
    if (!obj || !world) return false;
    if (R32(obj + kGameState) != obj + kPlayState) return false;
    uint32_t me = world + kWorldPlayer;
    if (R32(me + kCharKind) != kPlayerKind || R32(me + kCharDying)) return false;
    return game::HudShown();
}

const char* Where(uint32_t obj) {
    if (!obj) return "none";
    uint32_t state = R32(obj + kGameState);
    return state == obj + kPlayState ? "level" : state == obj + kFrontEnd ? "front end" : "other";
}

// the slot (0 to 7) of a level byte in the mission table, -1 when it is in none
int Slot(uint32_t level) {
    for (uint32_t i = 0; i < kMissions; i++)
        if (G<uint8_t>(kMissionTable + i) == level) return int(i);
    return -1;
}

// the profile's unlock byte of a level on a difficulty, 0 when there is none
uint32_t UnlockAddress(uint32_t level, uint32_t difficulty) {
    int slot = Slot(level);
    if (slot < 0 || difficulty > 3) return 0;
    return kUnlockTable + ((uint32_t(slot) + difficulty * 8) << 5);
}

// one line with everything the checkpoint depends on
std::string Text(uint32_t va) {
    if (!va) return "";
    const char* p = reinterpret_cast<const char*>(HostPtr(va));
    size_t n = 0;
    while (n < 24 && p[n] >= 32 && p[n] < 127) n++;
    return std::string(p, n);
}

void Dump(const char* tag) {
    uint32_t obj = R32(kGame);
    uint32_t world = R32(kWorld);
    uint32_t ui = R32(kFrontEndUi);
    uint32_t me = world ? world + kWorldPlayer : 0;
    LOGR("checkpoint: [%s] game %08x state %08x (%s) level %u start-at %u diff %u | world %08x player kind %u dying %u "
         "hud %d | set %u id %u valid %u | ui %08x request %u/%u",
         tag, obj, obj ? R32(obj + kGameState) : 0, Where(obj), obj ? unsigned(G<uint8_t>(obj + kLevel)) : 0,
         obj ? unsigned(G<uint8_t>(obj + kStartAt)) : 0, obj ? R32(obj + kDifficulty) : 0, world,
         me ? R32(me + kCharKind) : 0, me ? R32(me + kCharDying) : 0, int(game::HudShown()),
         unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId), R32(kSnapshotValid), ui, ui ? R32(ui + kUiRequestA) : 0,
         ui ? R32(ui + kUiRequestB) : 0);
    LOGR("checkpoint: [%s] pending state %08x stage %u | page '%s' next '%s'", tag, obj ? R32(obj + kPendingState) : 0,
         obj ? R32(obj + kStateStage) : 0, Text(ui ? ui + kUiPage : 0).c_str(), Text(ui ? ui + kUiNextPage : 0).c_str());
    // the level's own loading: the play state steps through its load (sub_0020CE00) one number at a time, the loading
    // screen keeps a timer; a number that stays the same is the step the load waits on
    uint32_t timer = ui ? R32(ui + kUiLoadTimer) : 0;
    float seconds;
    std::memcpy(&seconds, &timer, sizeof(seconds));
    uint32_t lw = R32(kLoadWorld);
    LOGR("checkpoint: [%s] play state step %u, world load step %u | loading screen timer %.2f running %u", tag,
         obj ? R32(obj + kPlayState + kPlayStep) : 0, lw ? R32(lw + kLoadWorldStep) : 0, double(seconds),
         ui ? unsigned(G<uint8_t>(ui + kUiLoadRunning)) : 0);
    LOGR("checkpoint: [%s] level %u start-at %u part %u", tag, obj ? unsigned(G<uint8_t>(obj + kLevel)) : 0,
         obj ? unsigned(G<uint8_t>(obj + kStartAt)) : 0, obj ? unsigned(G<uint8_t>(obj + kPart)) : 0);
}

// every tick while a load is followed: one line each time anything the checkpoint depends on changes (and a heartbeat)
void TickWatch(double seconds) {
    uint32_t obj = R32(kGame);
    uint32_t world = R32(kWorld);
    uint32_t ui = R32(kFrontEndUi);
    uint32_t lw = R32(kLoadWorld);
    uint32_t me = world ? world + kWorldPlayer : 0;
    std::string page = Text(ui ? ui + kUiPage : 0), next = Text(ui ? ui + kUiNextPage : 0);
    uint32_t v[24] = {obj,
                      obj ? R32(obj + kGameState) : 0,
                      obj ? R32(obj + kPendingState) : 0,
                      obj ? R32(obj + kStateStage) : 0,
                      obj ? R32(obj + kPlayState + kPlayStep) : 0,
                      lw ? R32(lw + kLoadWorldStep) : 0,
                      obj ? uint32_t(G<uint8_t>(obj + kLevel)) : 0,
                      obj ? uint32_t(G<uint8_t>(obj + kStartAt)) : 0,
                      obj ? uint32_t(G<uint8_t>(obj + kPart)) : 0,
                      uint32_t(G<uint8_t>(kFlag)),
                      R32(kCheckpointId),
                      R32(kSnapshotValid),
                      world,
                      uint32_t(game::HudShown()),
                      me ? R32(me + kCharKind) : 0,
                      me ? R32(me + kCharDying) : 0,
                      obj ? R32(obj + kReloadDword) : 0,
                      obj ? uint32_t(G<uint8_t>(obj + kReloadByte)) : 0,
                      ui ? R32(ui + kUiRequestA) : 0,
                      ui ? R32(ui + kUiRequestB) : 0,
                      ui ? R32(ui + kUiLoadTimer) : 0,
                      ui ? uint32_t(G<uint8_t>(ui + kUiLoadRunning)) : 0,
                      Hash(reinterpret_cast<const uint8_t*>(page.data()), page.size()),
                      Hash(reinterpret_cast<const uint8_t*>(next.data()), next.size())};
    static uint32_t last[24] = {};
    static bool first = true;
    static Clock::time_point at;
    bool changed = first || std::memcmp(v, last, sizeof(v)) != 0;
    if (!changed && Clock::now() - at < std::chrono::seconds(2)) return;
    first = false;
    at = Clock::now();
    std::memcpy(last, v, sizeof(v));
    float timer;
    std::memcpy(&timer, &v[20], sizeof(timer));
    LOGR("checkpoint: [watch %.2fs%s] state %08x(%s) pending %08x stage %u | play step %u world step %u | level %u "
         "start-at %u part %u | flag %u id %u valid %u | world %08x hud %u kind %u dying %u | reload %u/%u | ui req "
         "%u/%u timer %.2f run %u | page '%s' next '%s'",
         seconds, changed ? " *" : "", v[1], Where(obj), v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
         v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], double(timer), v[21], page.c_str(), next.c_str());
}

void Refuse(const char* why) {
    LOGR("checkpoint: not started: %s", why);
    SetStatus(why);
}

// game thread only: the load's steps, logged each time one changes (a timeline of where the time goes and where it
// stops), and a warning when one stays the same for a long time
bool g_request_fresh = false;  // a new ask: the progress log starts over
Clock::time_point g_watch_until;  // also followed after the saved bytes went in, to see how far the second load gets
Clock::time_point g_progress_at;
bool g_stall_logged = false;

void TrackProgress(double seconds) {
    uint32_t obj = R32(kGame);
    uint32_t lw = R32(kLoadWorld);
    uint32_t now[3] = {obj ? R32(obj + kStateStage) : 0, obj ? R32(obj + kPlayState + kPlayStep) : 0,
                       lw ? R32(lw + kLoadWorldStep) : 0};
    static uint32_t last[3] = {~0u, ~0u, ~0u};
    if (g_request_fresh) {
        last[0] = last[1] = last[2] = ~0u;
        g_request_fresh = false;
    }
    if (std::memcmp(now, last, sizeof(now)) != 0) {
        LOGR("checkpoint: load progress at %.1fs: state stage %u, play state step %u, world load step %u", seconds, now[0],
             now[1], now[2]);
        std::memcpy(last, now, sizeof(now));
        g_progress_at = Clock::now();
        g_stall_logged = false;
    } else if (!g_stall_logged && now[0] == 1 && Clock::now() - g_progress_at > std::chrono::seconds(15)) {
        g_stall_logged = true;
        LOGE("checkpoint: STALL: the load has been on world step %u for 15s",
             now[2]);
        Dump("stall");
    }
}

// game thread only: the last place the game was seen in while waiting, to log the changes
const char* g_last_where = "";
int g_last_steady = 0;

// checkpoint_mode.txt in the files folder, one digit, read at each load, so ways can be tried without a new build:
//   1 the game's own RESTART MISSION     2 the state change alone (sub_0020DCA0)
//   3 the saved bytes go into the level that is up and it loads again (sub_0020DBC0), the way the first version did
int RestartMode() {
    std::ifstream in(app::ExeDir() / "checkpoint_mode.txt");
    char c = 0;
    while (in.get(c))
        if (c == '1' || c == '2' || c == '3') return c - '0';
        else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
    return 3;
}

void Fail(const char* why) {
    LOGE("checkpoint: %s", why);
    SetStatus(why);
    g_phase = Phase::Idle;
}

// game thread: the ask. In a level it is the saved one's, from the level select the level is started first
void Begin(X86Context& ctx) {
    auto s = Current();
    if (!s) return Refuse("NO SAVED CHECKPOINT");
    uint32_t obj = R32(kGame);
    if (!obj) return Refuse("THE GAME IS NOT READY");
    Dump("asked");
    LOGR("checkpoint: the save: mission %u (level %u), difficulty %u, checkpoint %u, loadout %zu bytes, %zu records",
         s->mission, s->level, s->difficulty, s->id, s->loadout.size(), s->records.size() / kRecordSize);
    uint32_t state = R32(obj + kGameState);
    g_last_where = Where(obj);
    g_last_steady = 0;
    if (state == obj + kPlayState) {
        if (G<uint8_t>(obj + kLevel) != s->level) {
            LOGR("checkpoint: playing level %u, the save is of level %u", unsigned(G<uint8_t>(obj + kLevel)), s->level);
            return Refuse("GO TO THE MAIN MENU FIRST");
        }
        LOGR("checkpoint: in the saved level, waiting for the player");
    } else if (state == obj + kFrontEnd) {
        if (!s->mission || s->mission > kMissions || s->difficulty > 3) {
            LOGR("checkpoint: mission %u, difficulty %u: not something the level select starts", s->mission, s->difficulty);
            return Refuse("THE SAVE IS NOT FOR THIS GAME");
        }
        G<uint8_t>(obj + kLevel) = uint8_t(s->level);
        W32(obj + kDifficulty, s->difficulty);
        // every start of a mission in the game writes start-at 1 (a fresh process has 0, which the game never loads with):
        // the same here, the saved checkpoint goes in later by Finish
        G<uint8_t>(obj + kStartAt) = 1;
        if (G<uint8_t>(obj + kPart) == 0) G<uint8_t>(obj + kPart) = 1;
        LOGR("checkpoint: start-at set to 1, part byte %u", unsigned(G<uint8_t>(obj + kPart)));
        // sub_00124CF0 returns without doing anything when the mission is locked on this difficulty in the profile:
        // the flag is set for the call and put back after, so the profile is not unlocked for good
        uint32_t flag = UnlockAddress(s->level, s->difficulty);
        if (!flag) {
            LOGR("checkpoint: level %u is in no slot of the mission table", s->level);
            return Refuse("THE SAVE IS NOT FOR THIS GAME");
        }
        uint8_t was = G<uint8_t>(flag);
        LOGR("checkpoint: begin: mission table %s | unlock byte of this level+difficulty at %08x | obj %08x level %u diff %u",
             GuestHex(kMissionTable, kMissions, 8).c_str(), flag, obj, unsigned(G<uint8_t>(obj + kLevel)),
             R32(obj + kDifficulty));
        G<uint8_t>(flag) = 1;
        LOGR("checkpoint: calling the level select's start (unlock byte %08x was %u)", flag, unsigned(was));
        X86Context saved = ctx;
        CallGuest(ctx, kStartLevel, {});
        LOGR("checkpoint: the start returned eax=%08x", ctx.eax);
        ctx = saved;
        G<uint8_t>(flag) = was;
        Dump("after the start");
        if (kEnterDirectly) {
            // the menu would only go to the movie page: leave it no request, and change the game's state to the level
            uint32_t ui = R32(kFrontEndUi);
            if (ui) {
                W32(ui + kUiRequestA, 0);
                W32(ui + kUiRequestB, 0);
            }
            X86Context before = ctx;
            ctx.edi = obj;
            CallGuest(ctx, kSetState, {obj + kPlayState});
            ctx = before;
            LOGR("checkpoint: the game's state is being changed to the level (%08x)", obj + kPlayState);
            Dump("after the state change");
        }
    } else {
        LOGR("checkpoint: state %08x is neither the front end (%08x) nor a level (%08x)", state, obj + kFrontEnd,
             obj + kPlayState);
        return Refuse("NOT FROM HERE: USE THE MAIN MENU");
    }
    SetStatus("LOADING THE MISSION");
    g_phase = Phase::Waiting;
}

// game thread: the level is up, the saved bytes go in and the level loads again as the game's CONTINUE does it
void Finish(X86Context& ctx, double waited) {
    auto s = Current();
    LOGR("checkpoint: the level is up after %.1fs, flag %u, checkpoint %u before the saved one goes in", waited,
         unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId));
    Dump("level up");
    // nothing of the save is written into the level that is about to be taken down: the game clears the checkpoint
    // globals when the load starts (sub_001B3E80), so the bytes go in from the hooks below, in the new load
    if (!s || !Matches(*s)) return Fail("THE SAVE IS NOT FOR THIS LEVEL");
    uint32_t obj = R32(kGame);
    // The level is running, so it is restarted the way the game's own RESTART MISSION does it (sub_00124190): the reload
    // flags, then the state change to the play state again. Its first stage (the play state's exit, sub_0020CFC0, steps
    // 0x1C to 0x1F to 0x37) takes the level down, and then the load starts over from step 1. sub_0020DBC0 (kReload)
    // goes straight to the load, the way CONTINUE does after a death, when the level is already down: on a level that
    // is still up it loads over it without the teardown, which is what crashed.
    W32(obj + kReloadDword, 1);
    G<uint8_t>(obj + kReloadByte) = 0;
    g_apply = s;
    g_apply_at = Clock::now();
    LogSaved(*s, "finish");
    Dump("before the restart");
    int mode = RestartMode();
    LOGR("checkpoint: restarting in mode %d", mode);
    X86Context saved = ctx;
    if (mode == 3) {
        // the first version's way: the bytes go into the level that is up, then it loads again without a teardown
        if (!Apply(*s)) {
            ctx = saved;
            g_apply.reset();
            return Fail("THE SAVE COULD NOT BE PUT BACK");
        }
        ctx.edi = obj;
        CallGuest(ctx, kReload, {});
    } else if (mode == 1) {
        // the game's own RESTART MISSION: resets the UI pages first, then changes the state, then writes start-at 1
        CallGuest(ctx, kRestartMission, {});
    } else {
        ctx.edi = obj;
        CallGuest(ctx, kSetState, {obj + kPlayState});
    }
    ctx = saved;
    // after the call: RESTART MISSION writes start-at 1 and clears the checkpoint behind the state change
    G<uint8_t>(obj + kStartAt) = uint8_t(s->id);
    Dump("after the restart, start-at written");
    LOGR("checkpoint: loading checkpoint %u again, waiting for the game to ask for it", s->id);
    g_watch_until = Clock::now() + std::chrono::seconds(90);
    g_request_fresh = true;
    g_progress_at = Clock::now();
    Dump("reload called");
    SetStatus("LOADING THE CHECKPOINT");
    g_phase = Phase::Idle;
}

}  // namespace

std::string checkpoint::Label() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Ensure();
    if (g_phase.load() == Phase::Waiting) return "WAITING FOR THE LEVEL, TAP TO RETRY";
    if (!g_status.empty() && Clock::now() - g_status_at < std::chrono::seconds(8)) return g_status;
    if (!g_saved) return "NO SAVED CHECKPOINT";
    char text[64];
    if (g_saved->mission) std::snprintf(text, sizeof(text), "MISSION %u, CHECKPOINT %u", g_saved->mission, g_saved->id);
    else std::snprintf(text, sizeof(text), "CHECKPOINT %u", g_saved->id);
    return text;
}

// The title menu button's text. checkpoint_label.txt in the files folder is its model, one line, {c} the checkpoint's
// number and {m} the mission's; it is read again every couple of seconds, so it can be changed with the game open
std::string checkpoint::ButtonText() {
    static const char kDefault[] = "CONTINUAR: CHECKPOINT {c} MISSAO {m}";
    static std::string model;
    static Clock::time_point read_at;
    std::lock_guard<std::mutex> lock(g_mutex);
    Ensure();
    if (model.empty() || Clock::now() - read_at > std::chrono::seconds(2)) {
        read_at = Clock::now();
        std::string line;
        std::ifstream in(app::ExeDir() / "checkpoint_label.txt");
        if (!in || !std::getline(in, line)) line.clear();
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // the mark Notepad puts first
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        model = line.empty() ? kDefault : line;
    }
    if (!g_saved) return "CONTINUAR";
    std::string out;
    for (size_t i = 0; i < model.size();) {
        if (model.compare(i, 3, "{c}") == 0) {
            out += std::to_string(g_saved->id);
            i += 3;
        } else if (model.compare(i, 3, "{m}") == 0) {
            out += g_saved->mission ? std::to_string(g_saved->mission) : "?";
            i += 3;
        } else {
            out += model[i++];
        }
    }
    return out.substr(0, 120);
}

bool checkpoint::HasSave() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Ensure();
    return bool(g_saved);
}

void checkpoint::Continue() {
    g_auto_retries = 0;
    g_request = true;
}

// game thread, each menu and HUD tick (hooks.cpp MenuTick)
void CheckpointTick(X86Context& ctx) {
    // once, so a log shows this build is the one running and what the save file holds
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::error_code ec;
        auto size = std::filesystem::file_size(Path(), ec);
        std::string held = ec ? "no file yet" : std::to_string(size) + " bytes";
        LOGR("checkpoint: v5 running, %s: %s", Path().string().c_str(), held.c_str());
        crashlog::Install();
        LOGR("checkpoint: crash log %s, restart mode %d", crashlog::fd >= 0 ? "on (checkpoint_crash.log)" : "NOT opened", RestartMode());
    }
    static Clock::time_point began;
    static int steady;
    if (g_apply && Clock::now() - g_apply_at > std::chrono::seconds(120)) {
        LOGE("checkpoint: the load never asked for the checkpoint");
        Dump("never asked");
        g_apply.reset();
    }
    static Clock::time_point logged;
    if (g_request.exchange(false)) {
        // asked again: whatever was waiting starts over
        if (g_phase.load() == Phase::Waiting) LOGR("checkpoint: asked again while waiting, starting over");
        g_phase = Phase::Idle;
        began = Clock::now();
        g_request_fresh = true;
        logged = began;
        steady = 0;
        Begin(ctx);
    }
    if (g_phase.load() != Phase::Waiting) {
        // after the saved bytes went in the second load is followed too: the last line before a crash is where it was
        if (Clock::now() < g_watch_until) {
            double since = std::chrono::duration<double>(Clock::now() - began).count();
            TrackProgress(since);
            TickWatch(since);
        }
        return;
    }
    double waited = std::chrono::duration<double>(Clock::now() - began).count();
    if (waited > 300) {
        Dump("timeout");
        return Fail("THE MISSION DID NOT START");
    }
    // where the game is now, logged when it changes: the front end, a loading state, the level
    const char* where = Where(R32(kGame));
    if (std::strcmp(where, g_last_where) != 0) {
        LOGR("checkpoint: the game went from the %s to the %s after %.1fs", g_last_where, where, waited);
        g_last_where = where;
        Dump("state change");
    }
    TrackProgress(waited);
    TickWatch(waited);
    if (kAutoRetry && g_stall_logged && g_auto_retries.load() < 2 &&
        Clock::now() - g_progress_at > std::chrono::seconds(20)) {
        int n = ++g_auto_retries;
        LOGR("checkpoint: the load is stuck, starting it again (try %d)", n);
        g_request = true;
        return;
    }
    // a couple of seconds of the player standing in the level, so the game is done setting it up
    steady = LevelReady() ? steady + 1 : 0;
    if (steady == 1 && g_last_steady == 0) LOGR("checkpoint: the level looks ready, counting ticks (%.1fs)", waited);
    if (steady == 0 && g_last_steady > 0)
        LOGR("checkpoint: the level stopped being ready after %d ticks (%.1fs)", g_last_steady, waited);
    g_last_steady = steady;
    if (Clock::now() - logged > std::chrono::seconds(2)) {
        logged = Clock::now();
        char tag[48];
        std::snprintf(tag, sizeof(tag), "waiting %.0fs steady %d/120", waited, steady);
        Dump(tag);
    }
    if (steady >= 120) Finish(ctx, waited);
}

// A checkpoint is made (eax its number, ebx the world's copy of the player's loadout): the file follows
X86_EXTERN_FUNC(__imp__sub_001C8D20);
X86_FUNC(sub_001C8D20) {
    uint32_t snapshot = ctx.ebx;
    uint32_t id = ctx.eax;
    __imp__sub_001C8D20(ctx, base);
    Save(snapshot, id);
}

// The game puts a checkpoint back as a level loads over one: the saved bytes go into the buffers this load made,
// whatever the load did to the ones before it
X86_EXTERN_FUNC(__imp__sub_001BEF10);
X86_FUNC(sub_001BEF10) {
    LOGR("checkpoint: the game puts a checkpoint back (saved bytes %s)", g_apply ? "pending" : "none");
    if (g_apply) {
        auto s = std::move(g_apply);
        g_apply.reset();
        if (Apply(*s)) LOGR("checkpoint: put back as the level loaded");
        g_quiet_until = Clock::now() + std::chrono::seconds(20);
    }
    uint32_t lw = R32(kLoadWorld);
    uint32_t dest = lw ? R32(lw + 0x930) : 0;
    LOGR("checkpoint: sub_001BEF10: before: flag %u id %u | world %08x dest records %08x count %u source %08x | dest %s "
         "| source %s | start-at %u",
         unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId), lw, dest, lw ? R32(lw + kRecordCount) : 0,
         R32(kGlobals + kGlobalsPointer), GuestHex(dest, 12, 12).c_str(),
         GuestHex(R32(kGlobals + kGlobalsPointer), 12, 12).c_str(), unsigned(G<uint8_t>(R32(kGame) + kStartAt)));
    __imp__sub_001BEF10(ctx, base);
    LOGR("checkpoint: sub_001BEF10: after: dest %s | clock %08x | globals %s", GuestHex(dest, 12, 12).c_str(),
         R32(kGlobals), GuestHex(kGlobals, kGlobalsSize).c_str());
}

// The game clears the checkpoint flag and its globals as every load starts (step 0x37, the only caller): when a saved
// checkpoint is waiting, the flag goes back right after, so the load reaches the restore at the end of step 5
X86_EXTERN_FUNC(__imp__sub_001B3E80);
X86_FUNC(sub_001B3E80) {
    LOGR("checkpoint: sub_001B3E80 (the load clears the checkpoint): before: flag %u id %u valid %u start-at %u pending "
         "apply %s",
         unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId), R32(kSnapshotValid),
         unsigned(G<uint8_t>(R32(kGame) + kStartAt)), g_apply ? "yes" : "no");
    __imp__sub_001B3E80(ctx, base);
    if (g_apply) {
        G<uint8_t>(kFlag) = 1;
        W32(kCheckpointId, g_apply->id);
        W32(kSnapshotValid, 1);
        LOGR("checkpoint: the game cleared the checkpoint as the load started, set again (id %u)", g_apply->id);
    }
}

// Diagnostics: the world load's step 10 waits for this to return 1. Logged when the result changes and every 5s
// while it stays 0, with the pool it allocates from (current, end), to see what it waits on
X86_EXTERN_FUNC(__imp__sub_0013EF10);
X86_FUNC(sub_0013EF10) {
    uint32_t ecx_in = ctx.ecx;
    uint32_t arg = R32(ctx.esp + 4);
    __imp__sub_0013EF10(ctx, base);
    static int last = -1;
    static Clock::time_point at;
    int ret = int(uint8_t(ctx.eax));
    if (ret != last || (ret == 0 && Clock::now() - at > std::chrono::seconds(5))) {
        last = ret;
        at = Clock::now();
        uint32_t render = R32(0x2D0D60);
        uint32_t pool = render ? R32(render + 0xD3E4) : 0;
        uint32_t lw = R32(kLoadWorld);
        LOGR("checkpoint: sub_0013EF10 ecx=%u arg=%08x ret=%d | world %08x res5b2c %08x | pool %08x cur %08x end %08x "
             "| start-at %u",
             ecx_in, arg, ret, lw, lw ? R32(lw + 0x5B2C) : 0, pool, pool ? R32(pool + 0x40) : 0,
             pool ? R32(pool + 0x44) : 0, unsigned(G<uint8_t>(R32(kGame) + kStartAt)));
    }
}

// ---- traces of the game's own state machine. A hook is only here to log: it calls the game's function and reports.
// (each one logs when its numbers change, and every 2s while they stay)

// the state change: edi the game object, [esp+4] the state to go to
X86_EXTERN_FUNC(__imp__sub_0020DCA0);
X86_FUNC(sub_0020DCA0) {
    uint32_t obj = ctx.edi;
    uint32_t to = R32(ctx.esp + 4);
    LOGR("checkpoint: sub_0020DCA0 (state change) to %08x from %08x (obj %08x) stage %u pending %08x", to,
         obj ? R32(obj + kGameState) : 0, obj, obj ? R32(obj + kStateStage) : 0, obj ? R32(obj + kPendingState) : 0);
    __imp__sub_0020DCA0(ctx, base);
    LOGR("checkpoint: sub_0020DCA0 done: stage %u pending %08x reload %u/%u start-at %u", obj ? R32(obj + kStateStage) : 0,
         obj ? R32(obj + kPendingState) : 0, obj ? R32(obj + kReloadDword) : 0,
         obj ? unsigned(G<uint8_t>(obj + kReloadByte)) : 0, obj ? unsigned(G<uint8_t>(obj + kStartAt)) : 0);
}

// the load again without the teardown (the game's CONTINUE after a death): edi the game object
X86_EXTERN_FUNC(__imp__sub_0020DBC0);
X86_FUNC(sub_0020DBC0) {
    uint32_t obj = ctx.edi;
    LOGR("checkpoint: sub_0020DBC0 (reload without teardown): obj %08x play step %u flag %u id %u", obj,
         obj ? R32(obj + kPlayState + kPlayStep) : 0, unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId));
    __imp__sub_0020DBC0(ctx, base);
    LOGR("checkpoint: sub_0020DBC0 done: stage %u pending %08x", obj ? R32(obj + kStateStage) : 0,
         obj ? R32(obj + kPendingState) : 0);
}

// the play state's load, one tick: ecx the play state, its step at +4; al is 1 when the step finished
X86_EXTERN_FUNC(__imp__sub_0020CE00);
X86_FUNC(sub_0020CE00) {
    uint32_t self = ctx.ecx;
    uint32_t before = self ? R32(self + 4) : 0;
    __imp__sub_0020CE00(ctx, base);
    uint32_t after = self ? R32(self + 4) : 0;
    static EdgeLog edge;
    if (edge.Hit((uint64_t(before) << 40) ^ (uint64_t(after) << 16) ^ uint64_t(uint8_t(ctx.eax))))
        LOGR("checkpoint: sub_0020CE00 (play state load tick): step %u -> %u, ret %u | flag %u id %u start-at %u", before,
             after, unsigned(uint8_t(ctx.eax)), unsigned(G<uint8_t>(kFlag)), R32(kCheckpointId),
             unsigned(G<uint8_t>(R32(kGame) + kStartAt)));
}

// the play state's exit (the teardown, steps 0x1c to 0x37): ecx the play state
X86_EXTERN_FUNC(__imp__sub_0020CFC0);
X86_FUNC(sub_0020CFC0) {
    uint32_t self = ctx.ecx;
    uint32_t before = self ? R32(self + 4) : 0;
    __imp__sub_0020CFC0(ctx, base);
    uint32_t after = self ? R32(self + 4) : 0;
    static EdgeLog edge;
    if (edge.Hit((uint64_t(before) << 40) ^ (uint64_t(after) << 16) ^ uint64_t(uint8_t(ctx.eax))))
        LOGR("checkpoint: sub_0020CFC0 (play state exit tick): step %u -> %u, ret %u", before, after,
             unsigned(uint8_t(ctx.eax)));
}

// the world's load, one tick: [esp+4] the world; al 1 when the whole load is done
X86_EXTERN_FUNC(__imp__sub_001EDC90);
X86_FUNC(sub_001EDC90) {
    uint32_t w = R32(ctx.esp + 4);
    uint32_t before = w ? R32(w + kLoadWorldStep) : 0;
    __imp__sub_001EDC90(ctx, base);
    uint32_t after = w ? R32(w + kLoadWorldStep) : 0;
    static EdgeLog edge;
    if (edge.Hit((uint64_t(before) << 40) ^ (uint64_t(after) << 16) ^ uint64_t(uint8_t(ctx.eax))))
        LOGR("checkpoint: sub_001EDC90 (world load tick): world %08x step %u -> %u, ret %u | level %u start-at %u part %u "
             "(world copies %u/%u/%u)",
             w, before, after, unsigned(uint8_t(ctx.eax)), unsigned(G<uint8_t>(R32(kGame) + kLevel)),
             unsigned(G<uint8_t>(R32(kGame) + kStartAt)), unsigned(G<uint8_t>(R32(kGame) + kPart)),
             w ? unsigned(G<uint8_t>(w + 0x5AEC)) : 0, w ? R32(w + 0x5AF0) : 0, w ? unsigned(G<uint8_t>(w + 0x5AED)) : 0);
}
