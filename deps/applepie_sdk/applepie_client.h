#pragma once
// Optional MSVC C++17 client helpers. The public C ABI remains in applepie.h.
// No game discovery, write permission, background thread or automatic unloading.
#pragma pack(push, 8)
#include "applepie_native.h"
#include "applepie_seh.h"
#include "applepie_pointer_scope.h"
#include <cstring>
#include <vector>

namespace applepie {
namespace detail {
template<class T> struct ServiceTraits;
#define APH_CLIENT_SERVICE(Type, Id, Minimum, Functions) \
    template<> struct ServiceTraits<Type> { \
        static constexpr uint64_t id = Id; \
        static bool valid(const Type& a, uint64_t) { \
            return a.size >= Minimum && a.version == 1 && (Functions); \
        } \
    }
APH_CLIENT_SERVICE(APH_SharedApi, APH_SERVICE_SHARED, sizeof(APH_SharedApi),
    a.request && a.query && a.activate && a.release && a.listen && a.unlisten &&
    a.enter && a.publish && a.leave && a.native_fault);
APH_CLIENT_SERVICE(APH_PointerApi, APH_SERVICE_POINTERS, sizeof(APH_PointerApi),
    a.request && a.release && a.query && a.wrapper_fault);
APH_CLIENT_SERVICE(APH_RuntimeApi, APH_SERVICE_RUNTIME, APH_RUNTIME_API_V1_PREFIX_BYTES,
    a.snapshot && a.claim_source && a.update && a.validate);
APH_CLIENT_SERVICE(APH_WindowApi, APH_SERVICE_WINDOWS, sizeof(APH_WindowApi),
    a.subscribe && a.query && a.unsubscribe && a.post);
APH_CLIENT_SERVICE(APH_LifecycleApi, APH_SERVICE_LIFECYCLE, sizeof(APH_LifecycleApi),
    a.register_lifecycle && a.complete_enable && a.disable && a.checkpoint &&
    a.prepare_target_change && a.restoring);
APH_CLIENT_SERVICE(APH_WriteApi, APH_SERVICE_WRITES, sizeof(APH_WriteApi),
    a.request && a.query && a.enter && a.leave && a.release && a.current);
APH_CLIENT_SERVICE(APH_PluginServiceApi, APH_SERVICE_PLUGINS, sizeof(APH_PluginServiceApi),
    a.publish && a.withdraw && a.find && a.invoke);
APH_CLIENT_SERVICE(APH_DiagnosticsApi, APH_SERVICE_DIAGNOSTICS, sizeof(APH_DiagnosticsApi),
    a.last_error && a.report);
APH_CLIENT_SERVICE(APH_ConfigApi, APH_SERVICE_CONFIG, APH_CONFIG_API_V1_PREFIX_BYTES,
    a.register_schema && a.field && a.option && a.snapshot && a.save);
#undef APH_CLIENT_SERVICE
template<> struct ServiceTraits<APH_HookApi> {
    static constexpr uint64_t id = APH_SERVICE_HOOKS;
    static bool valid(const APH_HookApi& a, uint64_t instance) {
        return a.size >= sizeof(a) && a.abi_major == APH_ABI_MAJOR &&
            a.runtime_instance == instance && a.request_hook && a.query_hook &&
            a.activate_hook && a.release_hook && a.enter_call && a.leave_call && a.record_fault;
    }
};
template<size_t N> bool text(char (&out)[N], const char* in) {
    if (!in) return false;
    const size_t n = strnlen_s(in, N);
    if (!n || n >= N) return false;
    memcpy(out, in, n + 1);
    return true;
}
inline bool same(const APH_HookRequest& a, const APH_HookRequest& b) {
    return a.request_key == b.request_key && a.target == b.target && a.native_entry == b.native_entry &&
        a.mode == b.mode && a.recovery == b.recovery && a.module_timestamp == b.module_timestamp &&
        a.module_image_size == b.module_image_size &&
        !strncmp(a.native_signature, b.native_signature, sizeof(a.native_signature));
}
inline bool same(const APH_SharedRequest& a, const APH_SharedRequest& b) {
    return same(a.native, b.native) && a.contract_version == b.contract_version &&
        a.payload_bytes == b.payload_bytes && a.original_policy == b.original_policy &&
        !strncmp(a.contract, b.contract, sizeof(a.contract));
}
inline APH_Result terminal(uint32_t state, APH_Result result) {
    if (result < 0) return result;
    if (state == APH_HOOK_NEEDS_RESTART) return APH_E_NEEDS_RESTART;
    if (state == APH_HOOK_RELEASED || state == APH_HOOK_RETIRED_PINNED) return APH_E_REVOKED;
    if (state == APH_HOOK_FAILED) return APH_E_BACKEND;
    return APH_PENDING;
}
} // namespace detail

class Client {
    APH_Owner owner_ = 0;
    const APH_Api* api_ = nullptr;
    APH_LifecycleApi life_{};
public:
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Configure once in APH_Attach, before publishing this object to workers.
    APH_Result Attach(const APH_AttachArgs* args) {
        if (!args || args->size < sizeof(*args) || !args->owner || !args->api) return APH_E_ARGUMENT;
        const auto* a = args->api;
        if (a->size < sizeof(*a) || a->abi_major != APH_ABI_MAJOR || !a->runtime_instance ||
            !a->copy_service || !a->owner_status || !a->subscribe || !a->unsubscribe) return APH_E_ABI;
        if (owner_) return owner_ == args->owner && api_ == a ? APH_OK : APH_E_KEY_REUSED;
        owner_ = args->owner;
        api_ = a;
        return APH_OK;
    }
    APH_Owner owner() const { return owner_; }
    const APH_Api* api() const { return api_; }

