/* recomp_rc_on.h -- included INSIDE a lifted function by code lifted with
 * --reg-cache, after the function has declared a local rc_<reg> for each
 * guest register it uses and defined RC_<REG> for it (the lifter emits
 * both). No include guard: it is included once per function.
 *
 * Why: the guest registers live in one thread-local block (g_regs), and
 * every guest memory store goes through a volatile uint32_t pointer that
 * the compiler must assume may alias it -- so each register was reloaded
 * from the block after every store. Held in locals, they stay in host
 * registers. The block is brought up to date wherever anything else can
 * look at it -- before every call (direct, indirect, tail, kernel), at
 * every return, around recomp_debug_service -- and the locals reloaded
 * after every call. Guest memory stays volatile; only register traffic
 * changes. Registers the function never names are not cached, so a macro
 * that touches them (an indirect call's failure path setting eax) reaches
 * the block itself.
 *
 * Within the function both spellings -- the generated code's short names
 * (eax) and the g_ names some macros use (g_esp) -- mean the local.
 * recomp_rc_off.h, before the closing brace, restores them. */

#ifdef RC_EAX
#undef eax
#define eax rc_eax
#undef g_eax
#define g_eax rc_eax
#define RC_ST_EAX g_regs.r_eax = rc_eax;
#define RC_LD_EAX rc_eax = g_regs.r_eax;
#else
#define RC_ST_EAX
#define RC_LD_EAX
#endif
#ifdef RC_ECX
#undef ecx
#define ecx rc_ecx
#undef g_ecx
#define g_ecx rc_ecx
#define RC_ST_ECX g_regs.r_ecx = rc_ecx;
#define RC_LD_ECX rc_ecx = g_regs.r_ecx;
#else
#define RC_ST_ECX
#define RC_LD_ECX
#endif
#ifdef RC_EDX
#undef edx
#define edx rc_edx
#undef g_edx
#define g_edx rc_edx
#define RC_ST_EDX g_regs.r_edx = rc_edx;
#define RC_LD_EDX rc_edx = g_regs.r_edx;
#else
#define RC_ST_EDX
#define RC_LD_EDX
#endif
#ifdef RC_EBX
#undef ebx
#define ebx rc_ebx
#undef g_ebx
#define g_ebx rc_ebx
#define RC_ST_EBX g_regs.r_ebx = rc_ebx;
#define RC_LD_EBX rc_ebx = g_regs.r_ebx;
#else
#define RC_ST_EBX
#define RC_LD_EBX
#endif
#ifdef RC_ESI
#undef esi
#define esi rc_esi
#undef g_esi
#define g_esi rc_esi
#define RC_ST_ESI g_regs.r_esi = rc_esi;
#define RC_LD_ESI rc_esi = g_regs.r_esi;
#else
#define RC_ST_ESI
#define RC_LD_ESI
#endif
#ifdef RC_EDI
#undef edi
#define edi rc_edi
#undef g_edi
#define g_edi rc_edi
#define RC_ST_EDI g_regs.r_edi = rc_edi;
#define RC_LD_EDI rc_edi = g_regs.r_edi;
#else
#define RC_ST_EDI
#define RC_LD_EDI
#endif
#ifdef RC_ESP
#undef esp
#define esp rc_esp
#undef g_esp
#define g_esp rc_esp
#define RC_ST_ESP g_regs.r_esp = rc_esp;
#define RC_LD_ESP rc_esp = g_regs.r_esp;
#else
#define RC_ST_ESP
#define RC_LD_ESP
#endif

#define RECOMP_RC_STORE() do { RC_ST_EAX RC_ST_ECX RC_ST_EDX RC_ST_EBX \
                               RC_ST_ESI RC_ST_EDI RC_ST_ESP } while (0)
#define RECOMP_RC_LOAD()  do { RC_LD_EAX RC_LD_ECX RC_LD_EDX RC_LD_EBX \
                               RC_LD_ESI RC_LD_EDI RC_LD_ESP } while (0)

/* Every call macro (RECOMP_ICALL*, RECOMP_ITAIL) calls through this. */
#undef RECOMP_ABI_CALL
#define RECOMP_ABI_CALL(va, fn) do { RECOMP_RC_STORE(); RECOMP_ABI_CALL_BASE(va, fn); \
                                     RECOMP_RC_LOAD(); } while (0)
