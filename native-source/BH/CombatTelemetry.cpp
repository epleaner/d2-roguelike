#define NOMINMAX
#include "CombatTelemetry.h"
#include "ObservationTelemetry.h"
#include "ServerMonsters.h"
#include "Constants.h"
#include "LabProfile.h"
#include "D2Ptrs.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <detours.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <intrin.h>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using json = nlohmann::json;
constexpr size_t kRing = 4096, kActors = 16, kMissiles = 256, kEffects = 64;
constexpr DWORD kMinute = 60000;
struct Key { DWORD type, id, cls; };
constexpr Key Missing() { return {0xffffffff, 0xffffffff, 0xffffffff}; }
enum Kind : DWORD { Damage = 1, Death, Cold, ProjectileCreate, ProjectileDestroy, HitResult, HitRoll };
struct Event {
    uint64_t sequence;
    DWORD kind, frame, flags, transition;
    Key source, target;
    int32_t hpBefore, hpAfter, gross, debitHp;
    int32_t phys, fire, lightning, magic, cold, poison;
    int32_t slow67, slow68, slow69;
    DWORD expiry, beforeMode, afterMode;
    int32_t skill, skillLevel, x, y, totalFrames, currentFrame;
};
static_assert(sizeof(Event) <= 160 && std::is_trivial<Event>::value &&
    std::is_standard_layout<Event>::value, "bounded POD trace event");
struct Slot { std::atomic_flag busy = ATOMIC_FLAG_INIT; Event event{}; };
std::array<Slot, kRing> ring;
std::array<Key, kActors> actors{};
struct MissileEntry { Key missile, owner; };
std::array<MissileEntry, kMissiles> missiles{};
struct EffectState { Key target{}, source{}; DWORD expiry = 0; int slow67 = 0, slow68 = 0, slow69 = 0; bool valid = false; };
std::array<EffectState, kEffects> effects{};
std::atomic<bool> capturing{false}, restored{true}, publicationOpen{false}, finalized{true};
std::atomic<uintptr_t> serverGame{0};
std::atomic<DWORD> serverThread{0};
std::atomic<uint64_t> nextSequence{0}, lost{0}, wrongThread{0}, missileOverflow{0},
    effectOverflow{0}, readGaps{0}, interrupted{0};
std::atomic<uint32_t> inFlight{0};
std::mutex control, lifecycle;
std::string traceId, epoch;
DWORD deadline = 0;
HANDLE watchdog = nullptr, stopWatchdog = nullptr;
bool attached = false;
bool runHookAttached = false;
std::atomic<CombatTelemetry::KillListener> runListener{nullptr};
std::atomic<CombatTelemetry::DamageListener> runDamageListener{nullptr};

using DamageNative = void (__stdcall*)(void*, UnitAny*, BOOL, void*);
// D2Game DE0C0: EAX=defender, stack=(game, attacker, bMissile, damage), ret 16.
using ReactionNative = void (__stdcall*)(void*, UnitAny*, void*);
// D2Game DCE90: EAX=attacker, stack=(defender, stat value, missile), ret 12.
using RollNative = int (__stdcall*)(UnitAny*, int, int);
using KillNative = void (__fastcall*)(void*, UnitAny*, UnitAny*, int);
using ColdNative = void (__stdcall*)(int);
using RemoveNative = void (__fastcall*)(UnitAny*, int, void*);
using CreateNative = UnitAny* (__fastcall*)(void*, void*);
using FreeNative = void (__stdcall*)(UnitAny*);
using Getter = DWORD (__stdcall*)(UnitAny*, DWORD, DWORD);
using StateList = void* (__stdcall*)(UnitAny*, int);
using ListStat = int (__stdcall*)(void*, int, int);
using CheckState = BOOL (__stdcall*)(UnitAny*, int);
DamageNative originalDamage = nullptr;
ReactionNative originalReaction = nullptr;
RollNative originalRoll = nullptr;
KillNative originalKill = nullptr;
ColdNative originalCold = nullptr;
RemoveNative originalRemove = nullptr;
CreateNative originalCreate = nullptr;
FreeNative originalFree = nullptr;
Getter getStat = nullptr;
StateList getStateList = nullptr;
ListStat getListStat = nullptr;
void* volatile* getterSlot = nullptr;
CheckState checkState = nullptr;

