#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uidgid.h>

#include <asm/processor-flags.h>
#include <asm/special_insns.h>

#include "scth_internal.h"

typedef asmlinkage long (*scth_sys_fn_t)(const struct pt_regs *regs);

/* forward decl (to avoid -Wmissing-prototypes) */
asmlinkage long scth_syscall_wrapper(const struct pt_regs *regs);

static scth_sys_fn_t *scth_syscall_table(void)
{
    if (!g_scth.sys_call_table_addr)
        return NULL;
    return (scth_sys_fn_t *)(unsigned long)g_scth.sys_call_table_addr;
}

static scth_sys_fn_t scth_orig[NR_syscalls];
static bool scth_hooked[NR_syscalls];

static unsigned long scth_saved_cr0;
static unsigned long scth_saved_cr4;

/*
 * Forced CR writes: bypass native_write_cr0() checks.
 * This is what avoids the "CR0 WP bit went missing!?" path you hit.
 */
static __always_inline void write_cr0_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}

static __always_inline void write_cr4_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr4" : "+r"(val), "+m"(__force_order));
}

static __always_inline void conditional_cet_disable(void)
{
#ifdef X86_CR4_CET
    if (scth_saved_cr4 & X86_CR4_CET)
        write_cr4_forced(scth_saved_cr4 & ~X86_CR4_CET);
#endif
}

static __always_inline void conditional_cet_enable(void)
{
#ifdef X86_CR4_CET
    if (scth_saved_cr4 & X86_CR4_CET)
        write_cr4_forced(scth_saved_cr4);
#endif
}

/* enter "hack mode" to write into sys_call_table */
static __always_inline void scth_wp_off(void)
{
    preempt_disable();

    scth_saved_cr0 = read_cr0();
    scth_saved_cr4 = __read_cr4();

    conditional_cet_disable();

    /* force WP=0 (do NOT use native_write_cr0 path) */
    write_cr0_forced(scth_saved_cr0 & ~X86_CR0_WP);
}

/* exit "hack mode" */
static __always_inline void scth_wp_on(void)
{
    write_cr0_forced(scth_saved_cr0);
    conditional_cet_enable();

    preempt_enable();
}

/* wrapper M3: pass-through + log rate-limited */
asmlinkage long scth_syscall_wrapper(const struct pt_regs *regs)
{
    u32 nr = (u32)regs->orig_ax;
    scth_sys_fn_t orig;

    if (nr >= NR_syscalls)
        return -ENOSYS;

    orig = READ_ONCE(scth_orig[nr]);
    if (!orig)
        return -ENOSYS;

    if (READ_ONCE(g_scth.monitor_on)) {
        char comm[SCTH_COMM_LEN];
        u32 euid = (u32)from_kuid(&init_user_ns, current_euid());

        get_task_comm(comm, current);
        pr_info_ratelimited("scthrottle: wrapper hit nr=%u comm=%s euid=%u\n",
                            nr, comm, euid);
    }

    return orig(regs);
}

int scth_hook_install(u32 nr)
{
    scth_sys_fn_t *table;
    scth_sys_fn_t cur;

    if (nr >= NR_syscalls)
        return -EINVAL;

    table = scth_syscall_table();
    if (!table)
        return -EINVAL;

    if (READ_ONCE(scth_hooked[nr]))
        return -EEXIST;

    cur = READ_ONCE(table[nr]);

    if (cur == scth_syscall_wrapper) {
        WRITE_ONCE(scth_hooked[nr], true);
        return -EALREADY;
    }

    WRITE_ONCE(scth_orig[nr], cur);

    scth_wp_off();
    WRITE_ONCE(table[nr], scth_syscall_wrapper);
    scth_wp_on();

    WRITE_ONCE(scth_hooked[nr], true);
    pr_info("scthrottle: hooked sys_call_table[%u]\n", nr);

    return 0;
}

int scth_hook_remove(u32 nr)
{
    scth_sys_fn_t *table;
    scth_sys_fn_t orig;

    if (nr >= NR_syscalls)
        return -EINVAL;

    table = scth_syscall_table();
    if (!table)
        return -EINVAL;

    if (!READ_ONCE(scth_hooked[nr]))
        return -ENOENT;

    orig = READ_ONCE(scth_orig[nr]);
    if (!orig)
        return -EINVAL;

    scth_wp_off();
    WRITE_ONCE(table[nr], orig);
    scth_wp_on();

    WRITE_ONCE(scth_hooked[nr], false);
    pr_info("scthrottle: unhooked sys_call_table[%u]\n", nr);

    return 0;
}

void scth_hook_remove_all(void)
{
    u32 nr;

    for (nr = 0; nr < NR_syscalls; nr++) {
        if (READ_ONCE(scth_hooked[nr]))
            scth_hook_remove(nr);
    }
}