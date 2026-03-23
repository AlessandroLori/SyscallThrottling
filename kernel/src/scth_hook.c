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
#include <linux/atomic.h>
#include <linux/string.h>

#include <asm/processor.h>
#include <asm/processor-flags.h>
#include <asm/special_insns.h>

#include "scth_internal.h"


/* Gestione del syscall throttling, intercettazione e decisione del destino dei thread. 
    Implementa: meccanismo di hook su syscall table, wrapper per interettazione syscall controllo e 
    reindirizzamento a meccanismo, policy di scheduling, aggiornamento statistiche */


typedef asmlinkage long (*scth_sys_fn_t)(const struct pt_regs *regs);

static scth_sys_fn_t *scth_syscall_table(void)
// Responsabile di punto di accesso alla syscall table senza duplicare la logica, restituisce il puntatore alla syscall table

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
// Scrive forzosamente nel registro CR0, usato per disabilitare e riabilitare il bit di write protect per poter modificare la syscall table
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}

static __always_inline void write_cr4_forced(unsigned long val)
// Scrive forzosamente nel registro CR4 per poter patchare la tabella
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr4" : "+r"(val), "+m"(__force_order));
}

static __always_inline void scth_wp_off(struct scth_wp_ctx *ctx)
// Prepara patching alla syscall table disabilitando preemption e salvando CR0 e CR4 creando una finestra critica in cui il modulo può
// scrivere su syscall table
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
// Fase finale di scrittura su syscall table, ripristina i CR e la preemption

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
static bool scth_match_registered(u32 nr,
                                  const char comm[SCTH_COMM_LEN],
                                  u32 euid)
// Controlla snapshot RCU corrente: syscall registrata AND (prog OR uid)
{
    bool match = false;
    struct scth_cfg_store *cfg;

    rcu_read_lock();

    cfg = rcu_dereference(g_scth.cfg);
    if (cfg &&
        scth_cfg_has_sys(cfg, nr) &&
        (scth_cfg_has_prog(cfg, comm) || scth_cfg_has_uid(cfg, euid)))
        match = true;

    rcu_read_unlock();
    return match;
}

/* ---- blocked helpers (per stats) ---- */
static __always_inline void scth_blocked_inc_locked(void)
// Incrementa il numero di thread bloccati e aumenta il picco di thread massimi bloccati se necessario

{
    g_scth.current_blocked_threads++;
    if (g_scth.current_blocked_threads > g_scth.peak_blocked_threads)
        g_scth.peak_blocked_threads = g_scth.current_blocked_threads;
}

static __always_inline void scth_blocked_dec_locked(void)
// Decrementa il numero di thread bloccati
{
    if (g_scth.current_blocked_threads)
        g_scth.current_blocked_threads--;
}

static __always_inline void scth_blocked_inc(void)
//Prende il lock, incrementa e lo rilascia
{
    unsigned long flags;
    spin_lock_irqsave(&g_scth.lock, flags);
    scth_blocked_inc_locked();
    spin_unlock_irqrestore(&g_scth.lock, flags);
}

static __always_inline void scth_blocked_dec(void)
// Prende il lock, decrementa e lo rilascia 
{
    unsigned long flags;
    spin_lock_irqsave(&g_scth.lock, flags);
    scth_blocked_dec_locked();
    spin_unlock_irqrestore(&g_scth.lock, flags);
}

/* ---- peak delay helper ---- */
static __always_inline void scth_update_peak_delay(u64 delay_ns,
                                                   const char comm[SCTH_COMM_LEN],
                                                   u32 euid)
// Aggiorna il ritardo massimo osservato                                                
{
    unsigned long flags;
    spin_lock_irqsave(&g_scth.lock, flags);

    if (delay_ns > g_scth.peak_delay_ns) {
        g_scth.peak_delay_ns = delay_ns;
        strscpy(g_scth.peak_comm, comm, SCTH_COMM_LEN);
        g_scth.peak_euid = euid;
    }

    spin_unlock_irqrestore(&g_scth.lock, flags);
}

/* ---- aggregate stats helpers ---- */
static __always_inline void scth_stat_tracked(void)
// Conta quante syscall sono entrate nel sistema di throttling (aspettando)
{
    atomic64_inc(&g_scth.total_tracked);
}

