#ifndef SCTH_IOCTL_H
#define SCTH_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SCTH_ABI_VERSION 2

/* Device node: /dev/<SCTH_DEV_NAME> */
#define SCTH_DEV_NAME "scthrottle"

/* fixed-size comm like task->comm */
#define SCTH_COMM_LEN 16

/* Policies */
#define SCTH_POLICY_FIFO_STRICT 0
#define SCTH_POLICY_WAKE_RACE   1

/* ---- Struct ABI  ---- */

struct scth_cfg {
    __u32 abi_version;
    __u32 monitor_on;
    __u64 epoch_id;
    __u32 max_active;
    __u32 max_pending;
    __u32 policy_active;
    __u32 policy_pending;
};

/* list ABI: user gives pointer+capacity, kernel writes up to capacity and returns count */
struct scth_list_req {
    __u32 capacity;   /* in: max elements */
    __u32 count;      /* out: elements written */
    __u64 user_ptr;   /* in: (uintptr_t)userspace buffer */
};

/* program/uid/sys args */
struct scth_prog_arg { char comm[SCTH_COMM_LEN]; };
struct scth_uid_arg  { __u32 euid; };
struct scth_sys_arg  { __u32 nr; };

/* Stats */
struct scth_stats {
    __u32 abi_version;

    __u64 peak_delay_ns;
    char  peak_comm[SCTH_COMM_LEN];
    __u32 peak_euid;

    __u32 peak_blocked_threads;
    __u64 blocked_sum;
    __u64 blocked_samples;

    __u64 delay_sum_ns;
    __u64 delay_num;

    __u64 total_tracked;
    __u64 total_immediate;
    __u64 total_delayed;
    __u64 total_aborted;

    __u32 peak_fifo_qlen;
    __u32 current_fifo_qlen;

    __u64 epoch_id;
    __u32 last_epoch_used;
    __u32 max_active;
    __u32 policy_active;
    __u32 policy_pending;
};

/* ---- IOCTLs ---- */
#define SCTH_IOC_MAGIC        's'

#define SCTH_IOC_ON           _IO(SCTH_IOC_MAGIC, 0x01)
#define SCTH_IOC_OFF          _IO(SCTH_IOC_MAGIC, 0x02)
#define SCTH_IOC_SET_MAX      _IOW(SCTH_IOC_MAGIC, 0x03, __u32)
#define SCTH_IOC_SET_POLICY   _IOW(SCTH_IOC_MAGIC, 0x04, __u8)
#define SCTH_IOC_RESET_STATS  _IO(SCTH_IOC_MAGIC, 0x05)
#define SCTH_IOC_GET_CFG      _IOR(SCTH_IOC_MAGIC, 0x06, struct scth_cfg)
#define SCTH_IOC_GET_STATS    _IOR(SCTH_IOC_MAGIC, 0x07, struct scth_stats)

#define SCTH_IOC_ADD_PROG       _IOW(SCTH_IOC_MAGIC, 0x10, struct scth_prog_arg)
#define SCTH_IOC_DEL_PROG       _IOW(SCTH_IOC_MAGIC, 0x11, struct scth_prog_arg)
#define SCTH_IOC_GET_PROG_COUNT _IOR(SCTH_IOC_MAGIC, 0x12, __u32)
#define SCTH_IOC_GET_PROG_LIST  _IOWR(SCTH_IOC_MAGIC, 0x13, struct scth_list_req)

#define SCTH_IOC_ADD_UID       _IOW(SCTH_IOC_MAGIC, 0x20, struct scth_uid_arg)
#define SCTH_IOC_DEL_UID       _IOW(SCTH_IOC_MAGIC, 0x21, struct scth_uid_arg)
#define SCTH_IOC_GET_UID_COUNT _IOR(SCTH_IOC_MAGIC, 0x22, __u32)
#define SCTH_IOC_GET_UID_LIST  _IOWR(SCTH_IOC_MAGIC, 0x23, struct scth_list_req)

