#pragma once

#include_next <fenv.h>

#if defined(__APPLE__) && defined(__MACH__)

// Public domain polyfill for feenableexcept on OS X
// http://www-personal.umich.edu/~williams/archive/computation/fe-handling-example.c
//
// fenv_t's layout differs between Intel and Apple Silicon Macs:
//   x86_64: fenv_t has __control (x87 control word) and __mxcsr (SSE control/status)
//   arm64:  fenv_t has __fpcr and __fpsr (ARM FPCR/FPSR registers)
// so the exception-mask bit manipulation is implemented separately per architecture.

#if defined(__x86_64__)

inline int feenableexcept(unsigned int excepts)
{
    static fenv_t fenv;
    unsigned int new_excepts = excepts & FE_ALL_EXCEPT;
    // previous masks
    unsigned int old_excepts;

    if (fegetenv(&fenv)) {
        return -1;
    }
    old_excepts = fenv.__control & FE_ALL_EXCEPT;

    // unmask
    fenv.__control &= ~new_excepts;
    fenv.__mxcsr   &= ~(new_excepts << 7);

    return fesetenv(&fenv) ? -1 : old_excepts;
}

inline int fedisableexcept(unsigned int excepts)
{
    static fenv_t fenv;
    unsigned int new_excepts = excepts & FE_ALL_EXCEPT;
    // all previous masks
    unsigned int old_excepts;

    if (fegetenv(&fenv)) {
        return -1;
    }
    old_excepts = fenv.__control & FE_ALL_EXCEPT;

    // mask
    fenv.__control |= new_excepts;
    fenv.__mxcsr   |= new_excepts << 7;

    return fesetenv(&fenv) ? -1 : old_excepts;
}

#elif defined(__arm64__) || defined(__aarch64__)

// On ARM, FE_* exception bits (FE_ALL_EXCEPT = 0x1f) map directly to the
// low-order exception-flag bits of FPSR, and to the trap-enable bits of
// FPCR shifted left by 8.
#define __LMMS_FPCR_EXCEPT_SHIFT 8

inline int feenableexcept(unsigned int excepts)
{
    static fenv_t fenv;
    unsigned int new_excepts = excepts & FE_ALL_EXCEPT;
    unsigned int old_excepts;

    if (fegetenv(&fenv)) {
        return -1;
    }
    old_excepts = (fenv.__fpcr >> __LMMS_FPCR_EXCEPT_SHIFT) & FE_ALL_EXCEPT;

    // unmask (enable) the requested trap bits
    fenv.__fpcr |= (new_excepts << __LMMS_FPCR_EXCEPT_SHIFT);

    return fesetenv(&fenv) ? -1 : old_excepts;
}

inline int fedisableexcept(unsigned int excepts)
{
    static fenv_t fenv;
    unsigned int new_excepts = excepts & FE_ALL_EXCEPT;
    unsigned int old_excepts;

    if (fegetenv(&fenv)) {
        return -1;
    }
    old_excepts = (fenv.__fpcr >> __LMMS_FPCR_EXCEPT_SHIFT) & FE_ALL_EXCEPT;

    // mask (disable) the requested trap bits
    fenv.__fpcr &= ~(new_excepts << __LMMS_FPCR_EXCEPT_SHIFT);

    return fesetenv(&fenv) ? -1 : old_excepts;
}

#undef __LMMS_FPCR_EXCEPT_SHIFT

#endif // architecture dispatch

#endif // defined(__APPLE__) && defined(__MACH__)