bool Equal(const Key& a, const Key& b) {
    return a.type == b.type && a.id == b.id && a.cls == b.cls;
}
Key Identify(const UnitAny* unit) {
    return unit ? Key{unit->dwType, unit->dwUnitId, unit->dwTxtFileNo} : Missing();
}
DWORD Frame() {
    auto game = reinterpret_cast<const BYTE*>(serverGame.load(std::memory_order_relaxed));
    return game ? *reinterpret_cast<const DWORD*>(game + 0xA8) : 0;
}
bool StartAdmission() {
    inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (capturing.load(std::memory_order_acquire)) return true;
    inFlight.fetch_sub(1, std::memory_order_acq_rel);
    return false;
}
struct HookAdmission {
    bool admitted = StartAdmission();
    ~HookAdmission() { if (admitted) inFlight.fetch_sub(1, std::memory_order_acq_rel); }
};
struct FinishAdmission {
    bool admitted;
    ~FinishAdmission() { if (admitted) inFlight.fetch_sub(1, std::memory_order_acq_rel); }
};
bool Covered() {
    if (GetCurrentThreadId() != serverThread.load(std::memory_order_relaxed)) {
        wrongThread.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}
bool Tracked(const Key& key) {
    for (const Key& actor : actors) if (Equal(actor, key)) return true;
    return false;
}
void Publish(Event& event) {
    if (!publicationOpen.load(std::memory_order_acquire)) {
        lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    event.sequence = nextSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    Slot& slot = ring[(event.sequence - 1) % kRing];
    if (slot.busy.test_and_set(std::memory_order_acquire)) {
        lost.fetch_add(1, std::memory_order_relaxed); return;
    }
    slot.event = event;
    slot.busy.clear(std::memory_order_release);
}
int RawHp(UnitAny* unit) { return unit && getStat ? static_cast<int>(getStat(unit, 6, 0)) : 0; }
struct DamageContext {
    DamageContext* prior = nullptr;
    UnitAny* defender = nullptr;
    const BYTE* damage = nullptr;
    Key source{}, target{};
    int hp = 0, debitHp = 0, debitGross = 0;
    bool record = false, debitEntered = false, admitted = false;
};
__declspec(thread) DamageContext* activeDamage = nullptr;
BYTE* gameImage = nullptr;
DWORD __stdcall GetterHook(UnitAny* unit, DWORD stat, DWORD layer) {
    DWORD actual = getStat(unit, stat, layer);
    DamageContext* ctx = activeDamage;
    if (ctx && ctx->record && ctx->defender == unit && ctx->damage &&
        stat == 6 && layer == 0 && _ReturnAddress() == gameImage + 0xDE253) {
        ctx->debitEntered = true;
        ctx->debitHp = static_cast<int>(actual);
        ctx->debitGross = *reinterpret_cast<const int*>(ctx->damage + 0x4c);
    }
    return actual;
}
void __stdcall BeforeDamage(DamageContext* ctx, UnitAny* defender, void* game,
    UnitAny* attacker, BOOL bMissile, const BYTE* damage) {
    ctx->prior = activeDamage;
    ctx->damage = damage; // Also track untraced run hits for nested deduplication.
    // DE0C0 can nest while processing a hit. The same native damage record
    // must not receive the run multiplier twice in a nested invocation.
    bool alreadyScaled = false;
    for (DamageContext* prior = ctx->prior; prior; prior = prior->prior)
        if (prior->damage == damage) { alreadyScaled = true; break; }
    auto scale = runDamageListener.load(std::memory_order_acquire);
    if (scale && damage && !alreadyScaled)
        scale(game, attacker, defender, bMissile, const_cast<BYTE*>(damage));
    ctx->admitted = StartAdmission();
    ctx->record = ctx->admitted &&
        game == reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) &&
        defender && Tracked(Identify(defender)) && Covered();
    if (ctx->record) {
        ctx->defender = defender; ctx->damage = damage;
        ctx->source = Identify(attacker); ctx->target = Identify(defender);
        ctx->hp = RawHp(defender);
    }
    ctx->debitEntered = false;
    activeDamage = ctx;
}
void __stdcall AfterDamage(DamageContext* ctx, const BYTE* damage) {
    FinishAdmission finish{ctx->admitted};
    activeDamage = ctx->prior;
    if (!ctx->record || !damage) return;
    Event e{};
    e.kind = Damage; e.frame = Frame(); e.source = ctx->source; e.target = ctx->target;
    e.hpBefore = ctx->hp; e.hpAfter = RawHp(ctx->defender);
    e.flags = *reinterpret_cast<const WORD*>(damage + 4);
    e.transition = ctx->debitEntered ? 1 : 0;
    e.gross = ctx->debitEntered ? ctx->debitGross : 0;
    e.debitHp = ctx->debitEntered ? ctx->debitHp : 0;
    e.phys = *reinterpret_cast<const int*>(damage + 8);
    e.fire = *reinterpret_cast<const int*>(damage + 0x10);
    e.lightning = *reinterpret_cast<const int*>(damage + 0x1c);
    e.magic = *reinterpret_cast<const int*>(damage + 0x20);
    e.cold = *reinterpret_cast<const int*>(damage + 0x24);
    e.poison = *reinterpret_cast<const int*>(damage + 0x28);
    Publish(e);
}
// EAX=defender; arguments are game, attacker, bMissile, damage; native ret16.
// 64 local bytes hold the nested synchronous context. No pointer escapes the wrapper.
__declspec(naked) void DamageHook() {
    __asm {
        lea esp, [esp - 64]
        pushfd
        pushad
        lea ecx, [esp + 36]
        mov edx, [esp + 116]
        push edx
        mov edx, [esp + 116]
        push edx
        mov edx, [esp + 116]
        push edx
        mov edx, [esp + 116]
        push edx
        push dword ptr [esp + 44]
        push ecx
        call BeforeDamage
        popad
        popfd
        mov edx, [esp + 80]
        push edx
        mov edx, [esp + 80]
        push edx
        mov edx, [esp + 80]
        push edx
        mov edx, [esp + 80]
        push edx
        call dword ptr [originalDamage]
        pushfd
        pushad
        mov edx, [esp + 116]
        push edx
        lea edx, [esp + 40]
        push edx
        call AfterDamage
        popad
        popfd
        lea esp, [esp + 64]
        ret 16
    }
}
// DE641 is the only admitted call: GetResultFlags has already validated unit
// types, attack eligibility and melee range before entering IsHitSuccessful.
// The running-player forced-success path skips this call entirely. A passing
// roll can still be blocked or dodged afterward; it is not a resolved hit.
struct RollContext {
    Key source, target;
    DWORD frame;
    bool record, admitted;
};
static_assert(sizeof(RollContext) <= 48 && std::is_trivial<RollContext>::value,
    "bounded POD roll context");
void __stdcall BeforeRoll(RollContext* ctx, UnitAny* attacker, UnitAny* defender,
    const BYTE* returnAddress) {
    ctx->admitted = StartAdmission();
    ctx->record = ctx->admitted && returnAddress == gameImage + 0xde646 &&
        attacker && defender &&
        *reinterpret_cast<void* const*>(reinterpret_cast<const BYTE*>(attacker) + 0x80) ==
            reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) &&
        Tracked(Identify(defender)) && Covered();
    if (!ctx->record) return;
    ctx->frame = Frame();
    ctx->source = Identify(attacker); ctx->target = Identify(defender);
}
void __stdcall AfterRoll(RollContext* ctx, int result) {
    FinishAdmission finish{ctx->admitted};
    if (!ctx->record) return;
    Event e{};
    e.kind = HitRoll; e.frame = ctx->frame;
    e.source = ctx->source; e.target = ctx->target;
    e.flags = static_cast<DWORD>(result);
    Publish(e);
}
// Preserve every entry register and EFLAGS around both observers and the
// original helper, including EAX's exact native boolean on return.
__declspec(naked) void RollHook() {
    __asm {
        lea esp, [esp - 48]
        pushfd
        pushad
        lea ecx, [esp + 36]
        mov edx, [esp + 84]
        push edx
        mov edx, [esp + 92]
        push edx
        push dword ptr [esp + 36]
        push ecx
        call BeforeRoll
        popad
        popfd
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        call dword ptr [originalRoll]
        pushfd
        pushad
        push dword ptr [esp + 28]
        lea edx, [esp + 40]
        push edx
        call AfterRoll
        popad
        popfd
        lea esp, [esp + 48]
        ret 12
    }
}
// DCF20 is reached independently of ExecuteEvents (e.g. DEE9C and 92926).
// Copy the result word before the reaction can rewrite it; publish only after
// the native call returns, keeping detach quiescence across its trampoline.
struct ReactionContext {
    Key source, target;
    DWORD frame, flags;
    bool record, admitted;
};
static_assert(sizeof(ReactionContext) <= 48 && std::is_trivial<ReactionContext>::value,
    "bounded POD reaction context");
