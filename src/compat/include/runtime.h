#pragma once
// PC_PORT shim for the Metrowerks <runtime.h> (DOL runtime helpers, declared
// in third_party/petari/libs/Runtime/include/runtime.h). Vendored code may
// include it and call the conversion helpers; the asm bodies are provided
// host-side (compat/os/PPCIntrinsics.cpp).

#ifdef __cplusplus
extern "C" {
#endif

// PPC cvt double -> unsigned long long (MWERKS runtime asm). Used by
// MainLoopFramework::waitDrawDoneAndSetAlarm (GX watchdog interval).
unsigned long long __cvt_dbl_usll(double);

// PPC cvt double -> unsigned int.
unsigned int __cvt_fp2unsigned(double);

#ifdef __cplusplus
}
#endif
