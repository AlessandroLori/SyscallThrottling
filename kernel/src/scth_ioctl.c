#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include "scth_internal.h"

/* Contorllo amministrativo del modulo. Gestisce le variazioni di settings passati da temrinale, valida argomenti passati e permessi
    root */

static inline bool scth_is_root(void)
// Controlla se utente è root
{
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static void scth_stats_reset_locked(void)
// Resetta tutte statistiche del modulo
{
    /* peak delay */
    g_scth.peak_delay_ns = 0;
    memset(g_scth.peak_comm, 0, sizeof(g_scth.peak_comm));
    g_scth.peak_euid = 0;

    /* blocked stats */
    g_scth.peak_blocked_threads = 0;
    g_scth.blocked_sum_samples = 0;
    g_scth.blocked_num_samples = 0;

    /* fifo stats */
    g_scth.peak_fifo_qlen = 0;

    /* totals */
    atomic64_set(&g_scth.total_tracked, 0);
    atomic64_set(&g_scth.total_immediate, 0);
    atomic64_set(&g_scth.total_delayed, 0);
    atomic64_set(&g_scth.total_aborted, 0);
    atomic64_set(&g_scth.delay_sum_ns, 0);
    atomic64_set(&g_scth.delay_num, 0);
}

static struct scth_cfg_store *scth_cfg_current_locked(void)
{
    return rcu_dereference_protected(g_scth.cfg,
                                     lockdep_is_held(&g_scth.cfg_mutex));
}

static int scth_cfg_update_prog_locked(const char comm[SCTH_COMM_LEN], bool add)
{
    struct scth_cfg_store *old_cfg, *new_cfg;
    int ret;

    new_cfg = scth_cfg_clone(scth_cfg_current_locked(), GFP_KERNEL);
    if (!new_cfg)
        return -ENOMEM;

    ret = add ? scth_cfg_add_prog(new_cfg, comm)
              : scth_cfg_del_prog(new_cfg, comm);
    if (ret) {
        scth_cfg_destroy(new_cfg);
        return ret;
    }

    old_cfg = scth_cfg_current_locked();
    rcu_assign_pointer(g_scth.cfg, new_cfg);
    scth_cfg_retire(old_cfg);
    return 0;
}

static int scth_cfg_update_uid_locked(__u32 euid, bool add)
{
    struct scth_cfg_store *old_cfg, *new_cfg;
    int ret;

    new_cfg = scth_cfg_clone(scth_cfg_current_locked(), GFP_KERNEL);
    if (!new_cfg)
        return -ENOMEM;

    ret = add ? scth_cfg_add_uid(new_cfg, euid)
              : scth_cfg_del_uid(new_cfg, euid);
    if (ret) {
        scth_cfg_destroy(new_cfg);
        return ret;
    }

    old_cfg = scth_cfg_current_locked();
    rcu_assign_pointer(g_scth.cfg, new_cfg);
    scth_cfg_retire(old_cfg);
    return 0;
}

static int scth_cfg_update_sys_locked(__u32 nr, bool add)
{
    struct scth_cfg_store *old_cfg, *new_cfg;
    int ret, hook_ret;

    new_cfg = scth_cfg_clone(scth_cfg_current_locked(), GFP_KERNEL);
    if (!new_cfg)
        return -ENOMEM;

    ret = add ? scth_cfg_add_sys(new_cfg, nr)
              : scth_cfg_del_sys(new_cfg, nr);
    if (ret) {
        scth_cfg_destroy(new_cfg);
        return ret;
    }

    if (add) {
        /*
         * ADD_SYS:
         * installo hook prima di pubblicare la nuova snapshot.
         * Finché non pubblico il nuovo sys_bitmap, il wrapper bypassa.
         */
        hook_ret = scth_hook_install(nr);
        if (hook_ret) {
            scth_cfg_destroy(new_cfg);
            return hook_ret;
        }

        old_cfg = scth_cfg_current_locked();
        rcu_assign_pointer(g_scth.cfg, new_cfg);
        scth_cfg_retire(old_cfg);
        return 0;
    }

    /*
     * DEL_SYS:
     * pubblico prima la snapshot senza la syscall,
     * poi rimuovo l'hook.
     * Nel transitorio il wrapper vede sys non registrata e bypassa.
     */
    old_cfg = scth_cfg_current_locked();
    rcu_assign_pointer(g_scth.cfg, new_cfg);

    hook_ret = scth_hook_remove(nr);
    if (hook_ret) {
        /*
         * rollback publish:
         * new_cfg può essere già stata vista da reader RCU, quindi NON va distrutta
         * subito: va ritirata via call_rcu().
         */
        rcu_assign_pointer(g_scth.cfg, old_cfg);
        scth_cfg_retire(new_cfg);
        return hook_ret;
    }

    scth_cfg_retire(old_cfg);
    return 0;
}

long scth_ioctl_dispatch(unsigned int cmd, unsigned long arg)
// Dispatcher centrale di ioctl, riceve comando e sceglie operaizone da eseguire
{
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
        unsigned long flags;

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
        unsigned long flags;

        memset(&c, 0, sizeof(c));

        spin_lock_irqsave(&g_scth.lock, flags);
        c.abi_version    = SCTH_ABI_VERSION;
        c.monitor_on     = g_scth.monitor_on ? 1 : 0;
        c.epoch_id       = g_scth.epoch_id;
        c.max_active     = g_scth.max_active;
        c.max_pending    = g_scth.max_pending;
        c.policy_active  = g_scth.policy_active;
        c.policy_pending = g_scth.policy_pending;
        spin_unlock_irqrestore(&g_scth.lock, flags);

        if (copy_to_user((void __user *)arg, &c, sizeof(c)))
            return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_STATS: {
        struct scth_stats s;
        unsigned long flags;

        memset(&s, 0, sizeof(s));
        s.abi_version = SCTH_ABI_VERSION;

        spin_lock_irqsave(&g_scth.lock, flags);

        s.peak_delay_ns        = g_scth.peak_delay_ns;
        strscpy(s.peak_comm, g_scth.peak_comm, SCTH_COMM_LEN);
        s.peak_euid            = g_scth.peak_euid;

        s.peak_blocked_threads = g_scth.peak_blocked_threads;
        s.blocked_sum_samples  = g_scth.blocked_sum_samples;
        s.blocked_num_samples  = g_scth.blocked_num_samples;

        s.peak_fifo_qlen       = g_scth.peak_fifo_qlen;
        s.current_fifo_qlen    = g_scth.fifo_qlen;

        s.last_epoch_used      = g_scth.epoch_used;
        s.max_active           = g_scth.max_active;
        s.epoch_id             = g_scth.epoch_id;

        s.policy_active        = g_scth.policy_active;
        s.policy_pending       = g_scth.policy_pending;

        spin_unlock_irqrestore(&g_scth.lock, flags);

        s.total_tracked        = (__u64)atomic64_read(&g_scth.total_tracked);
        s.total_immediate      = (__u64)atomic64_read(&g_scth.total_immediate);
        s.total_delayed        = (__u64)atomic64_read(&g_scth.total_delayed);
        s.total_aborted        = (__u64)atomic64_read(&g_scth.total_aborted);

        s.delay_sum_ns         = (__u64)atomic64_read(&g_scth.delay_sum_ns);
        s.delay_num            = (__u64)atomic64_read(&g_scth.delay_num);

        if (copy_to_user((void __user *)arg, &s, sizeof(s)))
            return -EFAULT;
        return 0;
    }

    /* ---- PROG ---- */
    case SCTH_IOC_ADD_PROG: {
        struct scth_prog_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_update_prog_locked(a.comm, true);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_PROG: {
        struct scth_prog_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_update_prog_locked(a.comm, false);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_PROG_COUNT: {
        __u32 cnt;
        struct scth_cfg_store *cfg;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        cnt = cfg ? scth_cfg_prog_count(cfg) : 0;
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_PROG_LIST: {
        struct scth_list_req req;
        struct scth_prog_arg *kbuf;
        struct scth_cfg_store *cfg;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        needed = cfg ? scth_cfg_prog_count(cfg) : 0;
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.cap < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
            return -ENOSPC;
        }

        kbuf = kcalloc(needed ? needed : 1, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf) return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        wrote = cfg ? scth_cfg_fill_prog_list(cfg, kbuf, needed) : 0;
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
        ret = scth_cfg_update_uid_locked(a.euid, true);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_UID: {
        struct scth_uid_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_update_uid_locked(a.euid, false);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_UID_COUNT: {
        __u32 cnt;
        struct scth_cfg_store *cfg;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        cnt = cfg ? scth_cfg_uid_count(cfg) : 0;
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_UID_LIST: {
        struct scth_list_req req;
        __u32 *kbuf;
        struct scth_cfg_store *cfg;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        needed = cfg ? scth_cfg_uid_count(cfg) : 0;
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.cap < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
            return -ENOSPC;
        }

        kbuf = kcalloc(needed ? needed : 1, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf) return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        wrote = cfg ? scth_cfg_fill_uid_list(cfg, kbuf, needed) : 0;
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
        ret = scth_cfg_update_sys_locked(a.nr, true);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_DEL_SYS: {
        struct scth_sys_arg a;
        int ret;
        if (!scth_is_root()) return -EPERM;
        if (copy_from_user(&a, (void __user *)arg, sizeof(a))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        ret = scth_cfg_update_sys_locked(a.nr, false);
        mutex_unlock(&g_scth.cfg_mutex);
        return ret;
    }

    case SCTH_IOC_GET_SYS_COUNT: {
        __u32 cnt;
        struct scth_cfg_store *cfg;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        cnt = cfg ? scth_cfg_sys_count(cfg) : 0;
        mutex_unlock(&g_scth.cfg_mutex);

        if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt))) return -EFAULT;
        return 0;
    }

    case SCTH_IOC_GET_SYS_LIST: {
        struct scth_list_req req;
        __u32 *kbuf;
        struct scth_cfg_store *cfg;
        __u32 needed, wrote;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        needed = cfg ? scth_cfg_sys_count(cfg) : 0;
        mutex_unlock(&g_scth.cfg_mutex);

        if (req.cap < needed) {
            req.count = needed;
            if (copy_to_user((void __user *)arg, &req, sizeof(req))) return -EFAULT;
            return -ENOSPC;
        }

        kbuf = kcalloc(needed ? needed : 1, sizeof(*kbuf), GFP_KERNEL);
        if (!kbuf) return -ENOMEM;

        mutex_lock(&g_scth.cfg_mutex);
        cfg = scth_cfg_current_locked();
        wrote = cfg ? scth_cfg_fill_sys_list(cfg, kbuf, needed) : 0;
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