#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/moduleparam.h>
#include <linux/timer.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/list.h>

#include "scth_internal.h"

#ifndef SCTH_LOG_GRANT
#define SCTH_LOG_GRANT(...) do { } while (0)
#endif


/* Motore amministrativo del modulo, gestione del monitor in funzione della policy e gestione epoche */


struct scth_state g_scth;

/* Module parameter: sys_call_table address discovered by USCTM */
static unsigned long sys_call_table_addr = 0;
module_param(sys_call_table_addr, ulong, 0444);
MODULE_PARM_DESC(sys_call_table_addr, "Address of sys_call_table (from USCTM), e.g. 0xffffffff...");

static void scth_epoch_timer_fn(struct timer_list *t) 
// Funzione che rende il sistema epoch based. Viene chiamata periodicamente dal timer, è dove avviene il tick del monitor
// viene incrementata l'epochid e aggiorna gli slot dell'epoca. Se si passa da FIFO a WAKE&RACE i thread rimasti nella coda FIFO vengono
// drenati.
{
    unsigned long flags;
    LIST_HEAD(to_wake);
    bool wake_epoch_waiters = false;
    __u8 old_policy;

    spin_lock_irqsave(&g_scth.lock, flags);

    if (!g_scth.monitor_on) {
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return;
    }

    /* nuova epoca */
    g_scth.epoch_id++;
    g_scth.epoch_used = 0;

    old_policy = g_scth.policy_active;

    /* apply pending -> active */
    g_scth.max_active    = g_scth.max_pending;
    g_scth.policy_active = g_scth.policy_pending;

    /* campionamento blocked (media) */
    g_scth.blocked_sum_samples += g_scth.current_blocked_threads;
    g_scth.blocked_num_samples++;

    /*
     * Se passiamo da WAKE_RACE a FIFO, devo svegliare i waiter su epoch_wq
     * affinché si auto-migrino nella fifo_q.
     */
    if (old_policy == SCTH_POLICY_WAKE_RACE &&
        g_scth.policy_active == SCTH_POLICY_FIFO_STRICT) {
        wake_epoch_waiters = true;
    }

    if (!list_empty(&g_scth.fifo_q)) {
        atomic_set(&g_scth.epoch_tokens, 0);

        while (g_scth.epoch_used < g_scth.max_active && !list_empty(&g_scth.fifo_q)) {
            struct scth_waiter *w =
                list_first_entry(&g_scth.fifo_q, struct scth_waiter, node);

            list_del_init(&w->node);
            if (g_scth.fifo_qlen)
                g_scth.fifo_qlen--;

            w->granted = true;

            SCTH_LOG_GRANT("Q",
                           g_scth.epoch_id,
                           w->ticket,
                           g_scth.epoch_used,
                           g_scth.max_active,
                           g_scth.fifo_qlen);

            g_scth.epoch_used++;

            /* sveglio fuori lock */
            list_add_tail(&w->node, &to_wake);
        }

    } else if (g_scth.policy_active == SCTH_POLICY_WAKE_RACE) {
        atomic_set(&g_scth.epoch_tokens, (int)g_scth.max_active);
        wake_epoch_waiters = true;

    } else {
        atomic_set(&g_scth.epoch_tokens, 0);
    }

    /* reschedule ~1s */
    mod_timer(&g_scth.epoch_timer, jiffies + HZ);

    spin_unlock_irqrestore(&g_scth.lock, flags);

    if (wake_epoch_waiters)
        wake_up_all(&g_scth.epoch_wq);

    /* sveglia FIFO granted */
    while (!list_empty(&to_wake)) {
        struct scth_waiter *w = list_first_entry(&to_wake, struct scth_waiter, node);
        list_del_init(&w->node);
        wake_up(&w->wq);
    }
}

int scth_monitor_on(void)
// Accende il monitor, resetta epochid, applica valori di configurazione in pending, avvia il timer.
// Porta il sistema da uno stato inerte ad uno stato di possibile throttling
{
    unsigned long flags;
    bool already_on;
    bool wake_race = false;

    spin_lock_irqsave(&g_scth.lock, flags);
    already_on = g_scth.monitor_on;

    if (!already_on) {
        g_scth.monitor_on = true;
        g_scth.epoch_id = 0;
        g_scth.epoch_used = 0;

        /* apply pending subito */
        g_scth.max_active    = g_scth.max_pending;
        g_scth.policy_active = g_scth.policy_pending;

        if (g_scth.policy_active == SCTH_POLICY_WAKE_RACE) {
            atomic_set(&g_scth.epoch_tokens, (int)g_scth.max_active);
            wake_race = true;
        } else {
            /* FIFO: non concedo qui, concede il wrapper (IMM) o il timer (Q) */
        }

        mod_timer(&g_scth.epoch_timer, jiffies + HZ);
        pr_info("scthrottle: monitor_on\n");
    }

    spin_unlock_irqrestore(&g_scth.lock, flags);

    if (wake_race)
        wake_up_all(&g_scth.epoch_wq);

    return 0;
}

