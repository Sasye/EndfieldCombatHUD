#pragma once
#include <applepie_client.h>
#include <atomic>

// Module lifetime only. Attach configures these before publishing any worker.
static applepie::Client g_client;
static APH_RuntimeApi g_runtime{};
static std::atomic<bool> g_stopping{false};
static std::atomic<bool> g_pluginActive{false};
static std::atomic<DWORD> g_gameThread{0};
static std::atomic<uintptr_t> g_activeHpBar{0}; // comparison identity, never dereferenced
static HANDLE g_cancel = nullptr;
static HANDLE g_worker = nullptr;
static SRWLOCK g_workerLock = SRWLOCK_INIT;
static uint64_t g_epoch = 0;
static ULONGLONG g_enableDeadline = 0;

static bool RuntimeOpen() {
    if (g_stopping.load()) return false;
    if (g_runtime.probe_open) return g_runtime.probe_open(g_client.owner()) == APH_OK;
    APH_RuntimeSnapshot snapshot{};
    return g_runtime.snapshot && g_runtime.snapshot(sizeof(snapshot), &snapshot) == APH_OK &&
        snapshot.state < APH_RUNTIME_STOPPING;
}
static bool GameReadAllowed() {
    // These are metadata-validated UI native callbacks on the Unity window
    // thread, borrowing their self/arguments. No current actor or write lease.
    return g_pluginActive.load() && GetCurrentThreadId() == g_gameThread.load() && RuntimeOpen();
}
static bool BeginHudRead(APH_NativeBinding*, APH_NativeAdmission*) { return GameReadAllowed(); }
static void EndHudRead(APH_NativeAdmission*) {}
static applepie::NativeOptions HudOptions() {
    applepie::NativeOptions o{};
    o.begin = BeginHudRead;
    o.end = EndHudRead;
    return o;
}
static bool EnableOpen(uint64_t epoch) {
    APH_OwnerStatus status{};
    return WaitForSingleObject(g_cancel, 0) == WAIT_TIMEOUT && RuntimeOpen() &&
        g_client.Status(status) == APH_OK && status.state == APH_OWNER_ENABLING &&
        status.activation_epoch == epoch;
}
template<class Advance> static APH_Result AwaitHud(uint64_t epoch, Advance advance) {
    for (;;) {
        if (!EnableOpen(epoch)) return APH_E_REVOKED;
        APH_Result result = advance();
        if (result != APH_PENDING && result != APH_E_BUSY) return result;
        if (GetTickCount64() >= g_enableDeadline) return APH_E_UNAVAILABLE;
        if (WaitForSingleObject(g_cancel, 5) != WAIT_TIMEOUT) return APH_E_REVOKED;
    }
}

// ObjectPtr<Buff> is a 16-byte value, passed indirectly by the MSVC x64 ABI.
// Runtime metadata also validates its concrete generic type and value size.
struct BuffRef { void* object; uint32_t uid; uint32_t padding; };
static_assert(sizeof(BuffRef) == 16, "ObjectPtr<Buff> ABI");
using BuffChange = void(*)(void*, BuffRef, bool, int32_t, void*);

