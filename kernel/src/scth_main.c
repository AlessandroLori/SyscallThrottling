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
#include <linux/printk.h>

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

struct scth_state g_scth;

/* Module parameter: sys_call_table address discovered by USCTM */
static unsigned long sys_call_table_addr = 0;
module_param(sys_call_table_addr, ulong, 0444);
MODULE_PARM_DESC(sys_call_table_addr, "Address of sys_call_table (from USCTM), e.g. 0xffffffff...");

static void scth_epoch_timer_fn(struct timer_list *t)
{
    unsigned long flags;
    LIST_HEAD(to_wake);
    bool wake_race = false;

    spin_lock_irqsave(&g_scth.lock, flags);

    if (!g_scth.monitor_on) {
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return;
    }

    /* nuova epoca */
    g_scth.epoch_id++;
    g_scth.epoch_used = 0;

    /* apply pending -> active */
    g_scth.max_active    = g_scth.max_pending;
    g_scth.policy_active = g_scth.policy_pending;

    /* stats */
    g_scth.blocked_sum_samples += g_scth.current_blocked_threads;
    g_scth.blocked_num_samples++;

    if (g_scth.policy_active == SCTH_POLICY_WAKE_RACE) {
        atomic_set(&g_scth.epoch_tokens, (int)g_scth.max_active);
        wake_race = true;
    } else {
        /* FIFO_STRICT: grant SOLO i primi max_active in coda */
        while (g_scth.epoch_used < g_scth.max_active && !list_empty(&g_scth.fifo_q)) {
            struct scth_waiter *w =
                list_first_entry(&g_scth.fifo_q, struct scth_waiter, node);

            list_del_init(&w->node);
            if (g_scth.fifo_qlen)
                g_scth.fifo_qlen--;

            g_scth.epoch_used++;
            w->granted = true;

            SCTH_LOG_GRANT("Q", g_scth.epoch_id, w->ticket,
                           g_scth.epoch_used, g_scth.max_active, g_scth.fifo_qlen);

            /* wake fuori lock */
            list_add_tail(&w->node, &to_wake);
        }
    }

    mod_timer(&g_scth.epoch_timer, jiffies + HZ);
    spin_unlock_irqrestore(&g_scth.lock, flags);

    if (wake_race)
        wake_up_all(&g_scth.epoch_wq);

    while (!list_empty(&to_wake)) {
        struct scth_waiter *w = list_first_entry(&to_wake, struct scth_waiter, node);
        list_del_init(&w->node);
        wake_up_interruptible(&w->wq);
    }
}

int scth_monitor_on(void)
{
    unsigned long flags;
    LIST_HEAD(to_wake);
    bool already_on;
    bool wake_race = false;

    spin_lock_irqsave(&g_scth.lock, flags);
    already_on = g_scth.monitor_on;

    if (!already_on) {
        g_scth.monitor_on = true;
        g_scth.epoch_id = 0;
        g_scth.epoch_used = 0;

        g_scth.max_active    = g_scth.max_pending;
        g_scth.policy_active = g_scth.policy_pending;

        if (g_scth.policy_active == SCTH_POLICY_WAKE_RACE) {
            atomic_set(&g_scth.epoch_tokens, (int)g_scth.max_active);
            wake_race = true;
        } else {
            /* FIFO: se c'è già coda, concedi subito fino a max_active */
            while (g_scth.epoch_used < g_scth.max_active && !list_empty(&g_scth.fifo_q)) {
                struct scth_waiter *w =
                    list_first_entry(&g_scth.fifo_q, struct scth_waiter, node);

                list_del_init(&w->node);
                if (g_scth.fifo_qlen)
                    g_scth.fifo_qlen--;

                g_scth.epoch_used++;
                w->granted = true;

                SCTH_LOG_GRANT("Q", g_scth.epoch_id, w->ticket,
                               g_scth.epoch_used, g_scth.max_active, g_scth.fifo_qlen);

                list_add_tail(&w->node, &to_wake);
            }
        }

        mod_timer(&g_scth.epoch_timer, jiffies + HZ);
        pr_info("scthrottle: monitor_on policy=%u max_active=%u\n",
                (unsigned)g_scth.policy_active, (unsigned)g_scth.max_active);
    }

    spin_unlock_irqrestore(&g_scth.lock, flags);

    if (wake_race)
        wake_up_all(&g_scth.epoch_wq);

    while (!list_empty(&to_wake)) {
        struct scth_waiter *w = list_first_entry(&to_wake, struct scth_waiter, node);
        list_del_init(&w->node);
        wake_up_interruptible(&w->wq);
    }

    return 0;
}