    // Explicit acquisition: caller caches the table. On failure out is unchanged.
    // Runtime's optional tail stays zero when an older manager returns its prefix.
    template<class T> APH_Result Service(T& out) const {
        if (!api_) return APH_E_UNAVAILABLE;
        T candidate{};
        APH_Result r = api_->copy_service(owner_, detail::ServiceTraits<T>::id, 1, sizeof(T), &candidate);
        if (r != APH_OK) return r;
        if (!detail::ServiceTraits<T>::valid(candidate, api_->runtime_instance)) return APH_E_ABI;
        out = candidate;
        return APH_OK;
    }
    APH_Result Status(APH_OwnerStatus& out) const {
        return api_ ? api_->owner_status(owner_, sizeof(out), &out) : APH_E_UNAVAILABLE;
    }
    APH_Result RegisterLifecycle(const APH_LifecycleDesc& desc) {
        if (life_.size) return APH_E_DUPLICATE;
        APH_LifecycleApi table{};
        APH_Result r = Service(table);
        if (r != APH_OK) return r;
        r = table.register_lifecycle(owner_, &desc);
        if (r == APH_OK) life_ = table;
        return r;
    }
    APH_Result CompleteEnable(uint64_t epoch, APH_Result result) const {
        return life_.size ? life_.complete_enable(owner_, epoch, result) : APH_E_UNAVAILABLE;
    }
    APH_Result Disable() const { return life_.size ? life_.disable(owner_) : APH_E_UNAVAILABLE; }
    APH_Result Checkpoint() const { return life_.size ? life_.checkpoint(owner_) : APH_E_UNAVAILABLE; }
    const APH_LifecycleApi* lifecycle() const { return life_.size ? &life_ : nullptr; }
};

// Optional settings. Core owns persistence and optional completion dispatch;
// the plugin owns the notification context and chooses how to apply the values.
class Config {
    APH_Owner owner_ = 0;
    APH_ConfigApi api_{};
public:
    APH_Result Register(const Client& client, const APH_ConfigField* fields, uint32_t count) {
        if (owner_) return APH_E_DUPLICATE;
        APH_ConfigApi api{};
        auto r = client.Service(api);
        if (r != APH_OK) return r;
        const APH_ConfigSchema schema{sizeof(schema), count, fields};
        r = api.register_schema(client.owner(), &schema);
        if (r == APH_OK) { owner_ = client.owner(); api_ = api; }
        return r;
    }
    APH_Result Status(APH_ConfigSnapshot& out) const {
        return owner_ ? api_.snapshot(owner_, sizeof(out), &out, 0, nullptr) : APH_E_UNAVAILABLE;
    }
    APH_Result Notify(APH_ConfigNotifyFn callback, void* context = nullptr) const {
        return owner_ && api_.set_notifier ? api_.set_notifier(owner_, callback, context) : APH_E_UNAVAILABLE;
    }
    APH_Result Read(APH_ConfigSnapshot& out, std::vector<APH_ConfigValue>& values) const {
        APH_ConfigSnapshot status{};
        auto r = Status(status);
        if (r != APH_OK) return r;
        values.resize(status.field_count);
        return api_.snapshot(owner_, sizeof(out), &out, status.field_count, values.data());
    }
    APH_Result Field(uint32_t index, APH_ConfigField& out) const {
        return owner_ ? api_.field(owner_, index, sizeof(out), &out) : APH_E_UNAVAILABLE;
    }
    APH_Result Option(uint32_t field, uint32_t index, APH_ConfigOption& out) const {
        return owner_ ? api_.option(owner_, field, index, sizeof(out), &out) : APH_E_UNAVAILABLE;
    }
    APH_Result Save(uint64_t revision, const std::vector<APH_ConfigValue>& edited) const {
        if (edited.size() > UINT32_MAX) return APH_E_ARGUMENT;
        return owner_ ? api_.save(owner_, revision, uint32_t(edited.size()), edited.data()) : APH_E_UNAVAILABLE;
    }
};

// Module identity is evidence of where an address resides, NOT of its true ABI
// or compatibility with a game build. The plugin must validate those separately.
inline APH_Result MakeHookRequest(APH_HookRequest& out, uint64_t key, APH_NativeProc target,
    APH_NativeProc entry, const char* signature, const char* label) {
    APH_HookRequest r{};
    r.size = sizeof(r); r.mode = APH_EXCLUSIVE; r.request_key = key;
    r.target = target; r.native_entry = entry;
    if (!key || !target || !detail::text(r.native_signature, signature) ||
        !detail::text(r.diagnostic_name, label)) return APH_E_ARGUMENT;
    if (!APH_ModuleBuild(target, &r.module_timestamp, &r.module_image_size)) return APH_E_BUILD;
    out = r;
    return APH_OK;
}

// Low-level installation helper for a plugin's HANDWRITTEN native entry.
// This object, the entry context and original slot must have module lifetime.
// No destructor releases a claim. Retire is terminal, not logical hot disable.
class Hook {
    volatile LONG busy_ = 0;
    APH_Owner owner_ = 0;
    APH_Hook handle_ = 0;
    APH_HookApi api_{};
    APH_HookRequest request_{};
    APH_NativeProc* original_slot_ = nullptr;
    bool activating_ = false, retiring_ = false;
public:
    Hook() = default;
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    APH_Owner owner() const { return owner_; }
    APH_Hook handle() const { return handle_; }
    const APH_HookApi* api() const { return &api_; }

