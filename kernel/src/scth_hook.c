#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/string.h>

#include <asm/processor-flags.h>
#include <asm/special_insns.h>

#include "scth_internal.h"

typedef asmlinkage long (*scth_sys_fn_t)(const struct pt_regs *regs);

static asmlinkage long scth_syscall_wrapper(const struct pt_regs *regs);

static scth_sys_fn_t *scth_syscall_table(void)
{
    if (!g_scth.sys_call_table_addr)
        return NULL;
    return (scth_sys_fn_t *)(unsigned long)g_scth.sys_call_table_addr;
}

static scth_sys_fn_t scth_orig[NR_syscalls];
static bool scth_hooked[NR_syscalls];

/* ---- WP/CET safe patch context ---- */
struct scth_wp_ctx {
    unsigned long cr0;
    unsigned long cr4;
    unsigned long irqflags;
};

/* Forced CR writes: bypass native_write_cr0() checks */
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

static __always_inline void scth_wp_off(struct scth_wp_ctx *ctx)
{
    preempt_disable();
    local_irq_save(ctx->irqflags);

    ctx->cr0 = read_cr0();
    ctx->cr4 = __read_cr4();

#ifdef X86_CR4_CET
    if (ctx->cr4 & X86_CR4_CET)
        write_cr4_forced(ctx->cr4 & ~X86_CR4_CET);
#endif

    write_cr0_forced(ctx->cr0 & ~X86_CR0_WP);
}

static __always_inline void scth_wp_on(struct scth_wp_ctx *ctx)
{
    write_cr0_forced(ctx->cr0);

#ifdef X86_CR4_CET
    if (ctx->cr4 & X86_CR4_CET)
        write_cr4_forced(ctx->cr4);
#endif

    local_irq_restore(ctx->irqflags);
    preempt_enable();
}

/* ---- blocked threads accounting ---- */
static __always_inline void scth_blocked_inc(void)
{
    unsigned long flags;
    spin_lock_irqsave(&g_scth.lock, flags);
    g_scth.current_blocked_threads++;
    if (g_scth.current_blocked_threads > g_scth.peak_blocked_threads)
        g_scth.peak_blocked_threads = g_scth.current_blocked_threads;
    spin_unlock_irqrestore(&g_scth.lock, flags);
}

static __always_inline void scth_blocked_dec(void)
{
    unsigned long flags;
    spin_lock_irqsave(&g_scth.lock, flags);
    if (g_scth.current_blocked_threads)
        g_scth.current_blocked_threads--;
    spin_unlock_irqrestore(&g_scth.lock, flags);
}

/* ---- FILTER: prog OR uid (usa funzioni che esistono: has_*) ---- */
static bool scth_match_prog_or_uid(const char comm[SCTH_COMM_LEN], u32 euid)
{
    bool match_prog, match_uid;

    mutex_lock(&g_scth.cfg_mutex);
    match_prog = scth_cfg_has_prog(&g_scth.cfg, comm);
    match_uid  = scth_cfg_has_uid(&g_scth.cfg, euid);
    mutex_unlock(&g_scth.cfg_mutex);

    return match_prog || match_uid;
}

/* ---- M3: wait until next epoch ---- */
static __always_inline int scth_wait_next_epoch(u64 epoch0)
{
    long ret = wait_event_interruptible(
        g_scth.epoch_wq,
        READ_ONCE(g_scth.epoch_id) != epoch0 || !READ_ONCE(g_scth.monitor_on)
    );

    if (ret < 0)
        return -EINTR;

    return 0;
}

/* wrapper M3 */
static asmlinkage long scth_syscall_wrapper(const struct pt_regs *regs)
{
    u32 nr = (u32)regs->orig_ax;
    scth_sys_fn_t orig;
    char comm[SCTH_COMM_LEN];
    u32 euid;

    if (nr >= NR_syscalls)
        return -ENOSYS;

    orig = READ_ONCE(scth_orig[nr]);
    if (!orig)
        return -ENOSYS;

    /* monitor off => bypass immediato */
    if (!READ_ONCE(g_scth.monitor_on))
        return orig(regs);

    get_task_comm(comm, current);
    euid = (u32)from_kuid(&init_user_ns, current_euid());

    /* se NON matcha (prog OR uid) => nessun delay */
    if (!scth_match_prog_or_uid(comm, euid))
        return orig(regs);

    /* delay fino alla prossima epoca */
    {
        u64 e0 = READ_ONCE(g_scth.epoch_id);

        scth_blocked_inc();
        if (scth_wait_next_epoch(e0) != 0) {
            scth_blocked_dec();
            return -EINTR;
        }
        scth_blocked_dec();

        pr_info_ratelimited("scthrottle: delayed nr=%u comm=%s euid=%u\n",
                            nr, comm, euid);
    }

    return orig(regs);
}

int scth_hook_install(u32 nr)
{
    scth_sys_fn_t *table;
    scth_sys_fn_t cur;
    struct scth_wp_ctx ctx;

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

    scth_wp_off(&ctx);
    WRITE_ONCE(table[nr], scth_syscall_wrapper);
    scth_wp_on(&ctx);

    WRITE_ONCE(scth_hooked[nr], true);
    pr_info("scthrottle: hooked sys_call_table[%u]\n", nr);
    return 0;
}

int scth_hook_remove(u32 nr)
{
    scth_sys_fn_t *table;
    scth_sys_fn_t orig;
    struct scth_wp_ctx ctx;

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

    scth_wp_off(&ctx);
    WRITE_ONCE(table[nr], orig);
    scth_wp_on(&ctx);

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