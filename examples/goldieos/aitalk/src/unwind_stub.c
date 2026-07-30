/**
 * @file unwind_stub.c
 * @brief _Unwind_* stubs for the WS63 musl toolchain link.
 *
 * The bundled riscv32 toolchain ships only soft-float libgcc variants,
 * while goldieos builds with -mabi=ilp32f. musl's libstdc++
 * (vterminate/eh_catch/eh_throw etc.) references _Unwind_* symbols,
 * but goldieos compiles with -fno-unwind-tables and never throws —
 * these stubs only satisfy the linker and are never executed.
 */
void _Unwind_Resume(void)               { for (;;) {} }
void _Unwind_RaiseException(void)       { for (;;) {} }
void _Unwind_ForcedUnwind(void)         { for (;;) {} }
void _Unwind_Backtrace(void)            { for (;;) {} }
void _Unwind_DeleteException(void)      { for (;;) {} }
void _Unwind_SetGR(void)                { for (;;) {} }
void _Unwind_SetIP(void)                { for (;;) {} }
void _Unwind_Resume_or_Rethrow(void)    { for (;;) {} }

long _Unwind_GetGR(void)                { return 0; }
long _Unwind_GetIP(void)                { return 0; }
long _Unwind_GetIPInfo(void)            { return 0; }
long _Unwind_GetRegionStart(void)       { return 0; }
long _Unwind_GetTextRelBase(void)       { return 0; }
long _Unwind_GetDataRelBase(void)       { return 0; }
long _Unwind_GetLanguageSpecificData(void) { return 0; }
