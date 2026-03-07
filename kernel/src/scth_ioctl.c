#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "scth_internal.h"

static inline bool scth_is_root(void)
{
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static void scth_stats_reset_locked(void)
{
    g_scth.peak_delay_ns = 0;
    memset(g_scth.peak_comm, 0, sizeof(g_scth.peak_comm));
    g_scth.peak_euid = 0;

    g_scth.peak_blocked_threads = 0;
    g_scth.blocked_sum_samples = 0;
    g_scth.blocked_num_samples = 0;
    g_scth.current_blocked_threads = 0;
}

static void normalize_comm(char out[SCTH_COMM_LEN], const char in[SCTH_COMM_LEN])
{
    /* tronca a 15 e NUL-terminate */
    memset(out, 0, SCTH_COMM_LEN);
    memcpy(out, in, SCTH_COMM_LEN);
    out[SCTH_COMM_LEN - 1] = '\0';
}

long scth_ioctl_dispatch(unsigned int cmd, unsigned long arg)
{
    unsigned long flags;

    switch (cmd) {
    case SCTH_IOC_ON:
        if (!scth_is_root())
            return -EPERM;
        return scth_monitor_on();

    case SCTH_IOC_OFF:
        if (!scth_is_root())
            return -EPERM;
        return scth_monitor_off();

    case SCTH_IOC_SET_MAX: {
        __u32 v;
        if (!scth_is_root())
            return -EPERM;
        if (copy_from_user(&v, (void __user *)arg, sizeof(v)))
            return -EFAULT;
        if (v == 0)
            return -EINVAL;

        spin_lock_irqsave(&g_scth.lock, flags);
        g_scth.max_pending = v;
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return 0;
    }

    case SCTH_IOC_SET_POLICY: {
        __u8 p;
        if (!scth_is_root())
            return -EPERM;
        if (copy_from_user(&p, (void __user *)arg, sizeof(p)))
            return -EFAULT;
        if (p != SCTH_POLICY_FIFO_STRICT && p != SCTH_POLICY_WAKE_RACE)
            return -EINVAL;

        spin_lock_irqsave(&g_scth.lock, flags);
        g_scth.policy_pending = p;
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return 0;
    }

    case SCTH_IOC_RESET_STATS:
        if (!scth_is_root())
            return -EPERM;
        spin_lock_irqsave(&g_scth.lock, flags);
        scth_stats_reset_locked();
        spin_unlock_irqrestore(&g_scth.lock, flags);
        return 0;

    case SCTH_IOC_GET_CFG: {
        struct scth_cfg cfg;

        spin_lock_irqsave(&g_scth.lock, flags);
        cfg.abi_version    = SCTH_ABI_VERSION;
        cfg.monitor_on     = g_scth.monitor_on ? 1 : 0;
        cfg.policy_active  = g_scth.policy_active;
        cfg.policy_pending = g_scth.policy_pending;
        cfg.max_active     = g_scth.max_active;
        cfg.max_pending    = g_scth.max_pending;
        cfg.epoch_id       = g_scth.epoch_id;
        spin_unlock_irqrestore(&g_scth.lock, flags);

        if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)))
            return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_STATS: {
        struct scth_stats st;

        spin_lock_irqsave(&g_scth.lock, flags);
        memset(&st, 0, sizeof(st));
        st.abi_version = SCTH_ABI_VERSION;

        st.peak_delay_ns = g_scth.peak_delay_ns;
        memcpy(st.peak_comm, g_scth.peak_comm, SCTH_COMM_LEN);
        st.peak_euid = g_scth.peak_euid;

        st.peak_blocked_threads = g_scth.peak_blocked_threads;
        st.blocked_sum_samples  = g_scth.blocked_sum_samples;
        st.blocked_num_samples  = g_scth.blocked_num_samples;
        st.current_blocked_threads = g_scth.current_blocked_threads;

        spin_unlock_irqrestore(&g_scth.lock, flags);

        if (copy_to_user((void __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        return 0;
    }

    /* ---------------- Programs ---------------- */

    case SCTH_IOC_ADD_PROG: {
        struct scth_prog_arg a;
        char comm[SCTH_COMM_LEN];
        int ret;

        if (!scth_is_root())
            return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        normalize_comm(comm, a.comm);

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_prog(&g_scth.cfg, comm);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_PROG: {
        struct scth_prog_arg a;
        char comm[SCTH_COMM_LEN];
        int ret;

        if (!scth_is_root())
            return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        normalize_comm(comm, a.comm);

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_prog(&g_scth.cfg, comm);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_PROG_COUNT: {
        __u32 cnt;
        mutex_lock(&g_scth.cfg_mutex);
        cnt = scth_cfg_prog_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt)))
            return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_PROG_LIST: {
        struct scth_list_req req;
        struct scth_prog_arg *kbuf = NULL;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        needed = scth_cfg_prog_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.capacity < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req)))
                return -EFAULT;
            return -ENOSPC;
        }

        if (needed && !req.user_ptr)
            return -EINVAL;

        if (needed == 0) {
            req.count = 0;
            if (copy_to_user((void __user *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }

        kbuf = kcalloc(needed, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf)
            return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        wrote = scth_cfg_fill_prog_list(&g_scth.cfg, kbuf, needed);
        mutex_unlock(&g_scth.cfg_mutex);

        req.count = wrote;

        {
            void __user *uptr = (void __user *)(unsigned long)req.user_ptr;
            if (wrote && copy_to_user(uptr, kbuf, wrote * sizeof(*kbuf))) {
                kfree(kbuf);
                return -EFAULT;
            }
        }

        kfree(kbuf);

        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;
        return 0;
    }

    /* ---------------- UIDs ---------------- */

    case SCTH_IOC_ADD_UID: {
        struct scth_uid_arg a;
        int ret;

        if (!scth_is_root())
            return -EPERM;
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

        if (!scth_is_root())
            return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

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

        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt)))
            return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_UID_LIST: {
        struct scth_list_req req;
        __u32 *kbuf = NULL;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        needed = scth_cfg_uid_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.capacity < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req)))
                return -EFAULT;
            return -ENOSPC;
        }

        if (needed && !req.user_ptr)
            return -EINVAL;

        if (needed == 0) {
            req.count = 0;
            if (copy_to_user((void __user *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }

        kbuf = kcalloc(needed, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf)
            return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        wrote = scth_cfg_fill_uid_list(&g_scth.cfg, kbuf, needed);
        mutex_unlock(&g_scth.cfg_mutex);

        req.count = wrote;

        {
            void __user *uptr = (void __user *)(unsigned long)req.user_ptr;
            if (wrote && copy_to_user(uptr, kbuf, wrote * sizeof(*kbuf))) {
                kfree(kbuf);
                return -EFAULT;
            }
        }

        kfree(kbuf);

        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;
        return 0;
    }

    /* ---------------- Syscalls ---------------- */

    case SCTH_IOC_ADD_SYS: {
        struct scth_sys_arg a;
        int ret;

        if (!scth_is_root())
            return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_add_sys(&g_scth.cfg, a.nr);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_SYS: {
        struct scth_sys_arg a;
        int ret;

        if (!scth_is_root())
            return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_del_sys(&g_scth.cfg, a.nr);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_SYS_COUNT: {
        __u32 cnt;
        mutex_lock(&g_scth.cfg_mutex);
        cnt = scth_cfg_sys_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt)))
            return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_SYS_LIST: {
        struct scth_list_req req;
        __u32 *kbuf = NULL;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        needed = scth_cfg_sys_count(&g_scth.cfg);
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.capacity < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req)))
                return -EFAULT;
            return -ENOSPC;
        }

        if (needed && !req.user_ptr)
            return -EINVAL;

        if (needed == 0) {
            req.count = 0;
            if (copy_to_user((void __user *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }

        kbuf = kcalloc(needed, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf)
            return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        wrote = scth_cfg_fill_sys_list(&g_scth.cfg, kbuf, needed);
        mutex_unlock(&g_scth.cfg_mutex);

        req.count = wrote;

        {
            void __user *uptr = (void __user *)(unsigned long)req.user_ptr;
            if (wrote && copy_to_user(uptr, kbuf, wrote * sizeof(*kbuf))) {
                kfree(kbuf);
                return -EFAULT;
            }
        }

        kfree(kbuf);

        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}