static __always_inline void scth_stat_immediate(void)
// Conta quante syscall sono entrate nel sistema di throttling senza dover aspettare
{
    atomic64_inc(&g_scth.total_immediate);
}

static __always_inline void scth_stat_aborted(void)
// Conta quante syscall sono entrate nel sistema di throttling ma poi abortite per qualche motivo
{
    atomic64_inc(&g_scth.total_aborted);
}

static __always_inline void scth_stat_delayed(u64 delay_ns,
                                              const char comm[SCTH_COMM_LEN],
                                              u32 euid)
// Aggiorna le statistiche inerenti ai ritardi                                             
{
    atomic64_inc(&g_scth.total_delayed);
    atomic64_add(delay_ns, &g_scth.delay_sum_ns);
    atomic64_inc(&g_scth.delay_num);
    scth_update_peak_delay(delay_ns, comm, euid);
}

/* ---- WAKE_RACE token acquire (per-epoch) ---- */
static __always_inline bool scth_try_take_token(void)
// Implementa la vera gara in WAKE&RACE in cui i thread eseguono cmpxchng per poter decrementare il numero di slot disponibili in epoca
// e se ha successo ottiene lo slot di esecuzione per questa epoca
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

static __always_inline void scth_fifo_enqueue_waiter_locked(struct scth_waiter *w)
{
    struct scth_waiter *pos;

    list_for_each_entry(pos, &g_scth.fifo_q, node) {
        if (w->ticket < pos->ticket) {
            list_add_tail(&w->node, &pos->node);
            goto inserted;
        }
    }

    list_add_tail(&w->node, &g_scth.fifo_q);

inserted:
    g_scth.fifo_qlen++;
    if (g_scth.fifo_qlen > g_scth.peak_fifo_qlen)
        g_scth.peak_fifo_qlen = g_scth.fifo_qlen;
}

static __always_inline void scth_wrapper_enter(void)
{
    atomic_inc(&g_scth.active_wrappers);
}

static __always_inline void scth_wrapper_exit(void)
{
    if (atomic_dec_and_test(&g_scth.active_wrappers) &&
        READ_ONCE(g_scth.stopping))
        wake_up(&g_scth.unload_wq);
}

