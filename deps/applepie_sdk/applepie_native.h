#pragma once
// C++17 client-side, compile-time typed entry. Each <Tag, actual function type>
// instantiation has its own machine-code entry and original slot. No runtime
// signature interpretation, varargs dispatch, assembly thunk or ABI guessing.
#include "applepie.h"
#include <windows.h>
#include <intrin.h>
#include <type_traits>
struct APH_NativeAdmission {APH_WriteCall writes[2];uint32_t count,reserved;};
struct APH_NativeBinding {
    APH_Owner owner;
    const APH_HookApi* api;
    APH_Hook hook;
    APH_NativeProc original;
    uint32_t domain,access_flags;
    volatile LONG ready;
    bool module_observer; // audited resident observation; never an implicit write grant
    bool (*begin)(APH_NativeBinding*,APH_NativeAdmission*);
    void (*end)(APH_NativeAdmission*);
};
struct APH_NativeFrame {
    APH_NativeBinding* binding;
    APH_NativeFrame* previous;
    void* caller;
    uint32_t phase,calls;
    bool original_unwinding;
};
inline thread_local APH_NativeFrame* APH_native_frame=nullptr;
static inline void* APH_NativeCaller(){return APH_native_frame?APH_native_frame->caller:nullptr;}
static inline bool APH_NativeOriginalUnwinding(){
    for(auto* f=APH_native_frame;f;f=f->previous)if(f->original_unwinding)return true;
    return false;
}
static inline LONG APH_NativeFault(APH_NativeFrame* frame,EXCEPTION_POINTERS* e,bool original) {
    if(original)frame->original_unwinding=true;
    auto* b=frame->binding;
    b->api->record_fault(b->owner,b->hook,e->ExceptionRecord->ExceptionCode,
        (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress,frame->phase,0);
    return EXCEPTION_CONTINUE_SEARCH;
}
template<unsigned Tag,class Signature> struct APH_TypedNative;
template<unsigned Tag,class R,class... A> struct APH_TypedNative<Tag,R(*)(A...)> {
    using Function=R(*)(A...);
    inline static APH_NativeBinding binding{};
    inline static Function business=nullptr;
    static R raw(A... a) {return ((Function)binding.original)(a...);}
    static R Original(A... a) {
        APH_NativeFrame* f=APH_native_frame;
        if(!f||f->binding!=&binding)return Run(false,nullptr,a...);
        if(++f->calls!=1)RaiseException(0xe0412001u,EXCEPTION_NONCONTINUABLE,0,nullptr);
        f->phase=APH_ORIGINAL_RUNNING;
        __try {
            if constexpr(std::is_void_v<R>){raw(a...);f->phase=APH_ORIGINAL_RETURNED;return;}
            else {R result=raw(a...);f->phase=APH_ORIGINAL_RETURNED;return result;}
        } __except(APH_NativeFault(f,GetExceptionInformation(),true)){}
        if constexpr(!std::is_void_v<R>)return R{}; // unreachable: filter always searches
    }
    static R Run(bool allow_business,void* caller,A... a) {
        APH_HookCall call{};APH_NativeAdmission admission{};
        const APH_Result entered=binding.api->enter_call(binding.owner,binding.hook,&call);
        APH_NativeFrame frame{&binding,APH_native_frame,caller,APH_ORIGINAL_NOT_ENTERED,0,false};
        APH_native_frame=&frame;bool admitted=false;
        __try {
            __try {
                admitted=allow_business&&entered==APH_OK&&
                    ((call.flags&APH_CALL_BUSINESS)||(binding.module_observer&&(call.flags&APH_CALL_NATIVE_READY)&&!(call.flags&APH_CALL_RECURSIVE)))&&
                    InterlockedCompareExchange(&binding.ready,0,0)&&(!binding.begin||binding.begin(&binding,&admission));
                if(admitted)return business(a...);
                return Original(a...);
            } __except(APH_NativeFault(&frame,GetExceptionInformation(),false)){}
        } __finally {
            if(binding.end&&(admitted||admission.count))binding.end(&admission);
            APH_native_frame=frame.previous;
            if(entered==APH_OK)binding.api->leave_call(binding.owner,&call);
        }
        if constexpr(!std::is_void_v<R>)return R{};
    }
    __declspec(noinline) static R Entry(A... a){return Run(true,_ReturnAddress(),a...);}
};
