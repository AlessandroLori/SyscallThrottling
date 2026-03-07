#pragma once

#ifdef __KERNEL__
  #include <linux/ioctl.h>
  #include <linux/types.h>
#else
  #include <sys/ioctl.h>
  #include <stdint.h>
  typedef uint8_t  __u8;
  typedef uint16_t __u16;
  typedef uint32_t __u32;
  typedef uint64_t __u64;
#endif

#define SCTH_ABI_VERSION 1
#define SCTH_COMM_LEN 16
#define SCTH_IOC_MAGIC 'S'

enum scth_policy {
    SCTH_POLICY_FIFO_STRICT = 0,
    SCTH_POLICY_WAKE_RACE   = 1,
};

struct scth_prog_arg {
    char comm[SCTH_COMM_LEN]; /* truncate to 15 + NUL */
};

struct scth_uid_arg {
    __u32 euid;
};

struct scth_sys_arg {
    __u32 nr;
};

struct scth_cfg {
    __u32 abi_version;
    __u8  monitor_on;
    __u8  policy_active;
    __u8  policy_pending;
    __u8  _pad0;

    __u32 max_active;
    __u32 max_pending;

    __u64 epoch_id;
};

struct scth_stats {
    __u32 abi_version;

    __u64 peak_delay_ns;
    char  peak_comm[SCTH_COMM_LEN];
    __u32 peak_euid;

    __u32 peak_blocked_threads;

    __u64 blocked_sum_samples;
    __u64 blocked_num_samples;

    __u32 current_blocked_threads;
    __u32 _pad0;
};

struct scth_list_req {
    __u32 capacity;
    __u32 count;
    __u64 user_ptr; /* user pointer */
};

/* monitor/config */
#define SCTH_IOC_ON            _IO (SCTH_IOC_MAGIC,  0)
#define SCTH_IOC_OFF           _IO (SCTH_IOC_MAGIC,  1)
#define SCTH_IOC_SET_MAX       _IOW(SCTH_IOC_MAGIC,  2, __u32)
#define SCTH_IOC_SET_POLICY    _IOW(SCTH_IOC_MAGIC,  3, __u8)
#define SCTH_IOC_RESET_STATS   _IO (SCTH_IOC_MAGIC,  4)
#define SCTH_IOC_GET_CFG       _IOR(SCTH_IOC_MAGIC,  5, struct scth_cfg)
#define SCTH_IOC_GET_STATS     _IOR(SCTH_IOC_MAGIC,  6, struct scth_stats)

/* programs */
#define SCTH_IOC_ADD_PROG       _IOW(SCTH_IOC_MAGIC, 10, struct scth_prog_arg)
#define SCTH_IOC_DEL_PROG       _IOW(SCTH_IOC_MAGIC, 11, struct scth_prog_arg)
#define SCTH_IOC_GET_PROG_COUNT _IOR(SCTH_IOC_MAGIC, 12, __u32)
#define SCTH_IOC_GET_PROG_LIST  _IOWR(SCTH_IOC_MAGIC,13, struct scth_list_req)

/* uids */
#define SCTH_IOC_ADD_UID        _IOW(SCTH_IOC_MAGIC, 20, struct scth_uid_arg)
#define SCTH_IOC_DEL_UID        _IOW(SCTH_IOC_MAGIC, 21, struct scth_uid_arg)
#define SCTH_IOC_GET_UID_COUNT  _IOR(SCTH_IOC_MAGIC, 22, __u32)
#define SCTH_IOC_GET_UID_LIST   _IOWR(SCTH_IOC_MAGIC,23, struct scth_list_req)

/* syscalls */
#define SCTH_IOC_ADD_SYS        _IOW(SCTH_IOC_MAGIC, 30, struct scth_sys_arg)
#define SCTH_IOC_DEL_SYS        _IOW(SCTH_IOC_MAGIC, 31, struct scth_sys_arg)
#define SCTH_IOC_GET_SYS_COUNT  _IOR(SCTH_IOC_MAGIC, 32, __u32)
#define SCTH_IOC_GET_SYS_LIST   _IOWR(SCTH_IOC_MAGIC,33, struct scth_list_req)