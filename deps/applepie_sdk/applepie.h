#ifndef APPLEPIE_H
#define APPLEPIE_H
/* Windows x64, MSVC C11/C++17. In-process C ABI, pack 8. No CRT ownership transfer. */
#include <stdint.h>
#include <stddef.h>
#define APH_CALL __cdecl
#ifdef APH_BUILD_MANAGER
#define APH_EXPORT __declspec(dllexport)
#else
#define APH_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
typedef int32_t APH_Result;
typedef uint64_t APH_Owner;
typedef uint64_t APH_Hook;
typedef uint64_t APH_Subscription;
typedef void (APH_CALL *APH_NativeProc)(void);
enum { APH_ABI_MAJOR = 1, APH_ABI_MINOR = 1, APH_ARCH_X64 = 0x8664 };
enum {
    APH_OK = 0, APH_PENDING = 1,
    APH_E_ARGUMENT = -1, APH_E_ABI = -2, APH_E_UNAVAILABLE = -3,
    APH_E_BUSY = -4, APH_E_REVOKED = -5, APH_E_QUEUE_FULL = -6,
    APH_E_NEEDS_RESTART = -7, APH_E_FAULT = -8, APH_E_BACKEND = -9,
    APH_E_SIGNATURE = -10, APH_E_MODE = -11, APH_E_CONFLICT = -12,
    APH_E_KEY_REUSED = -13, APH_E_BUILD = -14, APH_E_FOREIGN_PATCH = -15,
    APH_E_CAPABILITY = -16, APH_E_DUPLICATE = -17,
    APH_E_DEPENDENCY = -18, APH_E_VERSION = -19, APH_E_CYCLE = -20,
    APH_E_IO = -21
};
#define APH_CAP_CORE UINT64_C(1)
#define APH_CAP_HOOKS UINT64_C(2)
#define APH_CAP_NOTICES UINT64_C(4)
/* Reserved, not advertised by the runtime. Resolution of some IL2CPP exports is not readiness. */
#define APH_CAP_GAME UINT64_C(0x100)
#define APH_CAP_GUI UINT64_C(0x200)
#define APH_SERVICE_HOOKS UINT64_C(0x415048484f4f4b31)
enum { APH_COLD, APH_STARTING, APH_CORE_READY, APH_SCANNED, APH_BOOT_FAILED };
enum { APH_OWNER_ATTACHED = 1, APH_OWNER_WAITING, APH_OWNER_ENABLING,
       APH_OWNER_ACTIVE, APH_OWNER_FAILED, APH_OWNER_FAULTED, APH_OWNER_NEEDS_RESTART };
enum { APH_EXCLUSIVE = 1, APH_SHARED_TYPED = 2 /* use APH_SERVICE_SHARED for this mode */ };
enum { APH_HOOK_PENDING, APH_HOOK_PREPARED, APH_HOOK_ACTIVE,
       APH_HOOK_RELEASING, APH_HOOK_RELEASED, APH_HOOK_RETIRED_PINNED,
       APH_HOOK_FAILED, APH_HOOK_NEEDS_RESTART };
enum { APH_ORIGINAL_NOT_ENTERED, APH_ORIGINAL_RUNNING, APH_ORIGINAL_RETURNED };
enum { APH_RECOVERY_NONE, APH_RECOVERY_READ_ONLY };
enum { APH_NOTICE_CAPABILITIES = 1 };
enum { APH_CALL_BUSINESS = 1, APH_CALL_RECURSIVE = 2, APH_CALL_NATIVE_READY = 4 };

/* Export DATA named APH_PluginManifest. No pointers, no executable preflight.
 * DllMain/static constructors must remain passive. ID: [a-z0-9._-], 1..63 chars.
 * Export APH_Attach and APH_Enable with the function types below. */
typedef struct APH_Manifest {
    uint32_t size;
    uint16_t abi_major, required_minor;
    uint32_t architecture;
    uint32_t flags; /* APH_PLUGIN_* flags; zero selects the basic lifecycle */
    uint64_t required_capabilities, optional_capabilities;
    char id[64];
    char display_name[64];
} APH_Manifest;