#define SCTH_IOC_ADD_SYS       _IOW(SCTH_IOC_MAGIC, 0x30, struct scth_sys_arg)
#define SCTH_IOC_DEL_SYS       _IOW(SCTH_IOC_MAGIC, 0x31, struct scth_sys_arg)
#define SCTH_IOC_GET_SYS_COUNT _IOR(SCTH_IOC_MAGIC, 0x32, __u32)
#define SCTH_IOC_GET_SYS_LIST  _IOWR(SCTH_IOC_MAGIC, 0x33, struct scth_list_req)

/* ---- Alias compat ---- */
#define SCTH_IOC_MONITOR_ON   SCTH_IOC_ON
#define SCTH_IOC_MONITOR_OFF  SCTH_IOC_OFF

#define SCTH_IOC_SETMAX       SCTH_IOC_SET_MAX
#define SCTH_IOC_SETPOLICY    SCTH_IOC_SET_POLICY
#define SCTH_IOC_GETCFG       SCTH_IOC_GET_CFG

#define SCTH_IOC_RESETSTATS   SCTH_IOC_RESET_STATS
#define SCTH_IOC_STATS        SCTH_IOC_GET_STATS

#define SCTH_IOC_ADDPROG      SCTH_IOC_ADD_PROG
#define SCTH_IOC_DELPROG      SCTH_IOC_DEL_PROG
#define SCTH_IOC_ADDUID       SCTH_IOC_ADD_UID
#define SCTH_IOC_DELUID       SCTH_IOC_DEL_UID
#define SCTH_IOC_ADDSYS       SCTH_IOC_ADD_SYS
#define SCTH_IOC_DELSYS       SCTH_IOC_DEL_SYS

#define SCTH_IOC_LIST_PROG    SCTH_IOC_GET_PROG_LIST
#define SCTH_IOC_LIST_UID     SCTH_IOC_GET_UID_LIST
#define SCTH_IOC_LIST_SYS     SCTH_IOC_GET_SYS_LIST

#define peak_prog     peak_comm
#define peak_uid      peak_euid
#define capacity_elems capacity
#define max_count      capacity
#define out_count      count
#define user_buf       user_ptr

struct scth_max_req    { __u32 max_per_sec; };
struct scth_policy_req { __u8  policy; };
struct scth_uid_req    { __u32 euid; };
struct scth_sys_req    { __u32 nr; };
struct scth_comm_req   { char  comm[SCTH_COMM_LEN]; };

#define max_per_epoch max_per_sec

/* ---- Alias compat---- */
#define abi abi_version

#define peak_prog peak_comm
#define peak_uid  peak_euid

#define blocked_sum_samples blocked_sum
#define blocked_num_samples blocked_samples

#define cap       capacity
#define max_count capacity
#define ptr       user_ptr
#define user_buf  user_ptr

#define SCTH_IOC_MONITOR_ON  SCTH_IOC_ON
#define SCTH_IOC_MONITOR_OFF SCTH_IOC_OFF

#define SCTH_IOC_SETMAX      SCTH_IOC_SET_MAX
#define SCTH_IOC_SETPOLICY   SCTH_IOC_SET_POLICY
#define SCTH_IOC_GETCFG      SCTH_IOC_GET_CFG
#define SCTH_IOC_STATS       SCTH_IOC_GET_STATS

#define SCTH_IOC_ADDPROG     SCTH_IOC_ADD_PROG
#define SCTH_IOC_DELPROG     SCTH_IOC_DEL_PROG
#define SCTH_IOC_ADDUID      SCTH_IOC_ADD_UID
#define SCTH_IOC_DELUID      SCTH_IOC_DEL_UID
#define SCTH_IOC_ADDSYS      SCTH_IOC_ADD_SYS
#define SCTH_IOC_DELSYS      SCTH_IOC_DEL_SYS

#define SCTH_IOC_LIST_PROG   SCTH_IOC_GET_PROG_LIST
#define SCTH_IOC_LIST_UID    SCTH_IOC_GET_UID_LIST
#define SCTH_IOC_LIST_SYS    SCTH_IOC_GET_SYS_LIST

#endif /* SCTH_IOCTL_H */