void __stdcall BeforeReaction(ReactionContext* ctx, UnitAny* defender, void* game,
    UnitAny* attacker, const BYTE* damage) {
    ctx->admitted = StartAdmission();
    ctx->record = ctx->admitted && damage && defender &&
        game == reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) &&
        Tracked(Identify(defender)) && Covered();
    if (!ctx->record) return;
    ctx->frame = Frame(); ctx->source = Identify(attacker);
    ctx->target = Identify(defender);
    ctx->flags = *reinterpret_cast<const WORD*>(damage + 4);
}
void __stdcall AfterReaction(ReactionContext* ctx) {
    FinishAdmission finish{ctx->admitted};
    if (!ctx->record) return;
    Event e{};
    e.kind = HitResult; e.frame = ctx->frame;
    e.source = ctx->source; e.target = ctx->target; e.flags = ctx->flags;
    Publish(e);
}
// EAX=defender, arguments=(game,attacker,damage), native ret12.
// Preserve all registers and EFLAGS across both observers and the native call.
__declspec(naked) void ReactionHook() {
    __asm {
        lea esp, [esp - 48]
        pushfd
        pushad
        lea ecx, [esp + 36]
        mov edx, [esp + 96]
        push edx
        mov edx, [esp + 96]
        push edx
        mov edx, [esp + 96]
        push edx
        push dword ptr [esp + 40]
        push ecx
        call BeforeReaction
        popad
        popfd
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        call dword ptr [originalReaction]
        pushfd
        pushad
        lea edx, [esp + 36]
        push edx
        call AfterReaction
        popad
        popfd
        lea esp, [esp + 48]
        ret 12
    }
}
void __fastcall KillHook(void* game, UnitAny* defender, UnitAny* attacker, int petKill) {
    HookAdmission admission;
    bool record = admission.admitted &&
        game == reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) &&
        defender && Tracked(Identify(defender)) && Covered();
    auto listener = runListener.load(std::memory_order_acquire);
    DWORD beforeRun = listener && defender && defender->dwType == UNIT_MONSTER ?
        defender->dwMode : 0;
    Key target = record ? Identify(defender) : Missing();
    Key source = record ? Identify(attacker) : Missing();
    DWORD before = record ? defender->dwMode : 0;
    originalKill(game, defender, attacker, petKill);
    if (listener && defender && defender->dwType == UNIT_MONSTER &&
        beforeRun != defender->dwMode &&
        (defender->dwMode == NPC_MODE_DEATH || defender->dwMode == NPC_MODE_DEAD))
        listener(game, defender);
    if (!record) return;
    DWORD after = defender->dwMode;
    if (after == before || (target.type == 1 && after != 0 && after != 12) ||
        (target.type == 0 && after != 0 && after != 17)) return;
    Event e{}; e.kind = Death; e.frame = Frame(); e.source = source; e.target = target;
    e.beforeMode = before; e.afterMode = after; Publish(e);
}
struct ColdContext {
    UnitAny* defender = nullptr; Key source{}, target{};
    DWORD beforeExpiry = 0, beforeOwnerType = 0xffffffff, beforeOwnerId = 0xffffffff;
    int before67 = 0, before68 = 0, before69 = 0;
    bool before = false, record = false, admitted = false;
};
void __stdcall BeforeCold(ColdContext* ctx, UnitAny* attacker, UnitAny* defender) {
    ctx->admitted = StartAdmission();
    ctx->record = ctx->admitted && attacker && defender &&
        *reinterpret_cast<void* const*>(reinterpret_cast<const BYTE*>(attacker) + 0x80) ==
            reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) &&
        Tracked(Identify(defender)) && Covered();
    if (!ctx->record) return;
    ctx->defender = defender; ctx->source = Identify(attacker); ctx->target = Identify(defender);
    ctx->before = checkState(defender, 11) != 0;
    ctx->beforeExpiry = 0;
    ctx->beforeOwnerType = ctx->beforeOwnerId = 0xffffffff;
    ctx->before67 = ctx->before68 = ctx->before69 = 0;
    void* list = getStateList(defender, 11);
    if (list) {
        ctx->beforeExpiry = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(list) + 0x18);
        ctx->beforeOwnerType = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(list) + 0x08);
        ctx->beforeOwnerId = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(list) + 0x0c);
        ctx->before67 = getListStat(list, 67, 0);
        ctx->before68 = getListStat(list, 68, 0);
        ctx->before69 = getListStat(list, 69, 0);
    }
}
void __stdcall AfterCold(ColdContext* ctx) {
    FinishAdmission finish{ctx->admitted};
    if (!ctx->record || !checkState(ctx->defender, 11)) return;
    void* list = getStateList(ctx->defender, 11);
    if (!list) return;
    DWORD expiry = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(list) + 0x18);
    DWORD ownerType = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(list) + 0x08);
    DWORD ownerId = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(list) + 0x0c);
    if (ownerType != ctx->source.type || ownerId != ctx->source.id) ctx->source = {ownerType, ownerId, 0xffffffff};
    int slow67 = getListStat(list, 67, 0), slow68 = getListStat(list, 68, 0), slow69 = getListStat(list, 69, 0);
    if (ctx->before && ctx->beforeExpiry == expiry && ctx->before67 == slow67 &&
        ctx->before68 == slow68 && ctx->before69 == slow69 &&
        ctx->beforeOwnerType == ownerType && ctx->beforeOwnerId == ownerId) return;
    Event e{}; e.kind = Cold; e.frame = Frame(); e.source = ctx->source; e.target = ctx->target;
    e.transition = ctx->before ? 2 : 1; e.expiry = expiry;
    e.slow67 = slow67; e.slow68 = slow68; e.slow69 = slow69;
    bool inserted = false;
    for (auto& state : effects) if (state.valid && Equal(state.target, ctx->target)) {
        state = {ctx->target, ctx->source, expiry, slow67, slow68, slow69, true};
        inserted = true; break;
    }
    if (!inserted) for (auto& state : effects) if (!state.valid) {
        state = {ctx->target, ctx->source, expiry, slow67, slow68, slow69, true};
        inserted = true; break;
    }
    if (!inserted) effectOverflow.fetch_add(1, std::memory_order_relaxed);
    Publish(e);
}
// EAX=attacker, ESI=defender, stack duration; native ret4.
__declspec(naked) void ColdHook() {
    __asm {
        lea esp, [esp - 64]
        pushfd
        pushad
        lea edx, [esp + 36]
        push esi
        push eax
        push edx
        call BeforeCold
        popad
        popfd
        push dword ptr [esp + 68]
        call dword ptr [originalCold]
        pushfd
        pushad
        lea edx, [esp + 36]
        push edx
        call AfterCold
        popad
        popfd
        lea esp, [esp + 64]
        ret 4
    }
}
void __fastcall RemoveHook(UnitAny* unit, int state, void* list) {
    HookAdmission admission;
    bool record = admission.admitted && unit && state == 11 &&
        *reinterpret_cast<void* const*>(reinterpret_cast<const BYTE*>(unit) + 0x80) ==
            reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) &&
        Tracked(Identify(unit)) && Covered();
    Key target = record ? Identify(unit) : Missing();
    bool before = record && checkState(unit, 11) != 0;
    Event e{};
    if (before && list) {
        auto* bytes = static_cast<BYTE*>(list);
        e.source = {*reinterpret_cast<DWORD*>(bytes + 0x08),
            *reinterpret_cast<DWORD*>(bytes + 0x0c), 0xffffffff};
        e.expiry = *reinterpret_cast<DWORD*>(bytes + 0x18);
        e.slow67 = getListStat(list, 67, 0);
        e.slow68 = getListStat(list, 68, 0);
        e.slow69 = getListStat(list, 69, 0);
    }
    originalRemove(unit, state, list);
    if (!before || checkState(unit, 11)) return;
    for (auto& previous : effects) if (previous.valid && Equal(previous.target, target)) {
        e.source = previous.source;
        previous.valid = false; break;
    }
    e.kind = Cold; e.frame = Frame(); e.target = target; e.transition = 3;
    Publish(e);
}
UnitAny* __fastcall CreateHook(void* game, void* params) {
    HookAdmission admission;
    UnitAny* missile = originalCreate(game, params);
    if (!admission.admitted ||
        game != reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) ||
        !params || !missile || missile->dwType != 3) return missile;
    Key unit = Identify(missile);
    auto* fields = static_cast<const BYTE*>(params);
    UnitAny* owner = *reinterpret_cast<UnitAny* const*>(fields + 4);
    Key source = owner ? Identify(owner) : Missing();
    if (!Tracked(source) || !Covered()) return missile;
    if (!missile->pMonsterData) {
        missileOverflow.fetch_add(1, std::memory_order_relaxed);
        return missile;
    }
    bool inserted = false;
    for (MissileEntry& tracked : missiles) if (tracked.missile.type == 0xffffffff) {
        tracked = {unit, source}; inserted = true; break;
    }
    if (!inserted) { missileOverflow.fetch_add(1, std::memory_order_relaxed); return missile; }
    Event e{}; e.kind = ProjectileCreate; e.frame = Frame(); e.source = source; e.target = unit;
    const BYTE* data = reinterpret_cast<const BYTE*>(missile->pMonsterData);
    e.skill = *reinterpret_cast<const int16_t*>(data + 0x0a);
    e.skillLevel = *reinterpret_cast<const int16_t*>(data + 0x0c);
    e.totalFrames = *reinterpret_cast<const int16_t*>(data + 0x0e);
    e.currentFrame = *reinterpret_cast<const int16_t*>(data + 0x10);
    if (missile->pPath) { e.x = missile->pPath->xPos; e.y = missile->pPath->yPos; }
    Publish(e); return missile;
}
void __stdcall FreeHook(UnitAny* unit) {
    HookAdmission admission;
    bool record = false;
    Event e{};
    // Common's destructor also frees client mirrors: those are outside this
    // server-only stream, not evidence of a missed server-thread event.
    if (admission.admitted && unit && unit->dwType == 3 &&
        *reinterpret_cast<void* const*>(reinterpret_cast<const BYTE*>(unit) + 0x80) ==
            reinterpret_cast<void*>(serverGame.load(std::memory_order_relaxed)) && Covered()) {
        Key key = Identify(unit);
        for (MissileEntry& tracked : missiles) if (Equal(tracked.missile, key)) {
            e.source = tracked.owner;
            tracked = {Missing(), Missing()};
            record = true; e.kind = ProjectileDestroy; e.frame = Frame(); e.target = key;
            if (unit->pPath) { e.x = unit->pPath->xPos; e.y = unit->pPath->yPos; }
            break;
        }
    }
    originalFree(unit);
    if (record) Publish(e);
}