/* Optional readonly DATA export APH_PluginInfo. Keep Manifest unchanged.
 * Plugins declaring this extension require ABI minor >= 1. Versions are
 * numeric major.minor.patch, 16 bits each; max_version is exclusive (0 = any).
 * Missing/incompatible OPTIONAL dependencies never prevent activation. */
#define APH_VERSION(major, minor, patch) (((uint64_t)(major) << 32) | ((uint64_t)(minor) << 16) | (uint64_t)(patch))
enum { APH_DEP_OPTIONAL = 1, APH_MAX_DEPENDENCIES = 8 };
typedef struct APH_Dependency {
    char id[64];
    uint64_t min_version, max_version;
    uint32_t flags, reserved;
} APH_Dependency;
typedef struct APH_PluginInfoData {
    uint32_t size, format;
    uint64_t plugin_version;
    uint32_t dependency_count, reserved;
    APH_Dependency dependencies[APH_MAX_DEPENDENCIES];
} APH_PluginInfoData;
typedef struct APH_BootStatus {
    uint32_t size, state;
    APH_Result result;
    uint32_t scan_count;
    uint64_t runtime_instance;
    uint64_t capabilities;
} APH_BootStatus;
typedef struct APH_OwnerStatus {
    uint32_t size, state;
    uint64_t activation_epoch;
    uint32_t in_flight, fault_code;
    uint64_t fault_address;
    APH_Hook fault_hook;
    uint32_t fault_phase, fault_thread;
    APH_Result result;
    uint32_t resource_count;
} APH_OwnerStatus;
typedef struct APH_Notice {
    uint32_t size, kind;
    uint64_t capabilities, activation_epoch;
} APH_Notice;
typedef void (APH_CALL *APH_NoticeFn)(APH_Owner, const APH_Notice*, void*);
typedef struct APH_SubscribeDesc {
    uint32_t size, kind;
    uint32_t recovery; /* READ_ONLY only; no game access/write permission */
    uint32_t reserved;
    APH_NoticeFn callback;
    void* user_data; /* retain until unsubscribe returns OK (poll if Pending) */
} APH_SubscribeDesc;
typedef struct APH_Api {
    uint32_t size;
    uint16_t abi_major, abi_minor;
    uint64_t runtime_instance;
    APH_Result (APH_CALL *copy_service)(APH_Owner, uint64_t, uint32_t, uint32_t, void*);
    APH_Result (APH_CALL *owner_status)(APH_Owner, uint32_t, APH_OwnerStatus*);
    APH_Result (APH_CALL *subscribe)(APH_Owner, const APH_SubscribeDesc*, APH_Subscription*);
    APH_Result (APH_CALL *unsubscribe)(APH_Owner, APH_Subscription);
} APH_Api;
typedef struct APH_AttachArgs {
    uint32_t size, reserved;
    APH_Owner owner;
    const APH_Api* api; /* process lifetime */
} APH_AttachArgs;
typedef APH_Result (APH_CALL *APH_AttachFn)(const APH_AttachArgs*);
typedef APH_Result (APH_CALL *APH_EnableFn)(APH_Owner, uint64_t activation_epoch);

