#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "scth_internal.h"

static int copy_u32_to_user(unsigned long arg, __u32 v)
{
    if (copy_to_user((void __user *)arg, &v, sizeof(v)))
        return -EFAULT;
    return 0;
}

static int copy_u32_from_user(unsigned long arg, __u32 *v)
{
    if (copy_from_user(v, (void __user *)arg, sizeof(*v)))
        return -EFAULT;
    return 0;
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
        unsigned long flags;

        if (copy_u32_from_user(arg, &v))
            return -EFAULT;
        if (v == 0)
            return -EINVAL;

        spin_lock_irqsave(&g_scth.lock, flags);
        g_scth.max_pending = v;
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return 0;
    }

    case SCTH_IOC_SET_POLICY: {
        __u32 v;
        unsigned long flags;

        if (copy_u32_from_user(arg, &v))
            return -EFAULT;
        if (v != SCTH_POLICY_FIFO_STRICT && v != SCTH_POLICY_WAKE_RACE)
            return -EINVAL;

        spin_lock_irqsave(&g_scth.lock, flags);
        g_scth.policy_pending = (__u8)v;
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return 0;
    }

    case SCTH_IOC_RESET_STATS: {
        unsigned long flags;

        spin_lock_irqsave(&g_scth.lock, flags);

        g_scth.peak_delay_ns = 0;
        memset(g_scth.peak_comm, 0, sizeof(g_scth.peak_comm));
        g_scth.peak_euid = 0;

        g_scth.peak_blocked_threads = 0;
        g_scth.blocked_sum_samples = 0;
        g_scth.blocked_num_samples = 0;

        g_scth.peak_fifo_qlen = 0;

        spin_unlock_irqrestore(&g_scth.lock, flags);

        atomic64_set(&g_scth.total_tracked, 0);
        atomic64_set(&g_scth.total_immediate, 0);
        atomic64_set(&g_scth.total_delayed, 0);
        atomic64_set(&g_scth.total_aborted, 0);

        atomic64_set(&g_scth.delay_sum_ns, 0);
        atomic64_set(&g_scth.delay_num, 0);

        return 0;
    }

    case SCTH_IOC_GET_CFG: {
        struct scth_cfg cfg;
        unsigned long flags;

        memset(&cfg, 0, sizeof(cfg));

        spin_lock_irqsave(&g_scth.lock, flags);
        cfg.abi_version    = 1;
        cfg.monitor_on     = g_scth.monitor_on ? 1 : 0;
        cfg.epoch_id       = g_scth.epoch_id;
        cfg.max_active     = g_scth.max_active;
        cfg.max_pending    = g_scth.max_pending;
        cfg.policy_active  = g_scth.policy_active;
        cfg.policy_pending = g_scth.policy_pending;
        spin_unlock_irqrestore(&g_scth.lock, flags);

        if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)))
            return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_STATS: {
        struct scth_stats st;
        unsigned long flags;

        memset(&st, 0, sizeof(st));
        st.abi_version = SCTH_ABI_VERSION;

        spin_lock_irqsave(&g_scth.lock, flags);
        st.peak_delay_ns = g_scth.peak_delay_ns;
        strscpy(st.peak_comm, g_scth.peak_comm, SCTH_COMM_LEN);
        st.peak_euid = g_scth.peak_euid;

        st.peak_blocked_threads = g_scth.peak_blocked_threads;
        st.blocked_sum_samples  = g_scth.blocked_sum_samples;
        st.blocked_num_samples  = g_scth.blocked_num_samples;

        st.peak_fifo_qlen    = g_scth.peak_fifo_qlen;
        st.current_fifo_qlen = g_scth.fifo_qlen;

        st.last_epoch_used = g_scth.epoch_used;
        st.max_active      = g_scth.max_active;
        st.epoch_id        = g_scth.epoch_id;

        st.policy_active  = g_scth.policy_active;
        st.policy_pending = g_scth.policy_pending;
        spin_unlock_irqrestore(&g_scth.lock, flags);

        st.delay_sum_ns    = (unsigned long long)atomic64_read(&g_scth.delay_sum_ns);
        st.delay_num       = (unsigned long long)atomic64_read(&g_scth.delay_num);

        st.total_tracked   = (unsigned long long)atomic64_read(&g_scth.total_tracked);
        st.total_immediate = (unsigned long long)atomic64_read(&g_scth.total_immediate);
        st.total_delayed   = (unsigned long long)atomic64_read(&g_scth.total_delayed);
        st.total_aborted   = (unsigned long long)atomic64_read(&g_scth.total_aborted);

        if (copy_to_user((void __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        return 0;
    }

    /* ---- PROG ---- */
    case SCTH_IOC_ADD_PROG: {
        struct scth_prog_arg a;
        int ret;

        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_prog(&g_scth.cfg, a.comm);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_PROG: {
        struct scth_prog_arg a;
        int ret;

        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_prog(&g_scth.cfg, a.comm);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_PROG_COUNT: {
        __u32 c;

        mutex_lock(&g_scth.cfg_mutex);
        c = scth_cfg_prog_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        return copy_u32_to_user(arg, c);
    }

    case SCTH_IOC_GET_PROG_LIST: {
        struct scth_list_req r;
        struct scth_prog_arg *buf;
        __u32 n;

        if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
            return -EFAULT;

        if (r.cap == 0 || r.ptr == 0) {
            r.count = 0;
            if (copy_to_user((void __user *)arg, &r, sizeof(r)))
                return -EFAULT;
            return 0;
        }

        buf = kmalloc_array(r.cap, sizeof(*buf), GFP_KERNEL);
        if (!buf)
            return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        n = scth_cfg_fill_prog_list(&g_scth.cfg, buf, r.cap);
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)(uintptr_t)r.ptr, buf, n * sizeof(*buf))) {
            kfree(buf);
            return -EFAULT;
        }
        kfree(buf);

        r.count = n;
        if (copy_to_user((void __user *)arg, &r, sizeof(r)))
            return -EFAULT;
        return 0;
    }

    /* ---- UID ---- */
    case SCTH_IOC_ADD_UID: {
        struct scth_uid_arg a;
        int ret;

        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_uid(&g_scth.cfg, a.euid);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_UID: {
        struct scth_uid_arg a;
        int ret;

        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_uid(&g_scth.cfg, a.euid);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_UID_COUNT: {
        __u32 c;

        mutex_lock(&g_scth.cfg_mutex);
        c = scth_cfg_uid_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        return copy_u32_to_user(arg, c);
    }

    case SCTH_IOC_GET_UID_LIST: {
        struct scth_list_req r;
        __u32 *buf;
        __u32 n;

        if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
            return -EFAULT;

        if (r.cap == 0 || r.ptr == 0) {
            r.count = 0;
            if (copy_to_user((void __user *)arg, &r, sizeof(r)))
                return -EFAULT;
            return 0;
        }

        buf = kmalloc_array(r.cap, sizeof(*buf), GFP_KERNEL);
        if (!buf)
            return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        n = scth_cfg_fill_uid_list(&g_scth.cfg, buf, r.cap);
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)(uintptr_t)r.ptr, buf, n * sizeof(*buf))) {
            kfree(buf);
            return -EFAULT;
        }
        kfree(buf);

        r.count = n;
        if (copy_to_user((void __user *)arg, &r, sizeof(r)))
            return -EFAULT;
        return 0;
    }

    /* ---- SYS ---- */
    case SCTH_IOC_ADD_SYS: {
        struct scth_sys_arg a;
        int ret, hret;

        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        /* prima aggiungo in cfg */
        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_sys(&g_scth.cfg, a.nr);
        mutex_unlock(&g_scth.cfg_mutex);
        if (ret)
            return ret;

        /* poi hook */
        hret = scth_hook_install(a.nr);
        if (hret && hret != -EALREADY && hret != -EEXIST) {
            /* rollback cfg */
            mutex_lock(&g_scth.cfg_mutex);
            scth_cfg_del_sys(&g_scth.cfg, a.nr);
            mutex_unlock(&g_scth.cfg_mutex);
            return hret;
        }

        return 0;
    }

    case SCTH_IOC_DEL_SYS: {
        struct scth_sys_arg a;
        int ret;

        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        /* unhook anche se del in cfg fallisce? qui facciamo prima cfg */
        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_sys(&g_scth.cfg, a.nr);
        mutex_unlock(&g_scth.cfg_mutex);
        if (ret)
            return ret;

        scth_hook_remove(a.nr);
        return 0;
    }

    case SCTH_IOC_GET_SYS_COUNT: {
        __u32 c;

        mutex_lock(&g_scth.cfg_mutex);
        c = scth_cfg_sys_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        return copy_u32_to_user(arg, c);
    }

    case SCTH_IOC_GET_SYS_LIST: {
        struct scth_list_req r;
        __u32 *buf;
        __u32 n;

        if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
            return -EFAULT;

        if (r.cap == 0 || r.ptr == 0) {
            r.count = 0;
            if (copy_to_user((void __user *)arg, &r, sizeof(r)))
                return -EFAULT;
            return 0;
        }

        buf = kmalloc_array(r.cap, sizeof(*buf), GFP_KERNEL);
        if (!buf)
            return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        n = scth_cfg_fill_sys_list(&g_scth.cfg, buf, r.cap);
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)(uintptr_t)r.ptr, buf, n * sizeof(*buf))) {
            kfree(buf);
            return -EFAULT;
        }
        kfree(buf);

        r.count = n;
        if (copy_to_user((void __user *)arg, &r, sizeof(r)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}