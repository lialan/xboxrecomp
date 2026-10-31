/* recomp_rc_off.h -- the end of a --reg-cache function: the register names
 * back to the thread-local block (see recomp_rc_on.h). */
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esi
#undef edi
#undef esp
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define esp g_esp
#undef g_eax
#undef g_ecx
#undef g_edx
#undef g_ebx
#undef g_esi
#undef g_edi
#undef g_esp
#define g_eax g_regs.r_eax
#define g_ecx g_regs.r_ecx
#define g_edx g_regs.r_edx
#define g_ebx g_regs.r_ebx
#define g_esi g_regs.r_esi
#define g_edi g_regs.r_edi
#define g_esp g_regs.r_esp
#undef RC_EAX
#undef RC_ECX
#undef RC_EDX
#undef RC_EBX
#undef RC_ESI
#undef RC_EDI
#undef RC_ESP
#undef RC_ST_EAX
#undef RC_ST_ECX
#undef RC_ST_EDX
#undef RC_ST_EBX
#undef RC_ST_ESI
#undef RC_ST_EDI
#undef RC_ST_ESP
#undef RC_LD_EAX
#undef RC_LD_ECX
#undef RC_LD_EDX
#undef RC_LD_EBX
#undef RC_LD_ESI
#undef RC_LD_EDI
#undef RC_LD_ESP
#undef RECOMP_RC_STORE
#undef RECOMP_RC_LOAD
#undef RECOMP_ABI_CALL
#define RECOMP_ABI_CALL(va, fn) RECOMP_ABI_CALL_BASE(va, fn)