typedef struct APH_HookRequest {
    uint32_t size, mode;
    uint64_t request_key;
    APH_NativeProc target, native_entry;
    uint32_t module_timestamp, module_image_size; /* exact loaded PE identity */
    uint32_t recovery, reserved;
    char native_signature[128]; /* exact ABI incl. hidden args; declaration, not proof */
    char diagnostic_name[64];
} APH_HookRequest;
typedef struct APH_HookStatus {
    uint32_t size, state;
    APH_Result result;
    uint32_t mode;
    APH_Hook hook;
    APH_Owner owner;
    APH_NativeProc original;
    uint32_t in_flight, backend_status;
} APH_HookStatus;
typedef struct APH_HookCall {
    uint32_t size, flags;
    APH_Hook hook;
    uint64_t activation_epoch;
    uint32_t depth, reserved;
} APH_HookCall;
typedef struct APH_HookApi {
    uint32_t size;
    uint16_t abi_major, abi_minor;
    uint64_t runtime_instance;
    /* All mutations enqueue; never wait from a callback. Arguments are copied.
     * Pending yields a claim, query until Prepared. Failures clear output handles. */
    APH_Result (APH_CALL *request_hook)(APH_Owner, const APH_HookRequest*, APH_Hook*);
    APH_Result (APH_CALL *query_hook)(APH_Owner, APH_Hook, uint32_t, APH_HookStatus*);
    /* Store original/context first, then pass that original here to publish it. */
    APH_Result (APH_CALL *activate_hook)(APH_Owner, APH_Hook, APH_NativeProc saved_original);
    /* Closes business admission immediately. Poll query; never grants DLL unload.
     * Once enabled, trampoline/target claim remain pinned until process exit. */
    APH_Result (APH_CALL *release_hook)(APH_Owner, APH_Hook);
    /* Exact native entry must pair these in __try/__finally. Recursion skips
     * business dispatch but still calls the exact original. No generic invocation. */
    APH_Result (APH_CALL *enter_call)(APH_Owner, APH_Hook, APH_HookCall*);
    APH_Result (APH_CALL *leave_call)(APH_Owner, const APH_HookCall*);
    /* Filter-safe: fixed atomic record, no lock, allocation or plugin/log call.
     * Only SDK-validated read-only faults may request recovery. Unknown native
     * state closes admission and marks NeedsRestart; original faults propagate. */
    void (APH_CALL *record_fault)(APH_Owner, APH_Hook, uint32_t code,
                                 uint64_t address, uint32_t phase, uint32_t recoverable);
} APH_HookApi;
APH_EXPORT APH_Result APH_CALL APH_Bootstrap(uint32_t major);
APH_EXPORT APH_Result APH_CALL APH_GetBootStatus(uint32_t bytes, APH_BootStatus*);
APH_EXPORT APH_Result APH_CALL APH_GetApi(uint32_t major, uint32_t minor, uint32_t bytes, APH_Api*);
typedef APH_Result (APH_CALL *APH_BootstrapFn)(uint32_t);
typedef APH_Result (APH_CALL *APH_GetBootStatusFn)(uint32_t, APH_BootStatus*);
typedef APH_Result (APH_CALL *APH_GetApiFn)(uint32_t, uint32_t, uint32_t, APH_Api*);

/* Shared hooks, runtime/target tickets, pointer patches and window routing. */
#define APH_SERVICE_SHARED UINT64_C(0x4150485348524431)
#define APH_SERVICE_POINTERS UINT64_C(0x4150485054525331)
#define APH_SERVICE_RUNTIME UINT64_C(0x41504852554e5431)
#define APH_SERVICE_WINDOWS UINT64_C(0x41504857494e4431)
typedef uint64_t APH_SharedClaim;
typedef uint64_t APH_Patch;
typedef uint64_t APH_Route;
enum { APH_E_CONTRACT = -30, APH_E_STALE = -31, APH_E_THREAD = -32, APH_E_EXPECTED = -33 };
enum { APH_BEFORE = 1, APH_AFTER = 2 };
enum { APH_ORIGINAL_ONCE = 1, APH_CONTROLLED_VOID = 2 };
enum { APH_OBSERVER = 0, APH_CONTROLLER = 1 };
enum { APH_CONTINUE = 0, APH_SUPPRESS = 2 };
enum { APH_SHARED_DISPATCH = 1, APH_SHARED_RECURSIVE = 2 };
typedef struct APH_SharedRequest {
    uint32_t size, reserved;
    APH_HookRequest native;
    char contract[64];
    uint32_t contract_version, payload_bytes;
    uint32_t original_policy, reserved2;
} APH_SharedRequest;
typedef struct APH_SharedStatus {
    uint32_t size, state;
    APH_Result result;
    uint32_t is_provider;
    APH_Hook native_hook;
    APH_NativeProc original; /* only exposed to the selected provider */
    uint32_t members, in_flight;
} APH_SharedStatus;
typedef struct APH_SharedCall {
    uint32_t size, flags;
    APH_SharedClaim provider;
    APH_HookCall native_call;
    uint64_t invocation;
} APH_SharedCall;
typedef struct APH_SharedEvent {
    uint32_t size, phase;
    uint64_t invocation, activation_epoch;
    const void* payload; /* exact contract type, borrowed, immutable */
    uint32_t payload_bytes, original_suppressed;
} APH_SharedEvent;
typedef APH_Result (APH_CALL *APH_SharedCallback)(APH_Owner, const APH_SharedEvent*, void*);
typedef struct APH_SharedListen {
    uint32_t size, phases;
    uint32_t role, recovery;
    APH_SharedCallback callback;
    void* user_data;
} APH_SharedListen;
typedef struct APH_SharedApi {
    uint32_t size, version;
    APH_Result (APH_CALL *request)(APH_Owner, const APH_SharedRequest*, APH_SharedClaim*);
    APH_Result (APH_CALL *query)(APH_Owner, APH_SharedClaim, uint32_t, APH_SharedStatus*);
    APH_Result (APH_CALL *activate)(APH_Owner, APH_SharedClaim, APH_NativeProc saved_original);
    APH_Result (APH_CALL *release)(APH_Owner, APH_SharedClaim);
    APH_Result (APH_CALL *listen)(APH_Owner, APH_SharedClaim, const APH_SharedListen*, APH_Subscription*);
    APH_Result (APH_CALL *unlisten)(APH_Owner, APH_Subscription);
    /* Provider code/context remain resident even when its business owner fails.
     * Exact typed adapter: enter -> before -> original (or controlled void skip)
     * -> after -> finally leave. An observer never receives original or write authority. */
    APH_Result (APH_CALL *enter)(APH_Owner, APH_SharedClaim, APH_SharedCall*);
    APH_Result (APH_CALL *publish)(const APH_SharedCall*, uint32_t phase, const void*, uint32_t bytes);
    APH_Result (APH_CALL *leave)(const APH_SharedCall*);
    /* Adapter/original fault: filter-safe, quarantine the whole site. Propagate. */
    void (APH_CALL *native_fault)(APH_Owner, APH_SharedClaim, uint32_t, uint64_t, uint32_t phase);
} APH_SharedApi;

