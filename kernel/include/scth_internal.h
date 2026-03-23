#pragma once

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/rcupdate.h>

#include "scth_ioctl.h"

#define SCTH_DEV_NAME "scthrottle"

/* NR_syscalls (fallback se non definito) */
#include <asm/unistd.h>
#ifndef NR_syscalls
#define NR_syscalls 1024
#endif

#define SCTH_PROG_HT_BITS 8  /* 256 buckets */
#define SCTH_UID_HT_BITS  8

#ifndef SCTH_LOG_GRANT
#define SCTH_LOG_GRANT(...) do { } while (0)
#endif

struct scth_prog_ent {
    struct hlist_node node;
    char comm[SCTH_COMM_LEN];
};

struct scth_uid_ent {
    struct hlist_node node;
    __u32 euid;
};

struct scth_cfg_store {
    struct rcu_head rcu;

    struct hlist_head prog_ht[1 << SCTH_PROG_HT_BITS];
    struct hlist_head uid_ht[1 << SCTH_UID_HT_BITS];

    unsigned long sys_bitmap[BITS_TO_LONGS(NR_syscalls)];

    __u32 prog_count;
    __u32 uid_count;
    __u32 sys_count;
};

struct scth_waiter {
    struct list_head node;
    wait_queue_head_t wq;
    bool granted;
    bool aborted;
    __u64 ticket;
};

/* Stato globale */
struct scth_state {
    /* lock principale (timer + coda FIFO + peak stats + blocked) */
    spinlock_t lock;

    bool monitor_on;

    __u32 max_active;     /* budget effettivo per epoca (attivo) */
    __u32 max_pending;    /* configurato via setmax (pending) */

    __u8  policy_active;
    __u8  policy_pending;

    __u64 epoch_id;
    __u32 epoch_used;     /* quanti “slot” già consumati in questa epoca (FIFO strict) */

    /* WAKE_RACE: token per epoca */
    atomic_t epoch_tokens;
    wait_queue_head_t epoch_wq;

    struct timer_list epoch_timer;

    unsigned long sys_call_table_addr; /* da USCTM */

    /* shutdown / unload coordination */
    bool stopping;
    atomic_t active_wrappers;
    wait_queue_head_t unload_wq;

    /* config sets (prog/uid/sys) */
    struct mutex cfg_mutex;
    struct scth_cfg_store __rcu *cfg;

    /* FIFO_STRICT: coda globale waiters */
    struct list_head fifo_q;
    __u32 fifo_qlen;
    atomic64_t fifo_seq; /* ticket progressivo (FIFO) */

    /* ---- stats “peak” ---- */
    __u64 peak_delay_ns;
    char  peak_comm[SCTH_COMM_LEN];
    __u32 peak_euid;

    __u32 peak_blocked_threads;
    __u32 current_blocked_threads;

    __u32 peak_fifo_qlen;

    /* media blocked: campionamento (timer) */
    __u64 blocked_sum_samples;
    __u64 blocked_num_samples;

    /* ---- stats “totali” (atomiche) ---- */
    atomic64_t total_tracked;     /* matchati (monitor_on + prog/uid + syscall) */
    atomic64_t total_immediate;   /* passati subito (FIFO IMM o token immediato) */
    atomic64_t total_delayed;     /* hanno atteso (FIFO wait o WAKE_RACE wait) */
    atomic64_t total_aborted;     /* usciti per signal/monitor_off durante attesa */

    atomic64_t delay_sum_ns;      /* somma delay (solo delayed) */
    atomic64_t delay_num;         /* quanti delayed */
};

extern struct scth_state g_scth;

/* device */
int  scth_dev_init(void);
void scth_dev_exit(void);

/* ioctl */
long scth_ioctl_dispatch(unsigned int cmd, unsigned long arg);

/* monitor control */
int  scth_monitor_on(void);
int  scth_monitor_off(void);

/* cfg API */
struct scth_cfg_store *scth_cfg_alloc_empty(gfp_t gfp);
struct scth_cfg_store *scth_cfg_clone(const struct scth_cfg_store *src, gfp_t gfp);
void scth_cfg_destroy(struct scth_cfg_store *c);
void scth_cfg_retire(struct scth_cfg_store *c);

int  scth_cfg_add_prog(struct scth_cfg_store *c, const char comm[SCTH_COMM_LEN]);
int  scth_cfg_del_prog(struct scth_cfg_store *c, const char comm[SCTH_COMM_LEN]);
bool scth_cfg_has_prog(struct scth_cfg_store *c, const char comm[SCTH_COMM_LEN]);
__u32 scth_cfg_prog_count(struct scth_cfg_store *c);
__u32 scth_cfg_fill_prog_list(struct scth_cfg_store *c, struct scth_prog_arg *out, __u32 cap);

int  scth_cfg_add_uid(struct scth_cfg_store *c, __u32 euid);
int  scth_cfg_del_uid(struct scth_cfg_store *c, __u32 euid);
bool scth_cfg_has_uid(struct scth_cfg_store *c, __u32 euid);
__u32 scth_cfg_uid_count(struct scth_cfg_store *c);
__u32 scth_cfg_fill_uid_list(struct scth_cfg_store *c, __u32 *out, __u32 cap);

int  scth_cfg_add_sys(struct scth_cfg_store *c, __u32 nr);
int  scth_cfg_del_sys(struct scth_cfg_store *c, __u32 nr);
bool scth_cfg_has_sys(struct scth_cfg_store *c, __u32 nr);
__u32 scth_cfg_sys_count(struct scth_cfg_store *c);
__u32 scth_cfg_fill_sys_list(struct scth_cfg_store *c, __u32 *out, __u32 cap);

/* hook API */
int  scth_hook_install(__u32 nr);
int  scth_hook_remove(__u32 nr);
void scth_hook_remove_all(void);