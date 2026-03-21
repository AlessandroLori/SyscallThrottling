#include "common.h"

// verifica funzionamento coda FIFO, genera più syscall di budget epoca, verifica ritardi e effettivo passaggio di syscall = budget 
//  per epoca

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_FIFO_STRICT));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_prog("uname"));

    pid_t pids[20];
    T_INFO("t03_fifo_burst: 20 uname, max=3, FIFO");
    T_CHECK_RC(scth_spawn_many_uname(pids, 20));
    T_CHECK_RC(scth_wait_all(pids, 20));

    struct scth_stats st;
    T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.total_tracked == 20, "tracked atteso 20, got %" PRIu64, (uint64_t)st.total_tracked);
    T_ASSERT(st.total_immediate == 3, "immediate atteso 3, got %" PRIu64, (uint64_t)st.total_immediate);
    T_ASSERT(st.total_delayed == 17, "delayed atteso 17, got %" PRIu64, (uint64_t)st.total_delayed);
    T_ASSERT(st.total_aborted == 0, "aborted atteso 0");
    T_ASSERT(st.delay_num == 17, "delay_num atteso 17, got %" PRIu64, (uint64_t)st.delay_num);
    T_ASSERT(st.peak_fifo_qlen > 0, "peak_fifo_qlen atteso > 0");
    T_ASSERT(st.current_fifo_qlen == 0, "current_fifo_qlen atteso 0");
    T_ASSERT(st.epoch_id > 0, "epoch_id atteso > 0");
    T_ASSERT(st.max_active == 3, "max_active atteso 3");
    T_ASSERT(st.policy_active == SCTH_POLICY_FIFO_STRICT, "policy_active attesa FIFO");
    T_PASS();
}