    APH_Result Request(const Client& client, const APH_HookRequest& request) {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (request.size < sizeof(request) || request.mode != APH_EXCLUSIVE || request.reserved ||
                request.recovery > APH_RECOVERY_READ_ONLY) return APH_E_ARGUMENT;
            if (handle_) {
                if (owner_ != client.owner() || !detail::same(request_, request)) return APH_E_KEY_REUSED;
                return retiring_ ? APH_E_REVOKED : APH_PENDING;
            }
            APH_HookApi table{};
            APH_Result r = client.Service(table);
            if (r != APH_OK) return r;
            APH_Hook handle = 0;
            r = table.request_hook(client.owner(), &request, &handle);
            if (handle) { owner_ = client.owner(); api_ = table; request_ = request; handle_ = handle; }
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    APH_Result Advance(APH_NativeProc* original) {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (!handle_ || !original) return APH_E_ARGUMENT;
            if (retiring_) return APH_E_REVOKED;
            if (original_slot_ && original_slot_ != original) return APH_E_ARGUMENT;
            APH_HookStatus s{};
            APH_Result r = api_.query_hook(owner_, handle_, sizeof(s), &s);
            if (r != APH_OK) return r;
            r = detail::terminal(s.state, s.result);
            if (r < 0) return r;
            if (s.state != APH_HOOK_PREPARED && s.state != APH_HOOK_ACTIVE) return APH_PENDING;
            if (!s.original) return APH_E_ABI;
            if (!original_slot_) { *original = s.original; original_slot_ = original; }
            else if (*original != s.original) return APH_E_ARGUMENT;
            if (s.state == APH_HOOK_ACTIVE) return s.result == APH_OK ? APH_OK : APH_PENDING;
            if (!activating_) {
                r = api_.activate_hook(owner_, handle_, s.original);
                if (r < 0) return r;
                activating_ = true;
            }
            return APH_PENDING;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    APH_Result Status(APH_HookStatus& out) const {
        return handle_ ? api_.query_hook(owner_, handle_, sizeof(out), &out) : APH_E_UNAVAILABLE;
    }
    APH_Result Retire() {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (!handle_) return APH_OK;
            retiring_ = true;
            return api_.release_hook(owner_, handle_);
        } __finally { InterlockedExchange(&busy_, 0); }
    }
};

struct NativeOptions {
    bool automatic_ready = true;
    bool module_observer = false;
    uint32_t domain = 0, access_flags = 0;
    bool (*begin)(APH_NativeBinding*, APH_NativeAdmission*) = nullptr;
    void (*end)(APH_NativeAdmission*) = nullptr;
};

// One <Tag, Signature> per target for the entire module lifetime. Different
// functions, including ones with the same prototype, need different Tags.
template<unsigned Tag, class Signature> class NativeHook;
template<unsigned Tag, class R, class... A> class NativeHook<Tag, R(*)(A...)> {
    using Adapter = APH_TypedNative<Tag, R(*)(A...)>;
    inline static Hook hook_{};
    inline static volatile LONG busy_ = 0;
    inline static bool configured_ = false;
    inline static NativeOptions options_{};
public:
    using Function = R(*)(A...);
    static APH_Result Request(const Client& client, uint64_t key, Function target, Function business,
        const char* signature, const char* label, const NativeOptions& options = {}) {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (!business || bool(options.begin) != bool(options.end) ||
                ((options.domain || options.access_flags) && !options.begin)) return APH_E_ARGUMENT;
            if (configured_ && (Adapter::business != business || options_.automatic_ready != options.automatic_ready ||
                options_.module_observer != options.module_observer || options_.domain != options.domain ||
                options_.access_flags != options.access_flags || options_.begin != options.begin || options_.end != options.end))
                return APH_E_KEY_REUSED;
            APH_HookRequest request{};
            APH_Result r = MakeHookRequest(request, key, (APH_NativeProc)target,
                (APH_NativeProc)Adapter::Entry, signature, label);
            if (r != APH_OK) return r;
            r = hook_.Request(client, request);
            if (hook_.handle() && !configured_) {
                auto& b = Adapter::binding;
                b.owner = hook_.owner(); b.api = hook_.api(); b.hook = hook_.handle();
                b.domain = options.domain; b.access_flags = options.access_flags;
                b.module_observer = options.module_observer; b.begin = options.begin; b.end = options.end;
                Adapter::business = business; options_ = options; configured_ = true;
            }
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    static APH_Result Advance() {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (!configured_) return APH_E_UNAVAILABLE;
            APH_Result r = hook_.Advance(&Adapter::binding.original);
            if (r == APH_OK && options_.automatic_ready) InterlockedExchange(&Adapter::binding.ready, 1);
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    // Optional group commit: use automatic_ready=false and publish only after
    // all related requests AND plugin-specific validation have succeeded.
    static APH_Result PublishReady() {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            APH_Result r = hook_.Advance(&Adapter::binding.original);
            if (r == APH_OK) InterlockedExchange(&Adapter::binding.ready, 1);
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    static APH_Result Status(APH_HookStatus& out) { return hook_.Status(out); }
    static APH_Hook handle() { return hook_.handle(); }
    static APH_Result Retire() {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            InterlockedExchange(&Adapter::binding.ready, 0);
            return hook_.Retire();
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    static R Original(A... args) { return Adapter::Original(args...); }
};

// Generic shared registration. No predefined game contracts or ABI adapter.
// A provider's object/entry/original remain resident even after Retire succeeds.
class SharedHook {
    volatile LONG busy_ = 0;
    APH_Owner owner_ = 0;
    APH_SharedClaim handle_ = 0;
    APH_SharedApi api_{};
    APH_SharedRequest request_{};
    APH_NativeProc* original_slot_ = nullptr;
    bool activating_ = false, retiring_ = false;
public:
    SharedHook() = default;
    SharedHook(const SharedHook&) = delete;
    SharedHook& operator=(const SharedHook&) = delete;
    APH_Owner owner() const { return owner_; }
    APH_SharedClaim handle() const { return handle_; }
    const APH_SharedApi* api() const { return &api_; }
    APH_Result Request(const Client& client, const APH_SharedRequest& request) {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (request.size < sizeof(request) || request.native.size < sizeof(request.native) ||
                request.native.mode != APH_SHARED_TYPED || request.reserved || request.reserved2 ||
                request.native.reserved || request.native.recovery != APH_RECOVERY_NONE) return APH_E_ARGUMENT;
            if (handle_) {
                if (owner_ != client.owner() || !detail::same(request_, request)) return APH_E_KEY_REUSED;
                return retiring_ ? APH_E_REVOKED : APH_PENDING;
            }
            APH_SharedApi table{};
            APH_Result r = client.Service(table);
            if (r != APH_OK) return r;
            APH_SharedClaim handle = 0;
            r = table.request(client.owner(), &request, &handle);
            if (handle) { owner_ = client.owner(); api_ = table; request_ = request; handle_ = handle; }
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    // Only the selected provider writes original. A member passes nullptr;
    // candidate adapters may pass their slot, which is untouched for members.
    APH_Result Advance(APH_NativeProc* provider_original = nullptr) {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (!handle_) return APH_E_UNAVAILABLE;
            if (retiring_) return APH_E_REVOKED;
            APH_SharedStatus s{};
            APH_Result r = api_.query(owner_, handle_, sizeof(s), &s);
            if (r != APH_OK) return r;
            r = detail::terminal(s.state, s.result);
            if (r < 0) return r;
            if (s.state != APH_HOOK_PREPARED && s.state != APH_HOOK_ACTIVE) return APH_PENDING;
            if (s.is_provider) {
                if (!provider_original || !s.original) return APH_E_ARGUMENT;
                if (original_slot_ && (original_slot_ != provider_original || *provider_original != s.original))
                    return APH_E_ARGUMENT;
                if (!original_slot_) { *provider_original = s.original; original_slot_ = provider_original; }
            }
            if (s.state == APH_HOOK_ACTIVE) return s.result == APH_OK ? APH_OK : APH_PENDING;
            if (!activating_) {
                r = api_.activate(owner_, handle_, s.is_provider ? s.original : nullptr);
                if (r < 0) return r;
                activating_ = true;
            }
            return APH_PENDING;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    APH_Result Status(APH_SharedStatus& out) const {
        return handle_ ? api_.query(owner_, handle_, sizeof(out), &out) : APH_E_UNAVAILABLE;
    }
    APH_Result Retire() {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (!handle_) return APH_OK;
            retiring_ = true;
            return api_.release(owner_, handle_);
        } __finally { InterlockedExchange(&busy_, 0); }
    }
};

inline APH_Result MakeSharedRequest(APH_SharedRequest& out, uint64_t key, APH_NativeProc target,
    APH_NativeProc entry, const char* signature, const char* contract, uint32_t version,
    uint32_t payload_bytes, uint32_t original_policy = APH_ORIGINAL_ONCE) {
    APH_SharedRequest r{}; r.size = sizeof(r);
    APH_Result result = MakeHookRequest(r.native, key, target, entry, signature, contract);
    if (result != APH_OK) return result;
    if (!version || payload_bytes > 256 || !detail::text(r.contract, contract) ||
        (original_policy != APH_ORIGINAL_ONCE && original_policy != APH_CONTROLLED_VOID)) return APH_E_ARGUMENT;
    r.native.mode = APH_SHARED_TYPED; r.contract_version = version;
    r.payload_bytes = payload_bytes; r.original_policy = original_policy;
    out = r;
    return APH_OK;
}

// Explicit close, never destructor cleanup: Pending retains both the handle
// and borrowed callback context. Close -> OK before Listen for a new epoch.
class Subscription {
    volatile LONG busy_ = 0;
    APH_Owner owner_ = 0;
    APH_Subscription handle_ = 0;
    APH_Result (APH_CALL *close_)(APH_Owner, APH_Subscription) = nullptr;
public:
    Subscription() = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    APH_Subscription handle() const { return handle_; }
    APH_Result Listen(const Client& client, const APH_SubscribeDesc& desc) {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (handle_) return APH_E_BUSY;
            if (!client.api()) return APH_E_UNAVAILABLE;
            APH_Subscription handle = 0;
            APH_Result r = client.api()->subscribe(client.owner(), &desc, &handle);
            if (handle) { owner_ = client.owner(); handle_ = handle; close_ = client.api()->unsubscribe; }
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    APH_Result Listen(const SharedHook& hook, const APH_SharedListen& desc) {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (handle_) return APH_E_BUSY;
            if (!hook.handle()) return APH_E_UNAVAILABLE;
            APH_Subscription handle = 0;
            APH_Result r = hook.api()->listen(hook.owner(), hook.handle(), &desc, &handle);
            if (handle) { owner_ = hook.owner(); handle_ = handle; close_ = hook.api()->unlisten; }
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
    APH_Result Close() {
        if (InterlockedCompareExchange(&busy_, 1, 0)) return APH_E_BUSY;
        __try {
            if (!handle_) return APH_OK;
            APH_Result r = close_(owner_, handle_);
            // A stale generation has already closed, drained and been reused by
            // Core. It cannot still refer to this subscription's callback.
            if (r == APH_OK || r == APH_E_STALE) { handle_ = 0; close_ = nullptr; return APH_OK; }
            return r;
        } __finally { InterlockedExchange(&busy_, 0); }
    }
};
} // namespace applepie
#pragma pack(pop)