/* ---- wrapper M3: FIFO_STRICT vs WAKE_RACE ---- */
static asmlinkage long scth_syscall_wrapper(const struct pt_regs *regs)
// Implementa sostituzione syscall in syscall table, bypass se monitor off, legge policy e la applica se FIFO o WAKE&RACE.
// Punto in cui viene effettuata la distinzione tra bypass, immidiate, delayed e aborted.
// Possiede logica FIFO e gestione della coda, risvegli e bypass
// Possiede logica WAKE&RACE e gestione del risveglio di tutti e di disponibilità slot (token) per epoca
{
    u32 nr = (u32)regs->orig_ax;
    scth_sys_fn_t orig;
    char comm[SCTH_COMM_LEN];
    u32 euid;
    u32 policy;
    long rc;

    if (nr >= NR_syscalls)
        return -ENOSYS;

    scth_wrapper_enter();

    orig = READ_ONCE(scth_orig[nr]);
    if (!orig) {
        rc = -ENOSYS;
        goto out;
    }

    if (READ_ONCE(g_scth.stopping) || !READ_ONCE(g_scth.monitor_on)) {
        rc = orig(regs);
        goto out;
    }

    get_task_comm(comm, current);
    euid = (u32)from_kuid(&init_user_ns, current_euid());

    /* se NON matcha config corrente (sys AND (prog OR uid)), bypass totale */
    if (!scth_match_registered(nr, comm, euid)) {
        rc = orig(regs);
        goto out;
    }

    /* questa syscall è davvero tracciata dal throttling */
    scth_stat_tracked();

    policy = READ_ONCE(g_scth.policy_active);

    /* ---------------- WAKE_RACE ---------------- */
    if (policy == SCTH_POLICY_WAKE_RACE) {
        u64 t0 = ktime_get_ns();
        u64 epoch0 = READ_ONCE(g_scth.epoch_id);
        bool blocked_counted = false;
        u64 my_ticket = 0;

        /* token immediato => immediate */
        if (scth_try_take_token()) {
            scth_stat_immediate();
            rc = orig(regs);
            goto out;
        }

        /* devo aspettare: conta blocked */
        scth_blocked_inc();
        blocked_counted = true;

        /* ticket usato anche per eventuale migrazione in FIFO */
        my_ticket = (u64)atomic64_inc_return(&g_scth.fifo_seq);

        for (;;) {
            long ret;

            ret = wait_event_interruptible(
                g_scth.epoch_wq,
                READ_ONCE(g_scth.epoch_id) != epoch0 ||
                !READ_ONCE(g_scth.monitor_on) ||
                READ_ONCE(g_scth.stopping) ||
                READ_ONCE(g_scth.policy_active) != SCTH_POLICY_WAKE_RACE
            );

            if (ret < 0) {
                if (blocked_counted)
                    scth_blocked_dec();
                scth_stat_aborted();
                rc = -EINTR;
                goto out;
            }

            if (READ_ONCE(g_scth.stopping) || !READ_ONCE(g_scth.monitor_on)) {
                if (blocked_counted)
                    scth_blocked_dec();
                scth_stat_aborted();
                rc = orig(regs);
                goto out;
            }

            /*
             * Migrazione WAKE_RACE -> FIFO:
             * il waiter si auto-inserisce nella fifo_q usando il ticket già assegnato,
             * così preserviamo l'ordine temporale di arrivo.
             */
            if (READ_ONCE(g_scth.policy_active) == SCTH_POLICY_FIFO_STRICT) {
                unsigned long flags;
                long qret;
                struct scth_waiter w;

                INIT_LIST_HEAD(&w.node);
                init_waitqueue_head(&w.wq);
                w.granted = false;
                w.aborted = false;
                w.ticket = my_ticket;

                spin_lock_irqsave(&g_scth.lock, flags);

                if (!g_scth.monitor_on || g_scth.stopping) {
                    spin_unlock_irqrestore(&g_scth.lock, flags);
                    if (blocked_counted)
                        scth_blocked_dec();
                    scth_stat_aborted();
                    rc = orig(regs);
                    goto out;
                }

                /*
                 * Se nel frattempo la policy è cambiata di nuovo, torno nel loop.
                 */
                if (g_scth.policy_active != SCTH_POLICY_FIFO_STRICT) {
                    spin_unlock_irqrestore(&g_scth.lock, flags);
                    epoch0 = READ_ONCE(g_scth.epoch_id);
                    continue;
                }

                /*
                 * Non incremento blocked: questo thread era già contato come blocked
                 * quando stava aspettando in WAKE_RACE.
                 */
                scth_fifo_enqueue_waiter_locked(&w);
                spin_unlock_irqrestore(&g_scth.lock, flags);

                qret = wait_event_interruptible(
                    w.wq,
                    READ_ONCE(w.granted) ||
                    READ_ONCE(w.aborted) ||
                    !READ_ONCE(g_scth.monitor_on) ||
                    READ_ONCE(g_scth.stopping)
                );

                if (blocked_counted)
                    scth_blocked_dec();

                if (qret < 0) {
                    spin_lock_irqsave(&g_scth.lock, flags);
                    if (!READ_ONCE(w.granted) && !READ_ONCE(w.aborted) && !list_empty(&w.node)) {
                        list_del_init(&w.node);
                        if (g_scth.fifo_qlen)
                            g_scth.fifo_qlen--;
                    }
                    spin_unlock_irqrestore(&g_scth.lock, flags);

                    scth_stat_aborted();
                    rc = -EINTR;
                    goto out;
                }

                if (READ_ONCE(g_scth.stopping) ||
                    !READ_ONCE(g_scth.monitor_on) ||
                    READ_ONCE(w.aborted)) {
                    scth_stat_aborted();
                    rc = orig(regs);
                    goto out;
                }

                {
                    u64 delay = ktime_get_ns() - t0;
                    scth_stat_delayed(delay, comm, euid);

                    pr_info_ratelimited("scthrottle: delayed nr=%u comm=%s euid=%u delay_ns=%llu ticket=%llu\n",
                                        nr, comm, euid,
                                        (unsigned long long)delay,
                                        (unsigned long long)my_ticket);
                }

                rc = orig(regs);
                goto out;
            }

            epoch0 = READ_ONCE(g_scth.epoch_id);
            if (scth_try_take_token())
                break;
        }

        if (blocked_counted)
            scth_blocked_dec();

        {
            u64 delay = ktime_get_ns() - t0;
            scth_stat_delayed(delay, comm, euid);

            pr_info_ratelimited("scthrottle: delayed nr=%u comm=%s euid=%u delay_ns=%llu ticket=%llu\n",
                                nr, comm, euid,
                                (unsigned long long)delay,
                                (unsigned long long)my_ticket);
        }

        rc = orig(regs);
        goto out;
    }

    /* ---------------- FIFO_STRICT (vera coda FIFO) ---------------- */
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

        if (!g_scth.monitor_on || g_scth.stopping) {
            spin_unlock_irqrestore(&g_scth.lock, flags);
            rc = orig(regs);
            goto out;
        }

        /*
         * FIFO STRICT:
         * - se coda vuota e ho budget in questa epoca -> passa subito
         * - se coda NON vuota -> ti accodi (anche se c'è budget) per non scavalcare
         */
        if (g_scth.fifo_qlen == 0 && g_scth.epoch_used < g_scth.max_active) {
            g_scth.epoch_used++;
            SCTH_LOG_GRANT("IMM", g_scth.epoch_id, 0,
                           g_scth.epoch_used, g_scth.max_active, g_scth.fifo_qlen);
            allowed_now = true;
        } else {
            w.ticket = (u64)atomic64_inc_return(&g_scth.fifo_seq);
            scth_fifo_enqueue_waiter_locked(&w);

            scth_blocked_inc_locked();
            allowed_now = false;
        }

        spin_unlock_irqrestore(&g_scth.lock, flags);

        if (allowed_now) {
            scth_stat_immediate();
            rc = orig(regs);
            goto out;
        }

        /* attesa fino a grant, oppure monitor_off/stopping, oppure abort/signal */
        ret = wait_event_interruptible(
            w.wq,
            READ_ONCE(w.granted) ||
            READ_ONCE(w.aborted) ||
            !READ_ONCE(g_scth.monitor_on) ||
            READ_ONCE(g_scth.stopping)
        );

        /* quando esco dall’attesa, non sono più blocked */
        scth_blocked_dec();

        /* signal: se ancora in coda, rimuovi e conta aborted */
        if (ret < 0) {
            spin_lock_irqsave(&g_scth.lock, flags);
            if (!READ_ONCE(w.granted) && !READ_ONCE(w.aborted) && !list_empty(&w.node)) {
                list_del_init(&w.node);
                if (g_scth.fifo_qlen)
                    g_scth.fifo_qlen--;
            }
            spin_unlock_irqrestore(&g_scth.lock, flags);

            scth_stat_aborted();
            rc = -EINTR;
            goto out;
        }

        /* monitor off / stopping / abort -> bypass immediato */
        if (READ_ONCE(g_scth.stopping) ||
            !READ_ONCE(g_scth.monitor_on) ||
            READ_ONCE(w.aborted)) {
            scth_stat_aborted();
            rc = orig(regs);
            goto out;
        }

        /* granted dopo attesa */
        {
            u64 delay = ktime_get_ns() - t0;
            scth_stat_delayed(delay, comm, euid);

            pr_info_ratelimited("scthrottle: delayed nr=%u comm=%s euid=%u delay_ns=%llu ticket=%llu\n",
                                nr, comm, euid,
                                (unsigned long long)delay,
                                (unsigned long long)w.ticket);
        }

        rc = orig(regs);
        goto out;
    }

out:
    scth_wrapper_exit();
    return rc;
}

int scth_hook_install(u32 nr)
// Rende syscall ordinaria syscall gestita dal wrapper
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
        return 0;
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
// Ripristina puntatore originale nella syscall table
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
// Rimuove ogni hook presente, usato in unload
{
    u32 nr;
    for (nr = 0; nr < NR_syscalls; nr++) {
        if (READ_ONCE(scth_hooked[nr]))
            scth_hook_remove(nr);
    }
}