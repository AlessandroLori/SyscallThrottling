#include "common.h"
#include <unistd.h>

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(1, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_prog("uname"));

    pid_t pids[5];
    T_INFO("t05_abort_off: 5 uname, max=1, off dopo 200ms");
    T_CHECK_RC(scth_spawn_many_uname(pids, 5));
    usleep(200000);
    T_CHECK_RC(scth_off());
    T_CHECK_RC(scth_wait_all(pids, 5));

    struct scth_stats st;
    T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.total_tracked == 5, "tracked atteso 5");
    T_ASSERT(st.total_immediate == 1, "immediate atteso 1");
    T_ASSERT(st.total_delayed == 0, "delayed atteso 0");
    T_ASSERT(st.total_aborted == 4, "aborted atteso 4");
    T_ASSERT(st.peak_delay_ns == 0, "peak_delay atteso 0");
    T_ASSERT(st.delay_num == 0, "delay_num atteso 0");
    T_ASSERT(st.peak_blocked_threads >= 4, "peak_blocked_threads atteso >= 4");
    T_ASSERT(st.peak_fifo_qlen > 0, "peak_fifo_qlen atteso > 0");
    T_PASS();
}
