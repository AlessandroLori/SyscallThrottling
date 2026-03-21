#include "common.h"
#include <stdlib.h>
#include <unistd.h>

// verfica inverso di t11, non root ma no match su userid ovvero no throttling

int main(void)
{
    if (scth_require_root()) return 2;

    const char *suid = getenv("SUDO_UID");
    __u32 target_uid = suid ? (__u32)strtoul(suid, NULL, 10) : 1000;

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_del_uid_ignore(target_uid));

    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_uid(target_uid));

    T_INFO("t12_root_uid_nomatch: worker root, config uid=%u, nessun prog match", (unsigned)target_uid);

    pid_t pids[12];
    T_CHECK_RC(scth_spawn_many_uname(pids, 12));
    T_CHECK_RC(scth_wait_all(pids, 12));

    struct scth_stats st;
    T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.total_tracked == 0, "tracked atteso 0, got %" PRIu64, (uint64_t)st.total_tracked);
    T_ASSERT(st.total_immediate == 0, "immediate atteso 0");
    T_ASSERT(st.total_delayed == 0, "delayed atteso 0");
    T_ASSERT(st.total_aborted == 0, "aborted atteso 0");
    T_ASSERT(st.peak_delay_ns == 0, "peak_delay atteso 0");
    T_PASS();
}