int scth_monitor_off(void)
{
    unsigned long flags;
    LIST_HEAD(to_wake);
    bool was_on;

    spin_lock_irqsave(&g_scth.lock, flags);
    was_on = g_scth.monitor_on;
    g_scth.monitor_on = false;

    atomic_set(&g_scth.epoch_tokens, 0);

    /* FIFO: aborta tutti in coda */
    while (!list_empty(&g_scth.fifo_q)) {
        struct scth_waiter *w =
            list_first_entry(&g_scth.fifo_q, struct scth_waiter, node);

        list_del_init(&w->node);
        w->aborted = true;
        list_add_tail(&w->node, &to_wake);
    }
    g_scth.fifo_qlen = 0;

    spin_unlock_irqrestore(&g_scth.lock, flags);

    wake_up_all(&g_scth.epoch_wq);

    while (!list_empty(&to_wake)) {
        struct scth_waiter *w = list_first_entry(&to_wake, struct scth_waiter, node);
        list_del_init(&w->node);
        wake_up_interruptible(&w->wq);
    }

    if (was_on)
        timer_delete_sync(&g_scth.epoch_timer);

    pr_info("scthrottle: monitor_off\n");
    return 0;
}

static int __init scth_init(void)
{
    int ret;

    memset(&g_scth, 0, sizeof(g_scth));
    spin_lock_init(&g_scth.lock);
    mutex_init(&g_scth.cfg_mutex);

    init_waitqueue_head(&g_scth.epoch_wq);

    INIT_LIST_HEAD(&g_scth.fifo_q);
    g_scth.fifo_qlen = 0;
    atomic64_set(&g_scth.fifo_seq, 0);

    scth_cfg_init(&g_scth.cfg);

    g_scth.monitor_on = false;
    g_scth.max_pending = 5;
    g_scth.max_active  = 0;
    g_scth.policy_pending = SCTH_POLICY_FIFO_STRICT;
    g_scth.policy_active  = SCTH_POLICY_FIFO_STRICT;

    atomic_set(&g_scth.epoch_tokens, 0);

    timer_setup(&g_scth.epoch_timer, scth_epoch_timer_fn, 0);

    g_scth.sys_call_table_addr = sys_call_table_addr;
    if (g_scth.sys_call_table_addr)
        pr_info("scthrottle: sys_call_table_addr=0x%lx (from USCTM)\n", g_scth.sys_call_table_addr);
    else
        pr_warn("scthrottle: sys_call_table_addr not provided (ok for M1/M2; required from M3)\n");

    ret = scth_dev_init();
    if (ret) {
        pr_err("scthrottle: failed to init device: %d\n", ret);
        return ret;
    }

    pr_info("scthrottle: loaded (device /dev/%s)\n", SCTH_DEV_NAME);
    return 0;
}

static void __exit scth_exit(void)
{
    scth_monitor_off();

    mutex_lock(&g_scth.cfg_mutex);
    scth_cfg_destroy(&g_scth.cfg);
    mutex_unlock(&g_scth.cfg_mutex);

    /* restore syscall hooks */
    scth_hook_remove_all();

    scth_dev_exit();
    pr_info("scthrottle: unloaded\n");
}

module_init(scth_init);
module_exit(scth_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("syscall_throttle project");
MODULE_DESCRIPTION("Syscall throttling module (M3: FIFO_STRICT queue vs WAKE_RACE token race)");