int scth_monitor_off(void)
// Disattiva monitor, ferma il timer, marca aborted tutti i thread che aspettano e libera le code mandando tutti in esecuzione.
{
    unsigned long flags;
    LIST_HEAD(to_wake);
    bool was_on;

    spin_lock_irqsave(&g_scth.lock, flags);

    was_on = g_scth.monitor_on;
    g_scth.monitor_on = false;

    /* WAKE_RACE: azzera token */
    atomic_set(&g_scth.epoch_tokens, 0);

    /* FIFO: aborta tutti quelli in coda */
    while (!list_empty(&g_scth.fifo_q)) {
        struct scth_waiter *w = list_first_entry(&g_scth.fifo_q, struct scth_waiter, node);
        list_del_init(&w->node);
        w->aborted = true;
        list_add_tail(&w->node, &to_wake);
    }
    g_scth.fifo_qlen = 0;

    spin_unlock_irqrestore(&g_scth.lock, flags);

    /* sveglia chi aspetta cambio epoca */
    wake_up_all(&g_scth.epoch_wq);

    /* sveglia i FIFO abortiti */
    while (!list_empty(&to_wake)) {
        struct scth_waiter *w = list_first_entry(&to_wake, struct scth_waiter, node);
        list_del_init(&w->node);
        wake_up(&w->wq);
    }

    if (was_on)
        timer_delete_sync(&g_scth.epoch_timer);

    pr_info("scthrottle: monitor_off\n");
    return 0;
}

static int __init scth_init(void)
// Entry point, inizializza tutte le strutture responsabili di sincronizzazione come code, contatori, lock, mutex,
// registra il device in /dev/scthrottle e legge il parametro sys_call_table_addr proveniente dal modulo in external/

{
    int ret;

    memset(&g_scth, 0, sizeof(g_scth));
    spin_lock_init(&g_scth.lock);
    mutex_init(&g_scth.cfg_mutex);
    init_waitqueue_head(&g_scth.epoch_wq);
    init_waitqueue_head(&g_scth.unload_wq);

    g_scth.stopping = false;
    atomic_set(&g_scth.active_wrappers, 0);

    atomic64_set(&g_scth.total_tracked, 0);
    atomic64_set(&g_scth.total_immediate, 0);
    atomic64_set(&g_scth.total_delayed, 0);
    atomic64_set(&g_scth.total_aborted, 0);
    atomic64_set(&g_scth.delay_sum_ns, 0);
    atomic64_set(&g_scth.delay_num, 0);

    INIT_LIST_HEAD(&g_scth.fifo_q);
    g_scth.fifo_qlen = 0;
    atomic64_set(&g_scth.fifo_seq, 0);

    {
        struct scth_cfg_store *cfg0 = scth_cfg_alloc_empty(GFP_KERNEL);
        if (!cfg0)
            return -ENOMEM;

        rcu_assign_pointer(g_scth.cfg, cfg0);
    }

    /* default */
    g_scth.monitor_on = false;
    g_scth.max_pending = 5;
    g_scth.max_active  = 0;
    g_scth.policy_pending = SCTH_POLICY_FIFO_STRICT;
    g_scth.policy_active  = SCTH_POLICY_FIFO_STRICT;

    atomic_set(&g_scth.epoch_tokens, 0);

    /* stats init */
    g_scth.peak_delay_ns = 0;
    memset(g_scth.peak_comm, 0, sizeof(g_scth.peak_comm));
    g_scth.peak_euid = 0;

    g_scth.peak_blocked_threads = 0;
    g_scth.blocked_sum_samples = 0;
    g_scth.blocked_num_samples = 0;
    g_scth.current_blocked_threads = 0;

    g_scth.peak_fifo_qlen = 0;

    atomic64_set(&g_scth.total_tracked, 0);
    atomic64_set(&g_scth.total_immediate, 0);
    atomic64_set(&g_scth.total_delayed, 0);
    atomic64_set(&g_scth.total_aborted, 0);

    atomic64_set(&g_scth.delay_sum_ns, 0);
    atomic64_set(&g_scth.delay_num, 0);

    timer_setup(&g_scth.epoch_timer, scth_epoch_timer_fn, 0);

    g_scth.sys_call_table_addr = sys_call_table_addr;
    if (g_scth.sys_call_table_addr)
        pr_info("scthrottle: sys_call_table_addr=0x%lx (from USCTM)\n", g_scth.sys_call_table_addr);
    else
        pr_warn("scthrottle: sys_call_table_addr not provided (ok for M1/M2; required from M3)\n");

    ret = scth_dev_init();
    if (ret) {
        struct scth_cfg_store *cfg0;

        mutex_lock(&g_scth.cfg_mutex);
        cfg0 = rcu_dereference_protected(g_scth.cfg, lockdep_is_held(&g_scth.cfg_mutex));
        RCU_INIT_POINTER(g_scth.cfg, NULL);
        mutex_unlock(&g_scth.cfg_mutex);

        scth_cfg_destroy(cfg0);

        pr_err("scthrottle: failed to init device: %d\n", ret);
        return ret;
    }

    pr_info("scthrottle: loaded (device /dev/%s)\n", SCTH_DEV_NAME);
    return 0;
}

static void __exit scth_exit(void)
// Termina il modulo, triggera lo spegnimento del monitor, rimuove hook attivi e pulisce il sistema.
{
    struct scth_cfg_store *cfg;

    WRITE_ONCE(g_scth.stopping, true);

    scth_monitor_off();
    scth_hook_remove_all();

    wait_event(g_scth.unload_wq,
               atomic_read(&g_scth.active_wrappers) == 0);

    mutex_lock(&g_scth.cfg_mutex);
    cfg = rcu_dereference_protected(g_scth.cfg, lockdep_is_held(&g_scth.cfg_mutex));
    RCU_INIT_POINTER(g_scth.cfg, NULL);
    mutex_unlock(&g_scth.cfg_mutex);

    synchronize_rcu();
    scth_cfg_destroy(cfg);

    /*
     * Importante: drena eventuali call_rcu() pendenti generate da vecchi update
     * della config, così nessun callback del modulo resta in coda dopo l'unload.
     */
    rcu_barrier();

    scth_dev_exit();
    pr_info("scthrottle: unloaded\n");
}

module_init(scth_init);
module_exit(scth_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("syscall_throttle project");
MODULE_DESCRIPTION("Syscall throttling module (M3: FIFO_STRICT queue vs WAKE_RACE token race + full stats)");