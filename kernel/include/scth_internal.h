#pragma once

#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/wait.h>
#include <linux/atomic.h>

#include "scth_ioctl.h"

#define SCTH_DEV_NAME "scthrottle"

/* NR_syscalls (fallback se non definito) */
#include <asm/unistd.h>
#ifndef NR_syscalls
#define NR_syscalls 1024
#endif

#define SCTH_PROG_HT_BITS 8  /* 256 buckets */
#define SCTH_UID_HT_BITS  8

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

/* Stato globale */
struct scth_state {
    spinlock_t lock;          /* per timer+variabili epoch/max/policy/stats */
    atomic_t epoch_tokens;   /* slot/gettoni rimasti nella epoca corrente */
    bool monitor_on;

    __u32 epoch_used;

    __u32 max_active;
    __u32 max_pending;

    __u8 policy_active;
    __u8 policy_pending;

    __u64 epoch_id;
    
    /* stats (M2: ancora get/reset, update arriverà con throttling) */
    __u64 peak_delay_ns;
    char  peak_comm[SCTH_COMM_LEN];
    __u32 peak_euid;

    __u32 peak_blocked_threads;
    __u64 blocked_sum_samples;
    __u64 blocked_num_samples;
    __u32 current_blocked_threads;

    wait_queue_head_t epoch_wq;

    struct timer_list epoch_timer;

    unsigned long sys_call_table_addr; /* da USCTM via script (M3) */

    /* nuovo: configurazione sets, protetta da mutex */
    struct mutex cfg_mutex;
    struct scth_cfg_store cfg;
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

/* cfg API (M2) */
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

int  scth_hook_install(__u32 nr);
int  scth_hook_remove(__u32 nr);
void scth_hook_remove_all(void);