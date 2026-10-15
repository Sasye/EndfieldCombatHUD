#pragma once
// ============================================================================
// Il2Cpp Runtime API — function declarations and helper utilities
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <initializer_list>

// Forward decl for Log (defined in main cpp)
void Log(const char *fmt, ...);

// ============================================================================
// Il2Cpp function pointer declarations
// ============================================================================
template<class Signature> struct HudRuntimeFunction;
template<class R, class... Args> struct HudRuntimeFunction<R(*)(Args...)> {
  using Pointer = R(*)(Args...);
  Pointer pointer = nullptr;
  HudRuntimeFunction& operator=(Pointer value) { pointer = value; return *this; }
  explicit operator bool() const { return pointer != nullptr; }
  R operator()(Args... args) const {
    // Also gate metadata loops and lazy reflection after a stop notification.
    // Attaching a worker does not permit it to invoke arbitrary game methods.
    if (!pointer || !RuntimeOpen()) {
      if constexpr (std::is_void_v<R>) return;
      else return R{};
    }
    return pointer(args...);
  }
};
#define D(ret, name, ...)                                                      \
  typedef ret (*t_##name)(__VA_ARGS__);                                        \
  static HudRuntimeFunction<t_##name> name
D(void *, il2cpp_get_corlib);
D(void **, il2cpp_thread_get_all_attached_threads, size_t *);
D(void *, il2cpp_domain_get);
D(void *, il2cpp_thread_attach, void *);
D(void, il2cpp_thread_detach, void *);
D(void **, il2cpp_domain_get_assemblies, void *, size_t *);
D(void *, il2cpp_assembly_get_image, void *);
D(const char *, il2cpp_image_get_name, void *);
D(size_t, il2cpp_image_get_class_count, void *);
D(void *, il2cpp_image_get_class, void *, size_t);
D(void *, il2cpp_class_get_methods, void *, void **);
D(const char *, il2cpp_method_get_name, void *);
D(uint32_t, il2cpp_method_get_param_count, void *);
D(const char *, il2cpp_class_get_name, void *);
D(const char *, il2cpp_class_get_namespace, void *);
D(void *, il2cpp_object_get_class, void *);
D(void *, il2cpp_class_from_name, void *, const char *, const char *);
D(void *, il2cpp_method_get_param, void *, uint32_t);
D(const char *, il2cpp_type_get_name, void *);
D(void, il2cpp_free, void *);
D(int, il2cpp_type_get_type, void *);
D(bool, il2cpp_type_is_byref, void *);
D(void *, il2cpp_class_from_type, void *);
D(bool, il2cpp_class_is_valuetype, void *);
D(bool, il2cpp_class_is_enum, void *);
D(int32_t, il2cpp_class_value_size, void *, uint32_t *);
D(void *, il2cpp_class_enum_basetype, void *);
D(uint32_t, il2cpp_method_get_flags, void *, uint32_t *);
D(void *, il2cpp_method_get_return_type, void *);
D(bool, il2cpp_method_is_generic, void *);
D(bool, il2cpp_method_is_inflated, void *);
D(void *, il2cpp_class_get_fields, void *, void **);
D(const char *, il2cpp_field_get_name, void *);
D(size_t, il2cpp_field_get_offset, void *);
D(int, il2cpp_field_get_flags, void *);
D(void *, il2cpp_class_get_method_from_name, void *, const char *, int);
D(void *, il2cpp_runtime_invoke, void *, void *, void **, void **);
D(void *, il2cpp_class_get_parent, void *);
D(void, il2cpp_field_static_get_value, void *, void *);
#undef D

// ============================================================================
// Helper types and functions
// ============================================================================
static HMODULE hGA = nullptr;
struct MInfo {
  void *mp;
};

static int ReadStr(void *s, char *b, int sz) {
  __try {
    if (!s) {
      b[0] = 0;
      return -1;
    }
    int32_t l = *(int32_t *)((char *)s + 16);
    if (l <= 0 || l > 2048) {
      b[0] = 0;
      return -1;
    }
    wchar_t *c = (wchar_t *)((char *)s + 20);
    int j = 0;
    for (int i = 0; i < l && j < sz - 1; i++)
      b[j++] = (c[i] < 128) ? (char)c[i] : '?';
    b[j] = 0;
    return j;
  } __except (1) {
    b[0] = 0;
    return -2;
  }
}

static bool Resolve() {
  hGA = GetModuleHandleW(L"GameAssembly.dll");
  if (!hGA)
    return false;
#define R(n) n = (t_##n)GetProcAddress(hGA, #n); \
  if (!n) { Log("[APH] Missing required IL2CPP export: %s", #n); return false; }
  R(il2cpp_get_corlib);
  R(il2cpp_thread_get_all_attached_threads);
  R(il2cpp_domain_get);
  R(il2cpp_thread_attach);
  R(il2cpp_thread_detach);
  R(il2cpp_domain_get_assemblies);
  R(il2cpp_assembly_get_image);
  R(il2cpp_image_get_name);
  R(il2cpp_image_get_class_count);
  R(il2cpp_image_get_class);
  R(il2cpp_class_get_methods);
  R(il2cpp_method_get_name);
  R(il2cpp_method_get_param_count);
  R(il2cpp_class_get_name);
  R(il2cpp_class_get_namespace);
  R(il2cpp_object_get_class);
  R(il2cpp_class_from_name);
  R(il2cpp_method_get_param);
  R(il2cpp_type_get_name);
  R(il2cpp_free);
  R(il2cpp_type_get_type);
  R(il2cpp_type_is_byref);
  R(il2cpp_class_from_type);
  R(il2cpp_class_is_valuetype);
  R(il2cpp_class_is_enum);
  R(il2cpp_class_value_size);
  R(il2cpp_class_enum_basetype);
  R(il2cpp_method_get_flags);
  R(il2cpp_method_get_return_type);
  R(il2cpp_method_is_generic);
  R(il2cpp_method_is_inflated);
  R(il2cpp_class_get_fields);
  R(il2cpp_field_get_name);
  R(il2cpp_field_get_offset);
  R(il2cpp_field_get_flags);
  R(il2cpp_runtime_invoke);
  R(il2cpp_class_get_parent);
  R(il2cpp_field_static_get_value);
#undef R
  return il2cpp_domain_get && il2cpp_class_get_methods &&
         il2cpp_method_get_name;
}

static bool WorkerAttachReady() {
  // Verified against GameAssembly startup (PE timestamp 0x6A870DA4).
  // get_corlib is a non-allocating pointer read. Corlib is
  // published only AFTER GC and the native attached-thread list initialize.
  // Domain::GetCurrent itself can allocate; do not use it as the first probe.
  if (!il2cpp_get_corlib || !il2cpp_thread_get_all_attached_threads ||
      !il2cpp_get_corlib()) return false;
  size_t count = 0;
  // A registered VM thread also proves the domain / Thread classes exist.
  // This mutable array is borrowed: only inspect its count, never its entries.
  return il2cpp_thread_get_all_attached_threads(&count) && count != 0;
}

static void *FindMethod(void *k, const char *n, int pc) {
  if (!k)
    return nullptr;
  void *it = nullptr, *m;
  while ((m = il2cpp_class_get_methods(k, &it))) {
    const char *mn = il2cpp_method_get_name(m);
    if (mn && strcmp(mn, n) == 0 && (int)il2cpp_method_get_param_count(m) == pc)
      return m;
  }
  return nullptr;
}

static bool TypeMatches(void* type, const char* expected) {
  if (!type || il2cpp_type_is_byref(type)) return false;
  const char* allocated = il2cpp_type_get_name(type);
  if (!allocated) return false;
  char name[256]{};
  size_t n = 0;
  for (const char* p = allocated; *p && n + 1 < sizeof(name); ++p) {
    if (*p != ' ') name[n++] = (*p == '/' || *p == '+') ? '.' : *p;
  }
  il2cpp_free((void*)allocated);
  if (strcmp(name, expected)) return false;
  int kind = il2cpp_type_get_type(type);
  if (!strcmp(expected, "System.Void")) return kind == 1;
  if (!strcmp(expected, "System.Boolean")) return kind == 2;
  if (!strcmp(expected, "System.Int32")) return kind == 8;
  if (!strcmp(expected, "System.Single")) return kind == 12;
  void* klass = il2cpp_class_from_type(type);
  if (!klass) return false;
  if (!strcmp(expected, "Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>")) {
    uint32_t alignment = 0;
    return kind == 21 && il2cpp_class_is_valuetype(klass) &&
        il2cpp_class_value_size(klass, &alignment) == sizeof(BuffRef) && alignment == 8;
  }
  if (!strcmp(expected, "Beyond.Gameplay.Core.Buff.FinishReason"))
    return il2cpp_class_is_enum(klass) &&
        il2cpp_type_get_type(il2cpp_class_enum_basetype(klass)) == 8;
  return !il2cpp_class_is_valuetype(klass);
}

static bool ValidateMethod(void* method, const char* result,
    std::initializer_list<const char*> parameters, bool isStatic = false) {
  if (!method || !RuntimeOpen() || !((MInfo*)method)->mp ||
      il2cpp_method_is_generic(method) || il2cpp_method_is_inflated(method) ||
      il2cpp_method_get_param_count(method) != parameters.size()) return false;
  uint32_t impl = 0;
  if (bool(il2cpp_method_get_flags(method, &impl) & 0x10) != isStatic ||
      !TypeMatches(il2cpp_method_get_return_type(method), result)) return false;
  uint32_t index = 0;
  for (const char* expected : parameters)
    if (!TypeMatches(il2cpp_method_get_param(method, index++), expected)) return false;
  return true;
}

template<class H, class Fn> static bool QueueHudHook(void* method, uint64_t key,
    const char* name, Fn body, const char* signature,
    std::initializer_list<const char*> parameters) {
  if (!ValidateMethod(method, "System.Void", parameters)) {
    Log("[ABI] %s: missing method or incompatible signature", name);
    return false;
  }
  APH_Result r = H::Request(g_client, key, (Fn)((MInfo*)method)->mp, body,
      signature, name, HudOptions());
  if (r >= 0) r = AwaitHud(g_epoch, [] { return H::Advance(); });
  Log("[APH] %s: %d", name, r);
  return r == APH_OK;
}

static void *FindClass(const char *ns, const char *n, void **a, size_t c) {
  for (size_t i = 0; i < c; i++) {
    if (!RuntimeOpen() || WaitForSingleObject(g_cancel, 0) != WAIT_TIMEOUT) return nullptr;
    void *img = il2cpp_assembly_get_image(a[i]);
    if (!img)
      continue;
    // Exact namespace/type lookup avoids scanning every class in the game.
    if (void* klass = il2cpp_class_from_name(img, ns, n)) return klass;
  }
  return nullptr;
}

static void *Invoke(void *method, void *obj, void **params = nullptr) {
  if (!method || !GameReadAllowed())
    return nullptr;
  __try {
    void *exc = nullptr;
    void* result = il2cpp_runtime_invoke(method, obj, params, &exc);
    return exc ? nullptr : result;
  } __except (1) {
    return nullptr;
  }
}

// ============================================================================
// Dynamic field offset resolution (from EIEM)
// ============================================================================

// Find a field by name, walking up the inheritance chain.
// Returns the field offset or -1 if not found.
static int FindFieldInHierarchy(void *klass, const char **names, int nameCount,
                                const char **outFieldName = nullptr) {
  if (!klass || !il2cpp_class_get_parent)
    return -1;
  void *cur = klass;
  int depth = 0;
  while (cur && depth < 10) {
    void *it = nullptr, *f;
    while ((f = il2cpp_class_get_fields(cur, &it))) {
      const char *fn = il2cpp_field_get_name(f);
      if (!fn)
        continue;
      for (int i = 0; i < nameCount; i++) {
        if (strcmp(fn, names[i]) == 0) {
          if (outFieldName)
            *outFieldName = fn;
          return (int)il2cpp_field_get_offset(f);
        }
      }
    }
    cur = il2cpp_class_get_parent(cur);
    depth++;
  }
  return -1;
}

// Convenience: single field name lookup
static int FindField(void *klass, const char *name) {
  const char *names[] = {name};
  return FindFieldInHierarchy(klass, names, 1);
}

// Dump all fields in the entire hierarchy (for diagnostics)
static void DumpFieldsHierarchy(void *klass) {
  if (!klass || !il2cpp_class_get_parent)
    return;
  void *cur = klass;
  int depth = 0;
  while (cur && depth < 10) {
    const char *cn = il2cpp_class_get_name(cur);
    Log("[DUMP]   --- %s ---", cn ? cn : "?");
    void *it = nullptr, *f;
    while ((f = il2cpp_class_get_fields(cur, &it))) {
      const char *fn = il2cpp_field_get_name(f);
      int fo = (int)il2cpp_field_get_offset(f);
      if (fn)
        Log("[DUMP]   [0x%X] %s", fo, fn);
    }
    cur = il2cpp_class_get_parent(cur);
    depth++;
  }
}
