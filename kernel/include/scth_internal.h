#pragma once

#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/string.h>

#include "scth_ioctl.h"

#define SCTH_DEV_NAME "scthrottle"

/* NR_syscalls (fallback se non definito) */
#include <asm/unistd.h>
#ifndef NR_syscalls
#define NR_syscalls 1024
#endif

#define SCTH_PROG_HT_BITS 8  /* 256 buckets */
#define SCTH_UID_HT_BITS  8

/* ---- DEBUG GRANT LOG ----
 * Metti 0 quando hai finito di debuggare FIFO_STRICT
 */
#define SCTH_DEBUG_GRANT 1

#if SCTH_DEBUG_GRANT
#define SCTH_LOG_GRANT(_tag, _epoch, _ticket, _used, _max, _qlen) \
    pr_info("scthrottle: GRANT[%s] epoch=%llu ticket=%llu used=%u/%u qlen=%u\n", \
            (_tag), (unsigned long long)(_epoch), (unsigned long long)(_ticket), \
            (unsigned int)(_used), (unsigned int)(_max), (unsigned int)(_qlen))
#else
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
    struct hlist_head prog_ht[1 << SCTH_PROG_HT_BITS];
    struct hlist_head uid_ht[1 << SCTH_UID_HT_BITS];
    unsigned long sys_bitmap[BITS_TO_LONGS(NR_syscalls)];

    __u32 prog_count;
    __u32 uid_count;
    __u32 sys_count;
};

/* waiter FIFO_STRICT: sta sullo stack del thread bloccato */
struct scth_waiter {
    struct list_head node;   /* usato sia in fifo_q che in lista locale to_wake */
    wait_queue_head_t wq;
    bool granted;
    bool aborted;
    __u64 ticket;
};

/* Stato globale */
struct scth_state {
    /* lock per epoch/max/policy + queue FIFO + peak stats */
    spinlock_t lock;

    bool monitor_on;

    __u32 max_active;    /* applicato (epoch corrente) */
    __u32 max_pending;   /* richiesto via ioctl, applicato a cambio epoca */

    __u8 policy_active;  /* applicato (epoch corrente) */
    __u8 policy_pending; /* richiesto via ioctl, applicato a cambio epoca */

    __u64 epoch_id;

    /* FIFO_STRICT: quanti slot già consumati in questa epoca */
    __u32 epoch_used;

    /* WAKE_RACE: token per epoca */
    atomic_t epoch_tokens;

    /* waitqueue per “cambio epoca” (serve a WAKE_RACE) */
    wait_queue_head_t epoch_wq;

    struct timer_list epoch_timer;

    unsigned long sys_call_table_addr; /* da USCTM via script (M3) */

    /* configurazione sets, protetta da mutex */
    struct mutex cfg_mutex;
    struct scth_cfg_store cfg;

    /* FIFO_STRICT: coda globale */
    struct list_head fifo_q;
    __u32 fifo_qlen;
    atomic64_t fifo_seq;

    /* ---- STATS (protette/aggiornate come sotto) ---- */

    /* peak delay (serve anche comm+uid => aggiornata sotto lock) */
    __u64 peak_delay_ns;
    char  peak_comm[SCTH_COMM_LEN];
    __u32 peak_euid;

    /* blocked threads */
    __u32 peak_blocked_threads;     /* peak di current_blocked_threads */
    __u64 blocked_sum_samples;      /* campioni per media */
    __u64 blocked_num_samples;
    __u32 current_blocked_threads;  /* quanti sono attualmente in wait */

    /* FIFO queue peak */
    __u32 peak_fifo_qlen;

    /* contatori (atomici, così non costringiamo lock su WAKE_RACE) */
    atomic64_t total_tracked;       /* matchati (monitor_on + prog/uid) */
    atomic64_t total_immediate;     /* passati subito (FIFO IMM o token immediato) */
    atomic64_t total_delayed;       /* hanno atteso (FIFO wait o WAKE_RACE wait) */
    atomic64_t total_aborted;       /* usciti per signal/monitor_off */

    atomic64_t delay_sum_ns;        /* somma dei delay */
    atomic64_t delay_num;           /* quanti delay (per media) */
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
void scth_cfg_init(struct scth_cfg_store *c);
void scth_cfg_destroy(struct scth_cfg_store *c);

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

/* hook */
int  scth_hook_install(__u32 nr);
int  scth_hook_remove(__u32 nr);
void scth_hook_remove_all(void);