enum { APH_RUNTIME_WAITING, APH_RUNTIME_LIVE, APH_RUNTIME_STOPPING, APH_RUNTIME_DEAD };
enum { APH_TARGET_INVALIDATE = 1, APH_TARGET_BIND = 2, APH_RUNTIME_SET_LIVE = 3,
       APH_RUNTIME_BEGIN_STOP = 4, APH_RUNTIME_SET_DEAD = 5 };
enum { APH_ACCESS_RUNTIME = 1, APH_ACCESS_TARGET = 2, APH_ACCESS_GAME_THREAD = 4 };
typedef struct APH_RuntimeSnapshot {
    uint32_t size, state;
    uint64_t runtime_epoch, target_generation;
    uint64_t target_identity; /* diagnostic identity only; never permission to dereference */
    uint32_t verified_thread, target_ready;
    APH_Owner source;
} APH_RuntimeSnapshot;
typedef struct APH_Access {
    uint32_t size, flags;
    uint64_t activation_epoch, runtime_epoch, target_generation;
} APH_Access;
typedef struct APH_RuntimeUpdate {
    uint32_t size, operation;
    uint64_t target_identity;
    uint32_t verified_thread, reserved;
} APH_RuntimeUpdate;
typedef struct APH_RuntimeApi {
    uint32_t size, version;
    APH_Result (APH_CALL *snapshot)(uint32_t, APH_RuntimeSnapshot*);
    /* Cooperative, single selected evidence source; LIVE does NOT publish GAME
     * capabilities. Only a validated native callback can attest its thread. */
    APH_Result (APH_CALL *claim_source)(APH_Owner);
    APH_Result (APH_CALL *update)(APH_Owner, const APH_RuntimeUpdate*);
    APH_Result (APH_CALL *validate)(APH_Owner, const APH_Access*);
    /* Optional tail, available when size >= sizeof(APH_RuntimeApi). Constant
     * time module/runtime check: WAITING/LIVE and a non-faulted resident owner.
     * NOT business, thread, target or write permission. No cached allow result;
     * callers still use validate/write admission at each work boundary.
     * Older managers copy only the prefix; zero-initialize before copy_service. */
    APH_Result (APH_CALL *probe_open)(APH_Owner);
} APH_RuntimeApi;
#define APH_RUNTIME_API_V1_PREFIX_BYTES ((uint32_t)offsetof(APH_RuntimeApi, probe_open))