bool Bytes(HMODULE module, DWORD rva, const BYTE* prefix, size_t count,
           std::string* mismatch = nullptr) {
    if (!module) return false;
    auto* address = reinterpret_cast<BYTE*>(module) + rva;
    MEMORY_BASIC_INFORMATION memory = {};
    bool readable = VirtualQuery(address, &memory, sizeof(memory)) && memory.AllocationBase == module &&
        memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
        reinterpret_cast<uintptr_t>(address) + count <=
            reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    if (readable && !memcmp(address, prefix, count)) return true;
    if (mismatch) {
        char header[80];
        sprintf_s(header, sizeof(header), "trace ABI RVA 0x%lx %s; actual=", rva,
                  readable ? "bytes changed" : "unreadable executable span");
        mismatch->assign(header);
        if (readable) {
            static const char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < count && i < 64; ++i) {
                mismatch->push_back(hex[address[i] >> 4]);
                mismatch->push_back(hex[address[i] & 15]);
            }
        }
    }
    return false;
}
bool Bind(std::string& error) {
    if (!LabProfile::VerifyServer(error) || !LabProfile::VerifyCommon(error)) return false;
    HMODULE game = GetModuleHandleA("D2Game.dll"), common = GetModuleHandleA("D2Common.dll");
    HMODULE mod = GetModuleHandleA("ProjectDiablo.dll");
    std::string modDigest;
    if (!mod || !LabProfile::ModFileIdentity(modDigest)) {
        error = "pinned S13 melee-range module unavailable"; return false;
    }
    static const BYTE damage[] = {0x51,0x53,0x8b,0x5c,0x24,0x0c,0x55,0x8b,0x6c,0x24,0x1c,0x56,0x57,0x8b,0x7c,0x24,0x1c};
    static const BYTE kill[] = {0x83,0xec,0x24,0x53,0x56,0x8b,0xda,0x85,0xdb,0x8b,0xf1};
    static const BYTE reaction[] = {0x83,0xec,0x20,0x53,0x8b,0x5c,0x24,0x2c,0x55,0x8b,0x6c,0x24,0x34,0x56,0x8b,0x74,0x24,0x30};
    static const BYTE roll[] = {0x81,0xec,0x14,0x04,0x00,0x00,0x53,0x55,0x8b,0xac,0x24,0x20,0x04,0x00,0x00,0x85,0xed,0x8b,0xd8};
    static const BYTE resultEntry[] = {0x53,0x33,0xdb,0x85,0xff,0x56,0x8b,0xf0,0x0f,0x84,0x0d,0x01,0x00,0x00};
    static const BYTE resultEligibility[] = {0x8b,0x44,0x24,0x0c,0x50,0x8b,0xc6,0x8b,0xcf,0xe8,0x7e,0x24,0x00,0x00,0x85,0xc0};
    // S13's pinned initializer at mod RVA FE81A replaces the range-call operand
    // at game DE625 with mod+270FF0. Verify that complete helper and the entire
    // caller span with its relocation-derived operand, not vanilla call bytes.
    static const BYTE modRange[] = {0x55,0x8b,0xec,0x56,0x57,0x8b,0x7d,0x08,0xba,0x01,0x00,0x00,0x00,0x8b,0xcf,0xe8,0x7c,0xa1,0x05,0x00,0x8b,0x75,0x10,0x85,0xc0,0x74,0x25,0x8b,0x00,0x83,0xf8,0x01,0x75,0x03,0x46,0xeb,0x1b,0x85,0xc0,0x75,0x17,0x57,0xe8,0x01,0x33,0x00,0x00,0xff,0x75,0x0c,0x99,0x2b,0xc2,0xd1,0xf8,0x03,0xf0,0xe8,0x92,0x68,0x00,0x00,0x03,0xf0,0x56,0xff,0x75,0x0c,0x57,0xe8,0xb6,0x32,0x00,0x00,0xf7,0xd8,0x5f,0x1b,0xc0,0xf7,0xd8,0x5e,0x5d,0xc2,0x0c,0x00};
    BYTE eligibleRollCall[] = {0x55,0x56,0x57,0xe8,0,0,0,0,0x85,0xc0,0x5d,0x0f,0x84,0x89,0x00,0x00,0x00,0xf6,0xc3,0x01,0x75,0x1b,0x8b,0x54,0x24,0x10,0x6a,0x00,0x52,0x56,0x8b,0xc7,0xe8,0x4a,0xf8,0xff,0xff,0x85,0xc0,0x74,0x03,0x83,0xcb,0x01};
    DWORD rangeDisplacement = reinterpret_cast<uintptr_t>(mod) + 0x270ff0 -
        (reinterpret_cast<uintptr_t>(game) + 0xde629);
    memcpy(eligibleRollCall + 4, &rangeDisplacement, sizeof(rangeDisplacement));
    static const BYTE cold[] = {0x83,0xec,0x08,0x53,0x8b,0x5c,0x24,0x10,0x85,0xdb,0x57,0x8b,0xf8};
    static const BYTE remove[] = {0x51,0x56,0x8b,0xf1,0x85,0xf6,0x57,0x8b,0xfa};
    static const BYTE create[] = {0x83,0xec,0x28,0x55,0x8b,0xea,0x89,0x4c,0x24,0x20,0x8b,0x4d,0x04,0x85,0xc9};
    static const BYTE free[] = {0x56,0x8b,0x74,0x24,0x08,0x85,0xf6,0x74,0x24,0x83,0x3e,0x03,0x75,0x1f,0x8b,0x56};
    static const BYTE getter[] = {0x8b,0x44,0x24,0x04,0x85,0xc0,0x75,0x03,0xc2,0x0c,0x00,0x53,0x8b,0x58,0x5c,0x85};
    static const BYTE list[] = {0x8b,0x44,0x24,0x04,0x8b,0x48,0x5c,0x85,0xc9,0x74,0x07,0x8b,0x41,0x10,0x85,0xc0};
    static const BYTE check[] = {0x8b,0x44,0x24,0x04,0x85,0xc0,0x74,0x2e,0x8b,0x08,0x85,0xc9,0x7c,0x28,0x83,0xf9};
    static const BYTE statList[] = {0x53,0x8b,0x5c,0x24,0x08,0x85,0xdb,0x75,0x06,0x33,0xc0,0x5b,0xc2,0x0c,0x00,0x57};
    static const BYTE thunk[] = {0xff, 0x25};
    if (!Bytes(mod,0x270ff0,modRange,sizeof(modRange),&error) ||
        !Bytes(game,0xdde90,roll,sizeof(roll),&error) ||
        !Bytes(game,0xde5a0,resultEntry,sizeof(resultEntry),&error) ||
        !Bytes(game,0xde5d4,resultEligibility,sizeof(resultEligibility),&error) ||
        !Bytes(game,0xde621,eligibleRollCall,sizeof(eligibleRollCall),&error)) return false;
    if (!Bytes(game,0xde0c0,damage,sizeof(damage)) || !Bytes(game,0xdcf20,reaction,sizeof(reaction)) ||
        !Bytes(game,0xdeee0,kill,sizeof(kill)) ||
        !Bytes(game,0xdc780,cold,sizeof(cold)) || !Bytes(game,0xdb4c0,remove,sizeof(remove)) ||
        !Bytes(game,0x6f930,create,sizeof(create)) || !Bytes(common,0x6a350,free,sizeof(free)) ||
        !Bytes(common,0x38b70,getter,sizeof(getter)) || !Bytes(common,0x38520,list,sizeof(list)) ||
        !Bytes(common,0x33cd0,check,sizeof(check)) || !Bytes(common,0x39140,statList,sizeof(statList)) ||
        !Bytes(game,0xa42a,thunk,sizeof(thunk)) ||
        *reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(game)+0xa42c) !=
            reinterpret_cast<DWORD>(reinterpret_cast<BYTE*>(game)+0xf82b0) ||
        *reinterpret_cast<void* volatile*>(reinterpret_cast<BYTE*>(game)+0xf82b0) !=
            reinterpret_cast<void*>(reinterpret_cast<BYTE*>(common)+0x38b70) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10973)) != reinterpret_cast<FARPROC>(reinterpret_cast<BYTE*>(common)+0x38b70) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10871)) != reinterpret_cast<FARPROC>(reinterpret_cast<BYTE*>(common)+0x38520) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10494)) != reinterpret_cast<FARPROC>(reinterpret_cast<BYTE*>(common)+0x33cd0) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10680)) != reinterpret_cast<FARPROC>(reinterpret_cast<BYTE*>(common)+0x39140) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10178)) != reinterpret_cast<FARPROC>(reinterpret_cast<BYTE*>(common)+0x6a350)) {
        error = "trace ABI bytes or common exports changed"; return false;
    }
    originalDamage = reinterpret_cast<DamageNative>(reinterpret_cast<BYTE*>(game)+0xde0c0);
    originalReaction = reinterpret_cast<ReactionNative>(reinterpret_cast<BYTE*>(game)+0xdcf20);
    originalRoll = reinterpret_cast<RollNative>(reinterpret_cast<BYTE*>(game)+0xdde90);
    originalKill = reinterpret_cast<KillNative>(reinterpret_cast<BYTE*>(game)+0xdeee0);
    originalCold = reinterpret_cast<ColdNative>(reinterpret_cast<BYTE*>(game)+0xdc780);
    originalRemove = reinterpret_cast<RemoveNative>(reinterpret_cast<BYTE*>(game)+0xdb4c0);
    originalCreate = reinterpret_cast<CreateNative>(reinterpret_cast<BYTE*>(game)+0x6f930);
    originalFree = reinterpret_cast<FreeNative>(reinterpret_cast<BYTE*>(common)+0x6a350);
    getStat = reinterpret_cast<Getter>(reinterpret_cast<BYTE*>(common)+0x38b70);
    getStateList = reinterpret_cast<StateList>(reinterpret_cast<BYTE*>(common)+0x38520);
    getListStat = reinterpret_cast<ListStat>(reinterpret_cast<BYTE*>(common)+0x39140);
    checkState = reinterpret_cast<CheckState>(reinterpret_cast<BYTE*>(common)+0x33cd0);
    gameImage = reinterpret_cast<BYTE*>(game);
    getterSlot = reinterpret_cast<void* volatile*>(gameImage + 0xf82b0);
    return true;
}
bool PatchGetter(bool attach) {
    if (!getterSlot) return false;
    void* original = reinterpret_cast<void*>(getStat);
    void* hook = reinterpret_cast<void*>(GetterHook);
    void* expected = attach ? original : hook;
    void* replacement = attach ? hook : original;
    if (*getterSlot == replacement) return true;
    if (*getterSlot != expected) return false;
    DWORD protection = 0;
    if (!VirtualProtect(const_cast<void**>(getterSlot), sizeof(void*), PAGE_READWRITE, &protection))
        return false;
    void* prior = InterlockedCompareExchangePointer(getterSlot, replacement, expected);
    DWORD ignored = 0;
    bool protectedAgain = VirtualProtect(const_cast<void**>(getterSlot), sizeof(void*), protection, &ignored) != 0;
    return prior == expected && protectedAgain;
}

