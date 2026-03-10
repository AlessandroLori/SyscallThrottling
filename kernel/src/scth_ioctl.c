#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/spinlock.h>

#include "scth_internal.h"

static inline bool scth_is_root(void)
{
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static void scth_stats_reset_locked(void)
{
    /* peak delay */
    g_scth.peak_delay_ns = 0;
    memset(g_scth.peak_comm, 0, sizeof(g_scth.peak_comm));
    g_scth.peak_euid = 0;

    /* blocked stats */
    g_scth.peak_blocked_threads = 0;
    g_scth.blocked_sum_samples = 0;
    g_scth.blocked_num_samples = 0;

    /* totals */
    atomic64_set(&g_scth.total_tracked, 0);
    atomic64_set(&g_scth.total_immediate, 0);
    atomic64_set(&g_scth.total_delayed, 0);
    atomic64_set(&g_scth.total_aborted, 0);
    atomic64_set(&g_scth.delay_sum_ns, 0);
    atomic64_set(&g_scth.delay_num, 0);
}

long scth_ioctl_dispatch(unsigned int cmd, unsigned long arg)
{
    switch (cmd) {

    case SCTH_IOC_ON:
        return scth_monitor_on();

    case SCTH_IOC_OFF:
        return scth_monitor_off();

    case SCTH_IOC_SET_MAX: {
        __u32 v;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&v, (void __user *)arg, sizeof(v))) return -EFAULT;

        /* set pending */
        spin_lock(&g_scth.lock);
        g_scth.max_pending = v;
        spin_unlock(&g_scth.lock);
        return 0;
    }

    case SCTH_IOC_SET_POLICY: {
        __u32 v;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&v, (void __user *)arg, sizeof(v))) return -EFAULT;
        if (v > SCTH_POLICY_WAKE_RACE) return -EINVAL;

        spin_lock(&g_scth.lock);
        g_scth.policy_pending = (__u8)v;
        spin_unlock(&g_scth.lock);
        return 0;
    }

    case SCTH_IOC_RESET_STATS: {
        if (!scth_is_root()) return -EPERM;
        spin_lock(&g_scth.lock);
        scth_stats_reset_locked();
        spin_unlock(&g_scth.lock);
        return 0;
    }

    case SCTH_IOC_GET_CFG: {
        struct scth_cfg c;

        spin_lock(&g_scth.lock);
        c.abi = SCTH_ABI_VERSION;
        c.monitor_on = g_scth.monitor_on ? 1 : 0;
        c.epoch_id = g_scth.epoch_id;
        c.max_active = g_scth.max_active;
        c.max_pending = g_scth.max_pending;
        c.policy_active = g_scth.policy_active;
        c.policy_pending = g_scth.policy_pending;
        spin_unlock(&g_scth.lock);

        if (copy_to_user((void __user *)arg, &c, sizeof(c))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_STATS: {
        struct scth_stats s;

        spin_lock(&g_scth.lock);
        s.abi = SCTH_ABI_VERSION;

        s.peak_delay_ns = g_scth.peak_delay_ns;
        memcpy(s.peak_comm, g_scth.peak_comm, SCTH_COMM_LEN);
        s.peak_euid = g_scth.peak_euid;

        s.peak_blocked_threads = g_scth.peak_blocked_threads;
        s.blocked_sum_samples = g_scth.blocked_sum_samples;
        s.blocked_num_samples = g_scth.blocked_num_samples;
        spin_unlock(&g_scth.lock);

        s.total_tracked   = atomic64_read(&g_scth.total_tracked);
        s.total_immediate = atomic64_read(&g_scth.total_immediate);
        s.total_delayed   = atomic64_read(&g_scth.total_delayed);
        s.total_aborted   = atomic64_read(&g_scth.total_aborted);
        s.delay_sum_ns    = atomic64_read(&g_scth.delay_sum_ns);
        s.delay_num       = atomic64_read(&g_scth.delay_num);

        if (copy_to_user((void __user *)arg, &s, sizeof(s))) return -EFAULT;
        return 0;
    }

    /* ---- PROG ---- */
    case SCTH_IOC_ADD_PROG: {
        struct scth_prog_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_prog(&g_scth.cfg, a.comm);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_PROG: {
        struct scth_prog_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_prog(&g_scth.cfg, a.comm);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_PROG_COUNT: {
        __u32 cnt;
        mutex_lock(&g_scth.cfg_mutex);
        cnt = scth_cfg_prog_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);
        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_PROG_LIST: {
        struct scth_list_req req;
        struct scth_prog_arg *kbuf;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        needed = scth_cfg_prog_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.cap < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
            return -ENOSPC;
        }

        kbuf = kcalloc(needed ? needed : 1, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf) return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        wrote = scth_cfg_fill_prog_list(&g_scth.cfg, kbuf, needed);
        mutex_unlock(&g_scth.cfg_mutex);

        req.count = wrote;
        if (wrote && copy_to_user((void __user *)(uintptr_t)req.ptr, kbuf, wrote * sizeof(*kbuf))) {
            kfree(kbuf);
            return -EFAULT;
        }
        kfree(kbuf);

        if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
        return 0;
    }

    /* ---- UID ---- */
    case SCTH_IOC_ADD_UID: {
        struct scth_uid_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_uid(&g_scth.cfg, a.euid);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_UID: {
        struct scth_uid_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_uid(&g_scth.cfg, a.euid);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_UID_COUNT: {
        __u32 cnt;
        mutex_lock(&g_scth.cfg_mutex);
        cnt = scth_cfg_uid_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);
        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_UID_LIST: {
        struct scth_list_req req;
        __u32 *kbuf;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        needed = scth_cfg_uid_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.cap < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
            return -ENOSPC;
        }

        kbuf = kcalloc(needed ? needed : 1, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf) return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        wrote = scth_cfg_fill_uid_list(&g_scth.cfg, kbuf, needed);
        mutex_unlock(&g_scth.cfg_mutex);

        req.count = wrote;
        if (wrote && copy_to_user((void __user *)(uintptr_t)req.ptr, kbuf, wrote * sizeof(*kbuf))) {
            kfree(kbuf);
            return -EFAULT;
        }
        kfree(kbuf);

        if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
        return 0;
    }

    /* ---- SYS ---- */
    case SCTH_IOC_ADD_SYS: {
        struct scth_sys_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_sys(&g_scth.cfg, a.nr);
        mutex_unlock(&g_scth.cfg_mutex);
        if (ret == 0)
            scth_hook_install(a.nr);
        return ret;
    }

    case SCTH_IOC_DEL_SYS: {
        struct scth_sys_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_sys(&g_scth.cfg, a.nr);
        mutex_unlock(&g_scth.cfg_mutex);
        if (ret == 0)
            scth_hook_remove(a.nr);
        return ret;
    }

    case SCTH_IOC_GET_SYS_COUNT: {
        __u32 cnt;
        mutex_lock(&g_scth.cfg_mutex);
        cnt = scth_cfg_sys_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);
        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_SYS_LIST: {
        struct scth_list_req req;
        __u32 *kbuf;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        needed = scth_cfg_sys_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.cap < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
            return -ENOSPC;
        }

        kbuf = kcalloc(needed ? needed : 1, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf) return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        wrote = scth_cfg_fill_sys_list(&g_scth.cfg, kbuf, needed);
        mutex_unlock(&g_scth.cfg_mutex);

        req.count = wrote;
        if (wrote && copy_to_user((void __user *)(uintptr_t)req.ptr, kbuf, wrote * sizeof(*kbuf))) {
            kfree(kbuf);
            return -EFAULT;
        }
        kfree(kbuf);

        if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}