enum { APH_PATCH_TEMPORARY = 1, APH_PATCH_PERSISTENT = 2 };
enum { APH_PATCH_APPLIED = 1, APH_PATCH_RESTORED = 2, APH_PATCH_UNCERTAIN = 3 };
typedef struct APH_PointerRequest {
    uint32_t size, scope;
    void** slot;
    void* expected;
    void* replacement;
    uint32_t module_timestamp, module_image_size;
    APH_Access access;
} APH_PointerRequest;
typedef struct APH_PointerStatus {
    uint32_t size, state;
    APH_Result result;
    uint32_t reserved;
    void* original;
    void* replacement;
} APH_PointerStatus;
typedef struct APH_PointerApi {
    uint32_t size, version;
    /* Writable, aligned, pinned PE data slots only. No generic WriteMemory or
     * IAT protection changes. Same-slot nesting returns Busy, never reuses a lease. */
    APH_Result (APH_CALL *request)(APH_Owner, const APH_PointerRequest*, APH_Patch*);
    APH_Result (APH_CALL *release)(APH_Owner, APH_Patch);
    APH_Result (APH_CALL *query)(APH_Owner, APH_Patch, uint32_t, APH_PointerStatus*);
    void (APH_CALL *wrapper_fault)(APH_Owner, APH_Patch, uint32_t, uint64_t);
} APH_PointerApi;

enum { APH_WINDOW_BEFORE = 1, APH_WINDOW_AFTER = 2, APH_WINDOW_COMMAND = 3 };
enum { APH_WINDOW_PENDING, APH_WINDOW_BOUND, APH_WINDOW_DEAD, APH_WINDOW_FAILED };
typedef struct APH_WindowEvent {
    uint32_t size, phase;
    uint64_t window, window_generation;
    uint32_t message, reserved;
    uint64_t wparam;
    int64_t lparam, original_result;
    APH_Access access;
} APH_WindowEvent;
typedef void (APH_CALL *APH_WindowCallback)(APH_Owner, const APH_WindowEvent*, void*);
typedef struct APH_WindowRequest {
    uint32_t size, recovery;
    uint64_t window;
    APH_WindowCallback callback;
    void* user_data;
} APH_WindowRequest;
typedef struct APH_WindowStatus {
    uint32_t size, state;
    uint64_t window_generation;
    uint32_t in_flight, window_thread;
    APH_Result result;
    uint32_t reserved;
} APH_WindowStatus;
typedef struct APH_WindowCommand {
    uint32_t size, opcode;
    uint64_t wparam;
    int64_t lparam; /* scalar only, never a borrowed pointer */
    APH_Access access;
} APH_WindowCommand;
typedef struct APH_WindowApi {
    uint32_t size, version;
    APH_Result (APH_CALL *subscribe)(APH_Owner, const APH_WindowRequest*, APH_Route*);
    APH_Result (APH_CALL *query)(APH_Owner, APH_Route, uint32_t, APH_WindowStatus*);
    APH_Result (APH_CALL *unsubscribe)(APH_Owner, APH_Route);
    APH_Result (APH_CALL *post)(APH_Owner, APH_Route, const APH_WindowCommand*);
} APH_WindowApi;