// Preserve the original cheap self predicate BEFORE SDK business admission.
// The accurate adapter still accounts for every passthrough/original call.
template<unsigned Tag> struct FilteredBuffHook {
    using Adapter = APH_TypedNative<Tag, BuffChange>;
    inline static applepie::Hook hook{};
    static void Entry(void* self, BuffRef buff, bool added, int32_t reason, void* method) {
        const bool relevant = self && (uintptr_t)self == g_activeHpBar.load();
        Adapter::Run(relevant, _ReturnAddress(), self, buff, added, reason, method);
    }
    static APH_Result Request(const applepie::Client& client, uint64_t key, BuffChange target,
        BuffChange body, const char* signature, const char* name, const applepie::NativeOptions&) {
        APH_HookRequest request{};
        APH_Result r = applepie::MakeHookRequest(request, key, (APH_NativeProc)target,
            (APH_NativeProc)Entry, signature, name);
        if (r != APH_OK) return r;
        r = hook.Request(client, request);
        if (hook.handle() && !Adapter::binding.api) {
            auto& b = Adapter::binding;
            b.owner = client.owner(); b.hook = hook.handle(); b.api = hook.api();
            b.begin = BeginHudRead; b.end = EndHudRead;
            Adapter::business = body;
        }
        return r;
    }
    static APH_Result Advance() {
        APH_Result r = hook.Advance(&Adapter::binding.original);
        if (r == APH_OK) InterlockedExchange(&Adapter::binding.ready, 1);
        return r;
    }
    static void Original(void* self, BuffRef buff, bool added, int32_t reason, void* method) {
        Adapter::Original(self, buff, added, reason, method);
    }
};
using ProgressHook = FilteredBuffHook<1>;
using BuffIconHook = FilteredBuffHook<2>;
using EnhanceHook = applepie::NativeHook<3, void(*)(void*, BuffRef, void*)>;
using GpuiEnhanceHook = applepie::NativeHook<4, void(*)(void*, BuffRef, void*)>;
using AddStackHook = applepie::NativeHook<5, void(*)(void*, BuffRef, bool, void*, void*, void*)>;
using RemoveStackHook = applepie::NativeHook<6, void(*)(void*, BuffRef, int32_t, void*, void*, void*)>;
using ClearHook = applepie::NativeHook<7, void(*)(void*, void*)>;
using LateTickHook = applepie::NativeHook<8, void(*)(void*, float, void*)>;
using SkillTickHook = applepie::NativeHook<9, void(*)(void*, float, void*)>;
using ShowHook = applepie::NativeHook<10, void(*)(void*, void*)>;
using HideHook = applepie::NativeHook<11, void(*)(void*, void*)>;
using BattleHook = applepie::NativeHook<12, void(*)(void*, void*)>;
using ReleaseHook = applepie::NativeHook<13, void(*)(void*, void*)>;
using ComboTickHook = applepie::NativeHook<14, void(*)(void*, float, void*)>;

// Same v1, zero-payload, original-once contracts as EIEM. Stop observers are
// resident across disable; business capture is never a module observer.
static void BeginRuntimeStop() {
    g_stopping.store(true);
    g_pluginActive.store(false);
    if (g_cancel) SetEvent(g_cancel);
}
static APH_Result APH_CALL ObserveStop(APH_Owner, const APH_SharedEvent* e, void*) {
    if (e->phase == APH_BEFORE) BeginRuntimeStop();
    return APH_OK;
}
template<unsigned Tag, class... Args> struct StopHook {
    inline static applepie::SharedHook hook{};
    inline static applepie::Subscription observer{};
    inline static APH_NativeProc original = nullptr;
    static LONG Fault(EXCEPTION_POINTERS* e, unsigned phase) {
        hook.api()->native_fault(hook.owner(), hook.handle(), e->ExceptionRecord->ExceptionCode,
            (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, phase);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static void Publish(const APH_SharedCall* call, unsigned phase) {
        if (hook.api()->publish(call, phase, nullptr, 0) < 0)
            RaiseException(0xe0411001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    static void Entry(Args... args) {
        APH_SharedCall call{};
        unsigned phase = APH_ORIGINAL_NOT_ENTERED;
        const APH_Result entered = hook.api()->enter(hook.owner(), hook.handle(), &call);
        __try {
            __try {
                if (entered == APH_OK) Publish(&call, APH_BEFORE);
                BeginRuntimeStop();
                phase = APH_ORIGINAL_RUNNING;
                ((void(*)(Args...))original)(args...);
                phase = APH_ORIGINAL_RETURNED;
                if (entered == APH_OK) Publish(&call, APH_AFTER);
            } __except(Fault(GetExceptionInformation(), phase)) {}
        } __finally { if (entered == APH_OK) hook.api()->leave(&call); }
    }
    static APH_Result Install(uint64_t epoch, APH_NativeProc target,
        const char* signature, const char* contract) {
        APH_SharedRequest request{};
        APH_Result r = applepie::MakeSharedRequest(request, Tag, target, (APH_NativeProc)Entry,
            signature, contract, 1, 0);
        if (r != APH_OK) return r;
        r = hook.Request(g_client, request);
        if (r < 0) return r;
        if (!observer.handle()) {
            const APH_SharedListen desc{sizeof(desc), APH_BEFORE, APH_MODULE_OBSERVER,
                APH_RECOVERY_NONE, ObserveStop, nullptr};
            r = observer.Listen(hook, desc);
            if (r != APH_OK) return r;
        }
        return AwaitHud(epoch, [] { return hook.Advance(&original); });
    }
};
using ShutdownHook = StopHook<100>;
using QuitHook = StopHook<101, void*>;
