#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/moduleparam.h>
#include <linux/timer.h>

#include "scth_internal.h"

struct scth_state g_scth;

/* Module parameter: sys_call_table address discovered by USCTM */
static unsigned long sys_call_table_addr = 0;
module_param(sys_call_table_addr, ulong, 0444);
MODULE_PARM_DESC(sys_call_table_addr, "Address of sys_call_table (from USCTM), e.g. 0xffffffff...");

static void scth_epoch_timer_fn(struct timer_list *t)
{
    unsigned long flags;

    spin_lock_irqsave(&g_scth.lock, flags);

    if (!g_scth.monitor_on) {
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return;
    }

    /* Nuova epoca */
    g_scth.epoch_id++;

    /* Applica pending -> active (last write wins) */
    g_scth.max_active = g_scth.max_pending;
    g_scth.policy_active = g_scth.policy_pending;

    /* Campionamento blocked (in M1 rimarrà 0) */
    g_scth.blocked_sum_samples += g_scth.current_blocked_threads;
    g_scth.blocked_num_samples++;

    /* Reschedule dopo ~1s */
    mod_timer(&g_scth.epoch_timer, jiffies + HZ);

    spin_unlock_irqrestore(&g_scth.lock, flags);
}

int scth_monitor_on(void)
{
    unsigned long flags;
    bool already_on;

    spin_lock_irqsave(&g_scth.lock, flags);
    already_on = g_scth.monitor_on;

    if (!already_on) {
        g_scth.monitor_on = true;
        g_scth.epoch_id = 0;

        /* applica pending subito */
        g_scth.max_active = g_scth.max_pending;
        g_scth.policy_active = g_scth.policy_pending;

        mod_timer(&g_scth.epoch_timer, jiffies + HZ);
    }

    spin_unlock_irqrestore(&g_scth.lock, flags);
    return 0;
}

int scth_monitor_off(void)
{
    unsigned long flags;
    bool was_on;

    spin_lock_irqsave(&g_scth.lock, flags);
    was_on = g_scth.monitor_on;
    g_scth.monitor_on = false;
    spin_unlock_irqrestore(&g_scth.lock, flags);

    if (was_on)
        timer_delete_sync(&g_scth.epoch_timer);

    return 0;
}

static int __init scth_init(void)
{
    int ret;

    memset(&g_scth, 0, sizeof(g_scth));
    spin_lock_init(&g_scth.lock);
    mutex_init(&g_scth.cfg_mutex);
    scth_cfg_init(&g_scth.cfg);

    /* default ragionevoli */
    g_scth.monitor_on = false;
    g_scth.max_pending = 5;
    g_scth.max_active  = 0;
    g_scth.policy_pending = SCTH_POLICY_FIFO_STRICT;
    g_scth.policy_active  = SCTH_POLICY_FIFO_STRICT;

    timer_setup(&g_scth.epoch_timer, scth_epoch_timer_fn, 0);

    /* bootstrap sys_call_table_addr (M1: non lo usiamo ancora, ma lo memorizziamo) */
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
    scth_dev_exit();
    pr_info("scthrottle: unloaded\n");
}

module_init(scth_init);
module_exit(scth_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("syscall_throttle project");
MODULE_DESCRIPTION("Syscall throttling module (M1: ioctl + tick, USCTM sys_call_table bootstrap)");