/* Lifecycle and game-write ownership. */
#define APH_SERVICE_LIFECYCLE UINT64_C(0x4150484c49464531)
#define APH_SERVICE_WRITES UINT64_C(0x4150485752495431)
#define APH_PLUGIN_LIFECYCLE 0x100u
#define APH_PLUGIN_LATE_LOAD 0x200u
#define APH_PLUGIN_MODULE_OBSERVER 0x400u
#define APH_PLUGIN_FLAGS (APH_PLUGIN_LIFECYCLE|APH_PLUGIN_LATE_LOAD|APH_PLUGIN_MODULE_OBSERVER)
enum { APH_OWNER_DISABLING=8, APH_OWNER_DISABLED=9 };
enum { APH_ANIMATION=1, APH_CAMERA=2 };
enum { APH_WRITE_FREE, APH_WRITE_OWNED, APH_WRITE_RELEASING, APH_WRITE_UNCERTAIN };
enum { APH_CLEANUP_RELEASE=1, APH_CLEANUP_DISABLE=2, APH_CLEANUP_TARGET_CHANGE=3 };
enum { APH_MODULE_OBSERVER=2 };
#define APH_WINDOW_MODULE_OBSERVER 0x100u
typedef uint64_t APH_WriteLease;
typedef struct APH_Cleanup {
    uint32_t size, reason, domains, reserved;
    APH_Access access;
} APH_Cleanup;
typedef void (APH_CALL *APH_DisablingFn)(APH_Owner,uint64_t epoch,void*);
typedef APH_Result (APH_CALL *APH_RestoreFn)(APH_Owner,const APH_Cleanup*,void*);
typedef struct APH_LifecycleDesc {
    uint32_t size, timeout_ms;
    APH_DisablingFn disabling; /* scalar/plugin-work cancellation only, no Unity */
    APH_RestoreFn restore; /* verified game thread when domains != 0 */
    void* context; /* module lifetime */
} APH_LifecycleDesc;
typedef struct APH_LifecycleApi {
    uint32_t size,version;
    APH_Result (APH_CALL *register_lifecycle)(APH_Owner,const APH_LifecycleDesc*);
    APH_Result (APH_CALL *complete_enable)(APH_Owner,uint64_t epoch,APH_Result);
    APH_Result (APH_CALL *disable)(APH_Owner);
    /* From a resident native/window observer at a documented safe phase.
     * Never waits. Runs restoration only after admission has closed/drained. */
    APH_Result (APH_CALL *checkpoint)(APH_Owner);
    /* Verified source before replacement. Pending closes ordinary target/write
     * access, retaining the old cleanup context and leases for safe checkpoints.
     * Keep old client roots/caches and retry from resident observers; never wait
     * or delay the game's original call. OK revokes the old target generation;
     * discover and bind the current target afterwards. Never invalidate/bind or
     * discard old restore state on Pending. Failure/timeout retains ownership. */
    APH_Result (APH_CALL *prepare_target_change)(APH_Owner source);
    /* True only inside Core's restricted restore callback on this thread. */
    uint32_t (APH_CALL *restoring)(APH_Owner);
} APH_LifecycleApi;
typedef struct APH_WriteRequest {uint32_t size,domain;APH_Access access;} APH_WriteRequest;
typedef struct APH_WriteStatus {
    uint32_t size,state,domain,in_flight;
    APH_Owner owner;
    APH_WriteLease lease;
    uint64_t activation_epoch,target_generation;
    APH_Result result;
    uint32_t reserved;
} APH_WriteStatus;
typedef struct APH_WriteCall {uint32_t size,domain;APH_Owner owner;APH_WriteLease lease;uint64_t serial;} APH_WriteCall;
typedef struct APH_WriteApi {
    uint32_t size,version;
    APH_Result (APH_CALL *request)(APH_Owner,const APH_WriteRequest*,APH_WriteLease*);
    APH_Result (APH_CALL *query)(uint32_t domain,uint32_t bytes,APH_WriteStatus*);
    APH_Result (APH_CALL *enter)(APH_Owner,APH_WriteLease,const APH_Access*,APH_WriteCall*);
    APH_Result (APH_CALL *leave)(const APH_WriteCall*);
    /* Closes new writes immediately; checkpoint calls the registered restore.
     * Ownership is not transferred until restore returns OK and counts drain. */
    APH_Result (APH_CALL *release)(APH_Owner,APH_WriteLease);
    uint32_t (APH_CALL *current)(APH_Owner); /* domains admitted on THIS thread */
} APH_WriteApi;
/* Synchronous, bounded POD messages. Core never exports a provider's raw function
 * pointer. Calls pin both activation epochs until return; disable drains them.
 * No pointers/CRT objects in payloads. Buffers are borrowed only for the call.
 * GAME_THREAD checks the verified runtime thread; it grants no write lease. */
#define APH_SERVICE_PLUGINS UINT64_C(0x415048504c554731)
#define APH_SERVICE_DIAGNOSTICS UINT64_C(0x4150484449414731)
enum { APH_SERVICE_ANY_THREAD = 0, APH_SERVICE_GAME_THREAD = 1, APH_SERVICE_MAX_BYTES = 4096 };
typedef uint64_t APH_PluginService;
typedef APH_Result (APH_CALL *APH_ServiceFn)(void* context, uint32_t operation,
    const void* input, uint32_t input_bytes, void* output, uint32_t output_bytes);
