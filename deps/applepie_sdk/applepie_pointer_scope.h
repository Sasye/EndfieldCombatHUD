#pragma once
#include "applepie.h"
#include <windows.h>
typedef void (APH_CALL *APH_PointerBody)(void*);
static inline LONG APH_PointerFilter(const APH_PointerApi* api,APH_Owner owner,APH_Patch patch,EXCEPTION_POINTERS* ex) {
    api->wrapper_fault(owner,patch,ex->ExceptionRecord->ExceptionCode,(uint64_t)(uintptr_t)ex->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* POD-only boundary. The body is the original managed/native wrapper, never a
 * generic replacement invocation. No SEH retry or fabricated wrapper success. */
static inline APH_Result APH_WithPointerPatch(const APH_PointerApi* api,APH_Owner owner,
    const APH_PointerRequest* request,APH_PointerBody body,void* context) {
    APH_Patch patch=0;
    APH_Result result=api->request(owner,request,&patch);
    if(result!=APH_OK)return result;
    __try {
        __try {body(context);}
        __except(APH_PointerFilter(api,owner,patch,GetExceptionInformation())) {}
    } __finally {result=api->release(owner,patch);}
    return result;
}