bool Transaction(bool attach, bool killOnly = false) {
    if (DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) { DetourTransactionAbort(); return false; }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { DetourTransactionAbort(); return false; }
    // Detours owns suspended thread contexts until commit/abort; their handles
    // must remain open throughout the complete transaction.
    std::array<HANDLE, 1024> threads{};
    size_t threadCount = 0;
    auto closeThreads = [&]() {
        for (size_t i = 0; i < threadCount; ++i) CloseHandle(threads[i]);
    };
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    bool ok = Thread32First(snapshot, &entry) != 0;
    const DWORD process = GetCurrentProcessId(), current = GetCurrentThreadId();
    do {
        if (!ok) break;
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != current) {
            if (threadCount == threads.size()) { ok = false; break; }
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) { ok = false; break; }
            LONG result = DetourUpdateThread(thread);
            if (result != NO_ERROR) { CloseHandle(thread); ok = false; break; }
            threads[threadCount++] = thread;
        }
    } while (Thread32Next(snapshot, &entry));
    DWORD endError = GetLastError();
    CloseHandle(snapshot);
    if (!ok || endError != ERROR_NO_MORE_FILES) {
        DetourTransactionAbort();
        closeThreads();
        return false;
    }
    struct Pair { void** original; void* hook; };
    Pair hooks[] = {
        {reinterpret_cast<void**>(&originalDamage), reinterpret_cast<void*>(DamageHook)},
        {reinterpret_cast<void**>(&originalReaction), reinterpret_cast<void*>(ReactionHook)},
        {reinterpret_cast<void**>(&originalRoll), reinterpret_cast<void*>(RollHook)},
        {reinterpret_cast<void**>(&originalKill), reinterpret_cast<void*>(KillHook)},
        {reinterpret_cast<void**>(&originalCold), reinterpret_cast<void*>(ColdHook)},
        {reinterpret_cast<void**>(&originalRemove), reinterpret_cast<void*>(RemoveHook)},
        {reinterpret_cast<void**>(&originalCreate), reinterpret_cast<void*>(CreateHook)},
        {reinterpret_cast<void**>(&originalFree), reinterpret_cast<void*>(FreeHook)}
    };
    for (auto& hook : hooks) if ((!killOnly ||
        hook.original == reinterpret_cast<void**>(&originalKill) ||
        hook.original == reinterpret_cast<void**>(&originalDamage)) &&
        (attach ? DetourAttach(hook.original, hook.hook) :
        DetourDetach(hook.original, hook.hook)) != NO_ERROR) {
        DetourTransactionAbort();
        closeThreads();
        return false;
    }
    LONG commit = DetourTransactionCommit();
    closeThreads();
    return commit == NO_ERROR;
}
json Failure(const char* reason) { return {{"ok",false},{"error",reason}}; }
json Identity(const Key& key) {
    return key.type == 0xffffffff ? json(nullptr) :
        json{{"type",key.type},{"id",key.id},
            {"class_id",key.cls == 0xffffffff ? json(nullptr) : json(key.cls)}};
}
json Encode(const Event& e) {
    json row = {{"sequence",e.sequence},{"frame",e.frame},{"source",Identity(e.source)},
        {"target",Identity(e.target)},
        {"coverage",{{"source_class",e.source.cls != 0xffffffff}}}};
    switch (e.kind) {
    case Damage:
        row["type"] = "native_damage"; row["stage"] = "resolved_hp_debit";
        row["debit_entered"] = e.transition != 0;
        row["gross_damage_raw"] = e.gross;
        row["hp_before_debit_raw"] = e.transition ? json(e.debitHp) : json(nullptr);
        row["overkill_raw"] = e.transition ?
            json(e.gross > e.debitHp ? int64_t(e.gross) - e.debitHp : 0) : json(0);
        row["hp_before_raw"] = e.hpBefore; row["hp_after_raw"] = e.hpAfter;
        row["result_flags"] = e.flags; row["units"] = "hp_raw_8_fraction";
        row["components_stage"] = "native_post_total_not_additive";
        row["components_raw"] = {{"physical",e.phys},{"fire",e.fire},{"lightning",e.lightning},
            {"magic",e.magic},{"cold",e.cold},{"poison",e.poison}}; break;
    case HitRoll:
        row["type"] = "native_hit_roll";
        row["stage"] = "eligible_attack_roll_return";
        row["eligible"] = true;
        row["roll_succeeded"] = e.flags != 0;
        row["result"] = e.flags ? "native_attack_roll_hit" : "native_attack_roll_miss";
        break;
    case HitResult: {
        row["type"] = "native_hit_result";
        row["stage"] = "reaction_entry";
        row["result_flags"] = e.flags;
        row["successful_hit_flag"] = (e.flags & 0x0001) != 0;
        row["block_flag"] = (e.flags & 0x0010) != 0;
        row["weapon_block_flag"] = (e.flags & 0x8000) != 0;
        // A zero result is not proof of a roll miss: range and validity checks
        // can also return zero. Keep those cases explicitly unresolved.
        row["miss_flag"] = nullptr;
        const bool blocked = (e.flags & 0x8010) != 0;
        row["outcome"] = (e.flags & 1) && blocked ? "native_overlapping_result_flags" :
            (e.flags & 1) ? "native_successful_hit_flag" :
            (e.flags & 0x0010) ? "native_block_flag" :
            (e.flags & 0x8000) ? "native_weapon_block_flag" :
            "native_no_successful_hit_flag";
        row["gross_damage_raw"] = nullptr;
        row["hp_debit_observed"] = nullptr;
        break;
    }
    case Death:
        row["type"] = "native_death"; row["mode_before"] = e.beforeMode;
        row["mode_after"] = e.afterMode; row["mode_transition_confirmed"] = true; break;
    case Cold:
        row["type"] = "native_effect"; row["state_id"] = 11;
        row["transition"] = e.transition == 1 ? "applied" : e.transition == 2 ? "refreshed" : "removed";
        row["expiry_frame"] = e.expiry;
        row["slows_raw"] = {{"67",e.slow67},{"68",e.slow68},{"69",e.slow69}};
        row["skill_id"] = nullptr; break;
    case ProjectileCreate:
        row["type"] = "native_projectile_created"; row["skill_id"] = e.skill;
        row["skill_level"] = e.skillLevel; row["position"] = {{"x",e.x},{"y",e.y}};
        row["total_frames"] = e.totalFrames; row["current_frame"] = e.currentFrame; break;
    case ProjectileDestroy:
        row["type"] = "native_projectile_destroyed";
        row["removal_reason"] = "unknown"; row["position"] = {{"x",e.x},{"y",e.y}}; break;
    default: break;
    }
    return row;
}
json Coverage() {
    uint64_t end = nextSequence.load(std::memory_order_acquire);
    uint64_t drops = lost.load(std::memory_order_relaxed);
    uint64_t threads = wrongThread.load(std::memory_order_relaxed);
    uint64_t overflow = missileOverflow.load(std::memory_order_relaxed);
    uint64_t effectsLost = effectOverflow.load(std::memory_order_relaxed);
    uint64_t readerLoss = readGaps.load(std::memory_order_relaxed);
    uint64_t unfinished = interrupted.load(std::memory_order_relaxed);
    bool stable = finalized.load(std::memory_order_acquire);
    return {{"stream","ExecuteEvents_applications+native_reaction_entries+native_eligible_melee_rolls"},
        {"application_stream","ExecuteEvents_applications"},{"reaction_stream","DamageReaction_entry"},
        {"roll_stream","GetResultFlags_eligible_melee_roll"},
        {"complete",stable && restored.load(std::memory_order_relaxed) &&
            !unfinished && !drops && !threads && !overflow && !effectsLost && !readerLoss && end <= kRing},
        {"finalized",stable},{"interrupted",unfinished},{"in_flight",inFlight.load(std::memory_order_acquire)},
        {"ring_overwritten",end > kRing ? end-kRing : 0}, {"slot_drops",drops},
        {"thread_loss",threads},{"projectile_tracking_overflow",overflow},
        {"effect_tracking_overflow",effectsLost},{"read_gaps",readerLoss},
        {"hooks_restored",restored.load(std::memory_order_relaxed)}};
}
void StopLocked() {
    capturing.store(false, std::memory_order_release);
    // A stop invoked recursively on the server producer cannot wait for itself
    // or detach a trampoline still on its own call stack. Wake the control
    // watchdog and return an explicitly unfinished receipt instead.
    if (GetCurrentThreadId() == serverThread.load(std::memory_order_relaxed) &&
        inFlight.load(std::memory_order_acquire)) {
        publicationOpen.store(false, std::memory_order_release);
        finalized.store(false, std::memory_order_release);
        interrupted.fetch_add(1, std::memory_order_relaxed);
        restored.store(false, std::memory_order_release);
        if (stopWatchdog) SetEvent(stopWatchdog);
        return;
    }
    DWORD until = GetTickCount() + 2000;
    while (inFlight.load(std::memory_order_acquire) && static_cast<int32_t>(GetTickCount()-until) < 0)
        Sleep(1);
    bool drained = inFlight.load(std::memory_order_acquire) == 0;
    publicationOpen.store(false, std::memory_order_release);
    finalized.store(drained, std::memory_order_release);
    if (!drained) interrupted.fetch_add(1, std::memory_order_relaxed);
    bool getterRestored = !getterSlot || PatchGetter(false);
    bool detoursRestored = true;
    if (attached) {
        detoursRestored = Transaction(false);
        if (detoursRestored) attached = false;
    }
    restored.store(getterRestored && detoursRestored, std::memory_order_release);
    if (stopWatchdog) SetEvent(stopWatchdog);
}
DWORD WINAPI Watchdog(LPVOID) {
    DWORD remaining = deadline - GetTickCount();
    if (remaining > kMinute) remaining = 0;
    DWORD signal = WaitForSingleObject(stopWatchdog, remaining);
    if (signal != WAIT_OBJECT_0 ||
        !finalized.load(std::memory_order_acquire) || !restored.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(control);
        StopLocked();
    }
    return 0;
}
void ReapWatchdog() {
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(control);
        thread = watchdog;
    }
    if (!thread) return;
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) {
        restored.store(false, std::memory_order_release);
        return;
    }
    std::lock_guard<std::mutex> lock(control);
    if (watchdog == thread) {
        CloseHandle(watchdog); watchdog = nullptr;
        CloseHandle(stopWatchdog); stopWatchdog = nullptr;
    }
}
}

