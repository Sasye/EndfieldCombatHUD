#ifndef APPLEPIE_SEH_H
#define APPLEPIE_SEH_H
#include "applepie.h"
#include <windows.h>
/* Use in a POD-only exact-signature entry. C++ destructors
 * under /EHsc are not a hardware-fault cleanup mechanism. Catch C++ exceptions
 * inside the plugin's own compatible C++ implementation, before this C boundary.
 * A read-only contract excludes writes, retained locks, acquired resources and
 * side effects. A write/execute AV, stack/heap damage or original fault propagates. */
static int APH_EntryFilter(const APH_HookApi* api, APH_Owner owner, APH_Hook hook,
                          EXCEPTION_POINTERS* e, uint32_t phase, uint32_t recovery) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    int recoverable = recovery == APH_RECOVERY_READ_ONLY &&
        phase != APH_ORIGINAL_RUNNING && r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        !(r->ExceptionFlags & EXCEPTION_NONCONTINUABLE) &&
        r->NumberParameters >= 2 && r->ExceptionInformation[0] == 0;
    api->record_fault(owner, hook, r->ExceptionCode, (uint64_t)(uintptr_t)r->ExceptionAddress,
                      phase, (uint32_t)recoverable);
    return recoverable ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
/* The client must validate its own exact native ABI separately. */
static int APH_ModuleBuild(APH_NativeProc target, uint32_t* timestamp, uint32_t* image_size) {
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &module)) return 0;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
        const IMAGE_NT_HEADERS64* pe = (const IMAGE_NT_HEADERS64*)((const char*)module + dos->e_lfanew);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || pe->Signature != IMAGE_NT_SIGNATURE) return 0;
        *timestamp = pe->FileHeader.TimeDateStamp;
        *image_size = pe->OptionalHeader.SizeOfImage;
        return 1;
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
               EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return 0; }
}
#endif