typedef struct APH_ServiceDesc {
    uint32_t size, thread_policy;
    uint64_t activation_epoch;
    char name[64];
    uint32_t major, minor;
    uint32_t recovery, reserved;
    APH_ServiceFn invoke;
    void* context;
} APH_ServiceDesc;
typedef struct APH_ServiceQuery {
    uint32_t size, major;
    char provider[64], name[64];
    uint32_t min_minor, reserved;
} APH_ServiceQuery;
typedef struct APH_PluginServiceApi {
    uint32_t size, version;
    APH_Result (APH_CALL *publish)(APH_Owner, const APH_ServiceDesc*, APH_PluginService*);
    APH_Result (APH_CALL *withdraw)(APH_Owner, APH_PluginService);
    APH_Result (APH_CALL *find)(APH_Owner, const APH_ServiceQuery*, APH_PluginService*);
    APH_Result (APH_CALL *invoke)(APH_Owner, uint64_t activation_epoch, APH_PluginService,
        uint32_t operation, const void* input, uint32_t input_bytes, void* output, uint32_t output_bytes);
} APH_PluginServiceApi;
typedef struct APH_ErrorDetail {
    uint32_t size;
    APH_Result result;
    uint32_t win32_error, backend_status;
    uint64_t timestamp_ms, target;
    APH_Hook hook;
    char operation[32], resource[64], message[256];
} APH_ErrorDetail;
typedef struct APH_DiagnosticsApi {
    uint32_t size, version;
    APH_Result (APH_CALL *last_error)(APH_Owner, uint32_t bytes, APH_ErrorDetail*);
    APH_Result (APH_CALL *report)(APH_Owner, const APH_ErrorDetail*);
} APH_DiagnosticsApi;

/* Optional module-lifetime basic settings. All strings are bounded UTF-8.
 * Registration copies the schema and reads disk once; call from Attach/worker.
 * Snapshots are memory-only. Optional completion notices never grant game access.
 * Saving is asynchronous and publishes values only after atomic replacement.
 * Config ID "manager" is reserved for Manager's own preferences file.
 * Old hosts return UNAVAILABLE from copy_service; existing ABI is unchanged. */
