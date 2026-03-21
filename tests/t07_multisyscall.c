#include "common.h"

// verifica funzionamento throttling con più syscall diverse

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());

    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_sys(39));
    T_CHECK_RC(scth_add_prog("uname"));
    T_CHECK_RC(scth_add_prog("python3"));

    pid_t pids[20];
    T_INFO("t07_multisyscall: 10 uname + 10 python3(getpid), FIFO max=3");
    T_CHECK_RC(scth_spawn_many_uname(pids, 10));
    T_CHECK_RC(scth_spawn_many_python_getpid(pids + 10, 10));
    T_CHECK_RC(scth_wait_all(pids, 20));

    struct scth_stats st;
    T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.total_tracked == 20, "tracked atteso 20");
    T_ASSERT(st.total_immediate + st.total_delayed == 20, "immediate+delayed atteso 20");
    T_ASSERT(st.total_aborted == 0, "aborted atteso 0");
    T_ASSERT(st.peak_delay_ns > 0, "peak_delay atteso > 0");
    T_ASSERT(st.peak_fifo_qlen > 0, "peak_fifo_qlen atteso > 0");
    T_ASSERT(st.policy_active == SCTH_POLICY_FIFO_STRICT, "policy_active attesa FIFO");
    T_PASS();
}
