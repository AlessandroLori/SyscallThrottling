#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/preempt.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/list.h>

#include <asm/processor.h>
#include <asm/processor-flags.h>
#include <asm/special_insns.h>

#include "scth_internal.h"

/* DEBUG: log dei GRANT per verificare FIFO_STRICT */
#define SCTH_DEBUG_GRANT 1
#if SCTH_DEBUG_GRANT
#define SCTH_LOG_GRANT(_tag, _epoch, _ticket, _used, _max, _qlen) \
    pr_info("scthrottle: GRANT[%s] epoch=%llu ticket=%llu used=%u/%u qlen=%u\n", \
            (_tag), (unsigned long long)(_epoch), (unsigned long long)(_ticket), \
            (unsigned int)(_used), (unsigned int)(_max), (unsigned int)(_qlen))
#else
#define SCTH_LOG_GRANT(...) do { } while (0)
#endif

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
    ctx->cr4 = native_read_cr4();

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

/* ---- cfg match: (prog OR uid) ---- */
static bool scth_match_prog_or_uid(const char comm[SCTH_COMM_LEN], u32 euid)
{
    bool match;

    mutex_lock(&g_scth.cfg_mutex);
    match = scth_cfg_has_prog(&g_scth.cfg, comm) || scth_cfg_has_uid(&g_scth.cfg, euid);
    mutex_unlock(&g_scth.cfg_mutex);

    return match;
}

/* ---- WAKE_RACE token acquire (per-epoch) ---- */
static __always_inline bool scth_try_take_token(void)
{
    int v;

    for (;;) {
        v = atomic_read(&g_scth.epoch_tokens);
        if (v <= 0)
            return false;
        if (atomic_cmpxchg(&g_scth.epoch_tokens, v, v - 1) == v)
            return true;
        cpu_relax();
    }
}

static asmlinkage long scth_syscall_wrapper(const struct pt_regs *regs)
{
    u32 nr = (u32)regs->orig_ax;
    scth_sys_fn_t orig;

    char comm[SCTH_COMM_LEN];
    u32 euid;
    u32 policy;

    if (nr >= NR_syscalls)
        return -ENOSYS;

    orig = READ_ONCE(scth_orig[nr]);
    if (!orig)
        return -ENOSYS;

    if (!READ_ONCE(g_scth.monitor_on))
        return orig(regs);

    get_task_comm(comm, current);
    euid = (u32)from_kuid(&init_user_ns, current_euid());

    if (!scth_match_prog_or_uid(comm, euid))
        return orig(regs);

    policy = READ_ONCE(g_scth.policy_active);

    /* ---------------- WAKE_RACE ---------------- */
    if (policy == SCTH_POLICY_WAKE_RACE) {
        u64 t0 = ktime_get_ns();
        bool waited = false;
        u64 epoch0 = READ_ONCE(g_scth.epoch_id);

        if (scth_try_take_token())
            return orig(regs);

        waited = true;

        for (;;) {
            long ret = wait_event_interruptible(
                g_scth.epoch_wq,
                READ_ONCE(g_scth.epoch_id) != epoch0 || !READ_ONCE(g_scth.monitor_on)
            );
            if (ret < 0)
                return -EINTR;

            if (!READ_ONCE(g_scth.monitor_on))
                return orig(regs);

            epoch0 = READ_ONCE(g_scth.epoch_id);
            if (scth_try_take_token())
                break;
        }

        if (waited) {
            u64 delay = ktime_get_ns() - t0;
            pr_info_ratelimited("scthrottle: delayed nr=%u comm=%s euid=%u delay_ns=%llu\n",
                                nr, comm, euid, (unsigned long long)delay);
        }

        return orig(regs);
    }

    /* ---------------- FIFO_STRICT ---------------- */
    {
        unsigned long flags;
        bool allowed_now = false;
        long ret;
        u64 t0 = ktime_get_ns();
        struct scth_waiter w;

        INIT_LIST_HEAD(&w.node);
        init_waitqueue_head(&w.wq);
        w.granted = false;
        w.aborted = false;
        w.ticket = 0;

        spin_lock_irqsave(&g_scth.lock, flags);

        if (!g_scth.monitor_on) {
            spin_unlock_irqrestore(&g_scth.lock, flags);
            return orig(regs);
        }

        if (g_scth.fifo_qlen == 0 && g_scth.epoch_used < g_scth.max_active) {
            g_scth.epoch_used++;

            SCTH_LOG_GRANT("IMM", g_scth.epoch_id, 0,
                           g_scth.epoch_used, g_scth.max_active, g_scth.fifo_qlen);

            allowed_now = true;
        } else {
            w.ticket = (u64)atomic64_inc_return(&g_scth.fifo_seq);
            list_add_tail(&w.node, &g_scth.fifo_q);
            g_scth.fifo_qlen++;
            g_scth.current_blocked_threads++;
            allowed_now = false;
        }

        spin_unlock_irqrestore(&g_scth.lock, flags);

        if (allowed_now)
            return orig(regs);

        ret = wait_event_interruptible(
            w.wq,
            READ_ONCE(w.granted) || READ_ONCE(w.aborted) || !READ_ONCE(g_scth.monitor_on)
        );

        spin_lock_irqsave(&g_scth.lock, flags);
        if (g_scth.current_blocked_threads)
            g_scth.current_blocked_threads--;
        spin_unlock_irqrestore(&g_scth.lock, flags);

        if (ret < 0) {
            spin_lock_irqsave(&g_scth.lock, flags);
            if (!READ_ONCE(w.granted) && !READ_ONCE(w.aborted) && !list_empty(&w.node)) {
                list_del_init(&w.node);
                if (g_scth.fifo_qlen)
                    g_scth.fifo_qlen--;
            }
            spin_unlock_irqrestore(&g_scth.lock, flags);
            return -EINTR;
        }

        if (!READ_ONCE(g_scth.monitor_on) || READ_ONCE(w.aborted))
            return orig(regs);

        {
            u64 delay = ktime_get_ns() - t0;
            pr_info_ratelimited("scthrottle: delayed nr=%u comm=%s euid=%u delay_ns=%llu ticket=%llu\n",
                                nr, comm, euid,
                                (unsigned long long)delay,
                                (unsigned long long)w.ticket);
        }

        return orig(regs);
    }
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