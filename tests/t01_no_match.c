#include "common.h"
#include <string.h>

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_add_sys(63));

    pid_t pids[20];
    T_INFO("t01_no_match: 20 uname, syscall 63 hookata ma nessun prog/uid match");
    T_CHECK_RC(scth_spawn_many_uname(pids, 20));
    T_CHECK_RC(scth_wait_all(pids, 20));

    struct scth_stats st;
    T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.total_tracked == 0, "tracked atteso 0, got %" PRIu64, (uint64_t)st.total_tracked);
    T_ASSERT(st.total_immediate == 0, "immediate atteso 0");
    T_ASSERT(st.total_delayed == 0, "delayed atteso 0");
    T_ASSERT(st.total_aborted == 0, "aborted atteso 0");
    T_ASSERT(st.peak_delay_ns == 0, "peak_delay atteso 0");
    T_ASSERT(st.delay_num == 0, "delay_num atteso 0");
    T_ASSERT(st.peak_fifo_qlen == 0, "peak_fifo_qlen atteso 0");
    T_ASSERT(st.max_active == 3, "max_active atteso 3, got %u", st.max_active);
    T_ASSERT(st.policy_active == SCTH_POLICY_FIFO_STRICT, "policy_active attesa FIFO");
    T_PASS();
}