namespace CombatTelemetry {
bool ListenForRoguelikeKills(KillListener listener, DamageListener damage,
    std::string& error) {
    if (!listener || !damage) { error = "run combat listeners missing"; return false; }
    std::lock_guard<std::mutex> lane(lifecycle);
    std::lock_guard<std::mutex> lock(control);
    if (attached || !restored.load() || !finalized.load() ||
        inFlight.load(std::memory_order_acquire)) {
        error = "combat trace owns the kill event"; return false;
    }
    if (runHookAttached) {
        if (runListener.load() != nullptr) {
            error = "another roguelike listener is active"; return false;
        }
        runListener.store(listener, std::memory_order_release);
        runDamageListener.store(damage, std::memory_order_release);
        return true;
    }
    if (!Bind(error) || !Transaction(true, true)) {
        if (error.empty()) error = "cannot attach isolated kill event";
        return false;
    }
    runHookAttached = true;
    runListener.store(listener, std::memory_order_release);
    runDamageListener.store(damage, std::memory_order_release);
    return true;
}
void StopRoguelikeKills() {
    runListener.store(nullptr, std::memory_order_release);
    runDamageListener.store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> lane(lifecycle);
    std::lock_guard<std::mutex> lock(control);
    if (runHookAttached && Transaction(false, true)) runHookAttached = false;
}
json Start(void* game, UnitAny* player, const json& args) {
    std::lock_guard<std::mutex> lane(lifecycle);
    {
        std::lock_guard<std::mutex> lock(control);
        if (attached || runHookAttached || !restored.load() || !finalized.load() ||
            inFlight.load(std::memory_order_acquire))
            return Failure("previous trace has not reached a stable restored boundary");
    }
    ReapWatchdog();
    if (!args.is_object() || !args.contains("request_id") || !args["request_id"].is_string() ||
        !args.contains("expected_epoch") || !args["expected_epoch"].is_string() ||
        !args.contains("unit_ids") || !args["unit_ids"].is_array() || args["unit_ids"].size() > 15)
        return Failure("trace requires request_id, expected_epoch and up to 15 unit_ids");
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "request_id" && it.key() != "expected_epoch" &&
            it.key() != "unit_ids" && it.key() != "duration")
            return Failure("unknown trace request field");
    unsigned duration = 60;
    if (args.contains("duration")) {
        if (!args["duration"].is_number_integer()) return Failure("duration must be 1..60 seconds");
        try { duration = args["duration"].get<unsigned>(); }
        catch (...) { return Failure("duration must be 1..60 seconds"); }
        if (duration < 1 || duration > 60) return Failure("duration must be 1..60 seconds");
    }
    std::lock_guard<std::mutex> lock(control);
    if (attached || runHookAttached || !restored.load() || !finalized.load() ||
        inFlight.load(std::memory_order_acquire))
        return Failure("previous trace has not reached a stable restored boundary");
    ServerMonsters::Request request;
    request.operation = ServerMonsters::Operation::Observe;
    request.expectedEpoch = args["expected_epoch"].get<std::string>();
    if (request.expectedEpoch.empty()) return Failure("expected_epoch required");
    for (const auto& id : args["unit_ids"]) {
        if (!id.is_number_integer()) return Failure("invalid tracked unit ID");
        uint64_t number = 0;
        try { number = id.get<uint64_t>(); } catch (...) { return Failure("invalid tracked unit ID"); }
        if (number > UINT32_MAX) return Failure("invalid tracked unit ID");
        for (DWORD prior : request.unitIds) if (prior == number) return Failure("duplicate tracked unit ID");
        request.unitIds.push_back(static_cast<DWORD>(number));
    }
    json snapshot = ObservationTelemetry::Capture(game, player, request);
    if (!snapshot.value("ok",false) || !snapshot.value("coherent",false) ||
        snapshot.value("session_epoch",std::string()) != request.expectedEpoch)
        return Failure("trace epoch or coherent server context unavailable");
    if (!snapshot["coverage"].value("runtime_data",false)) return Failure("trace live data identity unavailable");
    for (const auto& enemy : snapshot["enemies"])
        if (!enemy.contains("present") || !enemy["present"].is_boolean() || !enemy["present"].get<bool>())
            return Failure("tracked enemy absent or outside selected server scope");
    std::string bindingError;
    if (!Bind(bindingError)) return Failure(bindingError.c_str());
    std::string id = args["request_id"].get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        return Failure("request_id must be 32 lowercase hex characters");
    if (!player || player->dwType != 0) return Failure("server player unavailable");
    for (auto& actor : actors) actor = Missing();
    for (auto& missile : missiles) missile = {Missing(), Missing()};
    for (auto& effect : effects) effect = EffectState{};
    actors[0] = Identify(player);
    for (size_t i = 0; i < request.unitIds.size(); ++i) {
        const auto& enemy = snapshot["enemies"][i];
        actors[i+1] = {1,request.unitIds[i],enemy["class_id"].get<DWORD>()};
    }
    for (auto& slot : ring) { slot.busy.test_and_set(std::memory_order_acquire); slot.event = Event{}; slot.busy.clear(std::memory_order_release); }
    nextSequence.store(0); lost.store(0); wrongThread.store(0); missileOverflow.store(0);
    effectOverflow.store(0); readGaps.store(0); interrupted.store(0);
    finalized.store(true, std::memory_order_release);
    serverGame.store(reinterpret_cast<uintptr_t>(game)); serverThread.store(GetCurrentThreadId());
    epoch = request.expectedEpoch; traceId = id; deadline = GetTickCount() + duration * 1000;
    stopWatchdog = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!stopWatchdog) return Failure("trace watchdog unavailable");
    if (!Transaction(true)) { CloseHandle(stopWatchdog); stopWatchdog = nullptr; return Failure("trace hook transaction failed"); }
    attached = true; restored.store(false);
    if (!PatchGetter(true)) {
        StopLocked();
        CloseHandle(stopWatchdog); stopWatchdog = nullptr;
        return Failure("trace debit IAT hook could not be installed or restored");
    }
    watchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    if (!watchdog) {
        StopLocked(); CloseHandle(stopWatchdog); stopWatchdog = nullptr;
        return Failure("trace watchdog thread unavailable");
    }
    finalized.store(false, std::memory_order_release);
    publicationOpen.store(true, std::memory_order_release);
    capturing.store(true, std::memory_order_release);
    return {{"ok",true},{"status","started"},{"source","offline_server"},{"trace_id",traceId},
        {"session_epoch",epoch},{"max_seconds",duration},
        {"capacity",kRing},{"tracked_actors",request.unitIds.size()+1},{"coverage",Coverage()}};
}
json Read(const json& args) {
    if (!args.is_object()) return Failure("trace read arguments must be an object");
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "trace_id" && it.key() != "expected_epoch" &&
            it.key() != "after_sequence" && it.key() != "limit")
            return Failure("unknown trace read field");
    std::lock_guard<std::mutex> lock(control);
    if (traceId.empty() || (args.contains("trace_id") &&
        (!args["trace_id"].is_string() || args["trace_id"].get<std::string>() != traceId)) ||
        (args.contains("expected_epoch") &&
        (!args["expected_epoch"].is_string() || args["expected_epoch"].get<std::string>() != epoch)))
        return Failure("unknown trace_id or stale session epoch");
    uint64_t after = 0; unsigned limit = 128;
    if (args.contains("after_sequence")) {
        if (!args["after_sequence"].is_number_unsigned() && !args["after_sequence"].is_number_integer())
            return Failure("after_sequence must be nonnegative");
        try { after = args["after_sequence"].get<uint64_t>(); } catch (...) { return Failure("invalid after_sequence"); }
    }
    if (args.contains("limit")) {
        if (!args["limit"].is_number_unsigned() && !args["limit"].is_number_integer()) return Failure("limit must be 1..128");
        try { limit = args["limit"].get<unsigned>(); } catch (...) { return Failure("limit must be 1..128"); }
        if (limit < 1 || limit > 128) return Failure("limit must be 1..128");
    }
    uint64_t end = nextSequence.load(std::memory_order_acquire);
    uint64_t oldest = end > kRing ? end-kRing+1 : 1;
    uint64_t cursor = after == UINT64_MAX ? UINT64_MAX : after + 1;
    json gaps = json::array(), events = json::array();
    if (cursor < oldest) { gaps.push_back({{"from",cursor},{"through",oldest-1},{"reason","ring_overwritten"}}); cursor = oldest; }
    unsigned scanned = 0;
    while (cursor <= end && scanned++ < limit) {
        Slot& slot = ring[(cursor-1)%kRing];
        if (slot.busy.test_and_set(std::memory_order_acquire)) {
            readGaps.fetch_add(1, std::memory_order_relaxed);
            gaps.push_back({{"from",cursor},{"through",cursor},{"reason","slot_busy"}}); ++cursor; continue;
        }
        Event copy = slot.event;
        slot.busy.clear(std::memory_order_release);
        if (copy.sequence != cursor) {
            readGaps.fetch_add(1, std::memory_order_relaxed);
            gaps.push_back({{"from",cursor},{"through",cursor},{"reason","slot_lost_or_overwritten"}});
        } else events.push_back(Encode(copy));
        ++cursor;
    }
    json coverage = Coverage();
    if (!gaps.empty()) coverage["complete"] = false;
    return {{"ok",true},{"trace_id",traceId},{"session_epoch",epoch},{"events",std::move(events)},
        {"gaps",std::move(gaps)},{"next_sequence",cursor},{"latest_sequence",end},{"coverage",coverage}};
}
json Stop(const json& args) {
    if (!args.is_object()) return Failure("trace stop arguments must be an object");
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "trace_id" && it.key() != "expected_epoch")
            return Failure("unknown trace stop field");
    std::lock_guard<std::mutex> lane(lifecycle);
    {
        std::lock_guard<std::mutex> lock(control);
        if (traceId.empty() || (args.contains("trace_id") &&
            (!args["trace_id"].is_string() || args["trace_id"].get<std::string>() != traceId)) ||
            (args.contains("expected_epoch") &&
            (!args["expected_epoch"].is_string() || args["expected_epoch"].get<std::string>() != epoch)))
            return Failure("unknown trace_id or stale session epoch");
        StopLocked();
    }
    if (!(GetCurrentThreadId() == serverThread.load(std::memory_order_relaxed) &&
        inFlight.load(std::memory_order_acquire))) ReapWatchdog();
    std::lock_guard<std::mutex> lock(control);
    return {{"ok",restored.load() && finalized.load()},{"trace_id",traceId},
        {"session_epoch",epoch},{"coverage",Coverage()}};
}
void Shutdown() {
    StopRoguelikeKills();
    std::lock_guard<std::mutex> lane(lifecycle);
    {
        std::lock_guard<std::mutex> lock(control);
        StopLocked();
    }
    if (!(GetCurrentThreadId() == serverThread.load(std::memory_order_relaxed) &&
        inFlight.load(std::memory_order_acquire))) ReapWatchdog();
}
} // namespace CombatTelemetry