#define APH_SERVICE_CONFIG UINT64_C(0x415048434f4e4631)
enum { APH_CONFIG_BOOL = 1, APH_CONFIG_INT, APH_CONFIG_FLOAT, APH_CONFIG_ENUM, APH_CONFIG_TEXT, APH_CONFIG_KEY };
enum { APH_CONFIG_TEXT_BYTES = 256 };
typedef struct APH_ConfigValue {
    uint32_t type;
    int32_t integer; /* BOOL: 0/1; INT: signed 32 bits; KEY: Windows VK 0..255, 0 unbound */
    double number; /* FLOAT only, finite */
    char text[APH_CONFIG_TEXT_BYTES]; /* ENUM: stable option string; TEXT: one line */
} APH_ConfigValue;
typedef struct APH_ConfigOption { char value[64]; } APH_ConfigOption;
typedef struct APH_ConfigField {
    uint32_t size, type;
    char key[48], label[96], description[160];
    APH_ConfigValue initial;
    double minimum, maximum; /* Required range for INT/FLOAT; inclusive. */
    uint32_t text_max_bytes, option_count; /* TEXT: 1..255 UTF-8 bytes; ENUM: nonempty */
    const APH_ConfigOption* options; /* Copied at registration; field() returns NULL here. */
} APH_ConfigField;
typedef struct APH_ConfigSchema {
    uint32_t size, field_count;
    const APH_ConfigField* fields; /* Copied, including defaults and options. */
} APH_ConfigSchema;
typedef struct APH_ConfigSnapshot {
    uint32_t size, field_count;
    uint64_t revision; /* Starts at 1. Incremented only after a successful save. */
    uint32_t pending;
    APH_Result last_result;
    uint32_t win32_error, defaulted_fields; /* Number of invalid stored fields. */
} APH_ConfigSnapshot;
typedef void (APH_CALL *APH_ConfigNotifyFn)(APH_Owner, void*);
enum { APH_CONFIG_API_V1_PREFIX_BYTES = 48 };
typedef struct APH_ConfigApi {
    uint32_t size, version;
    APH_Result (APH_CALL *register_schema)(APH_Owner, const APH_ConfigSchema*);
    APH_Result (APH_CALL *field)(APH_Owner, uint32_t index, uint32_t bytes, APH_ConfigField*);
    APH_Result (APH_CALL *option)(APH_Owner, uint32_t field, uint32_t index, uint32_t bytes, APH_ConfigOption*);
    /* NULL values + zero capacity reads metadata only. Otherwise capacity must
     * hold field_count values in schema order. No partial success. */
    APH_Result (APH_CALL *snapshot)(APH_Owner, uint32_t bytes, APH_ConfigSnapshot*,
                                  uint32_t capacity, APH_ConfigValue* values);
    /* Full transaction. Expected revision prevents overwriting newer edits.
     * PENDING: queued; BUSY: another save; STALE: reread before retrying.
     * Use completion notices, then read a snapshot. Config survives disable/enable;
     * plugins apply snapshots in their own appropriate phase, not on this call. */
    APH_Result (APH_CALL *save)(APH_Owner, uint64_t expected_revision,
                              uint32_t count, const APH_ConfigValue*);
    /* Optional appended v1 extension. Install once after register_schema, before
     * the first save; then read the initial snapshot once. Older hosts leave NULL.
     * Requires MODULE_OBSERVER. Callback/context live for the entire module,
     * including logical disable. No unregister/replacement or hot unloading.
     * Notices run serially per owner, outside registry/configuration locks, after save success
     * OR failure. Read the current snapshot (later saves may already be pending).
     * Only update plugin-owned data or signal work; no game calls or waits, and
     * no exceptions crossing the ABI. Faulted owners receive no further notices. */
    APH_Result (APH_CALL *set_notifier)(APH_Owner, APH_ConfigNotifyFn, void* user_data);
} APH_ConfigApi;
static inline const char* APH_ResultName(APH_Result r) {
    switch(r) {
    case APH_OK:return "OK";case APH_PENDING:return "Pending";
    case APH_E_ARGUMENT:return "Invalid argument";case APH_E_ABI:return "ABI mismatch";
    case APH_E_UNAVAILABLE:return "Unavailable";case APH_E_BUSY:return "Busy";
    case APH_E_REVOKED:return "Revoked";case APH_E_QUEUE_FULL:return "Capacity reached";
    case APH_E_NEEDS_RESTART:return "Needs restart";case APH_E_FAULT:return "Callback fault";
    case APH_E_BACKEND:return "Hook backend failure";case APH_E_SIGNATURE:return "Signature mismatch";
    case APH_E_MODE:return "Hook mode conflict";case APH_E_CONFLICT:return "Resource conflict";
    case APH_E_KEY_REUSED:return "Request key reused";case APH_E_BUILD:return "Module build mismatch";
    case APH_E_FOREIGN_PATCH:return "Foreign hook conflict";case APH_E_CAPABILITY:return "Capability missing";
    case APH_E_DUPLICATE:return "Duplicate ID";case APH_E_DEPENDENCY:return "Dependency unavailable";
    case APH_E_VERSION:return "Version mismatch";case APH_E_CYCLE:return "Dependency cycle";
    case APH_E_IO:return "Configuration I/O failure";
    case APH_E_CONTRACT:return "Contract mismatch";case APH_E_STALE:return "Stale epoch or handle";
    case APH_E_THREAD:return "Wrong thread";case APH_E_EXPECTED:return "Expected value mismatch";
    default:return "Unknown result";
    }
}
static inline int APH_OwnerHasFault(uint32_t state) {
    return state==APH_OWNER_FAILED||state==APH_OWNER_FAULTED||state==APH_OWNER_NEEDS_RESTART;
}
#pragma pack(pop)
#ifdef __cplusplus
}
static_assert(sizeof(void*) == 8 && offsetof(APH_Api, copy_service) == 16, "Windows x64 ABI");
static_assert(sizeof(APH_Manifest) == 160, "Manifest ABI");
#else
_Static_assert(sizeof(void*) == 8 && offsetof(APH_Api, copy_service) == 16, "Windows x64 ABI");
_Static_assert(sizeof(APH_Manifest) == 160, "Manifest ABI");
#endif
#endif
