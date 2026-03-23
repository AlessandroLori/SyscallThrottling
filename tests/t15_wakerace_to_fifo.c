#include "common.h"

#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Verifica transizione dinamica WAKE_RACE -> FIFO_STRICT.
 *
 * Scenario:
 * - policy iniziale WAKE_RACE, max=3
 * - 12 processi "uname" partono quasi insieme
 * - 3 passano subito, gli altri restano in attesa su epoch_wq
 * - mentre sono in attesa, si imposta policy_pending = FIFO_STRICT
 * - alla nuova epoca policy_active diventa FIFO_STRICT
 * - i waiter WAKE_RACE devono essere migrati nella fifo_q e poi drenati
 *
 * Condizioni attese:
 * - nessun worker resta bloccato
 * - policy finale attiva = FIFO_STRICT
 * - peak_fifo_qlen > 0, segno che la migrazione verso FIFO è avvenuta
 * - nessun aborted
 */

static int wait_pid_timeout(pid_t pid, int *status, int timeout_ms)
{
    int elapsed = 0;

    for (;;) {
        pid_t rc = waitpid(pid, status, WNOHANG);
        if (rc == pid)
            return 0;
        if (rc < 0)
            return -errno;

        if (elapsed >= timeout_ms)
            return -ETIMEDOUT;

        usleep(10000); /* 10 ms */
        elapsed += 10;
    }
}

static int wait_all_timeout(pid_t *pids, size_t n, int timeout_ms_each)
{
    for (size_t i = 0; i < n; i++) {
        int status = 0;
        int rc = wait_pid_timeout(pids[i], &status, timeout_ms_each);
        if (rc < 0)
            return rc;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return -ECHILD;
    }
    return 0;
}

int main(void)
{
    if (scth_require_root()) return 2;

    T_CHECK_RC(scth_setup_base(3, SCTH_POLICY_WAKE_RACE));
    T_CHECK_RC(scth_cleanup_common());
    T_CHECK_RC(scth_add_sys(63));
    T_CHECK_RC(scth_add_prog("uname"));

    T_INFO("t15_wakerace_to_fifo: WAKE_RACE -> FIFO con migrazione dei waiter");

    pid_t pids[12];
    T_CHECK_RC(scth_spawn_many_uname(pids, 12));

    /* lascio tempo ai worker di entrare in WAKE_RACE e restare in attesa */
    usleep(200000);

    struct scth_cfg cfg1, cfg2, cfg3;
    struct scth_stats st;

    T_CHECK_RC(scth_get_cfg(&cfg1));
    T_INFO("cfg prima del cambio policy:");
    scth_print_cfg(&cfg1);

    T_ASSERT(cfg1.policy_active == SCTH_POLICY_WAKE_RACE,
             "policy_active iniziale attesa WAKE_RACE");

    T_CHECK_RC(scth_setpolicy(SCTH_POLICY_FIFO_STRICT));

    T_CHECK_RC(scth_get_cfg(&cfg2));
    T_INFO("cfg subito dopo setpolicy(FIFO_STRICT):");
    scth_print_cfg(&cfg2);

    T_ASSERT(cfg2.policy_active == SCTH_POLICY_WAKE_RACE,
             "policy_active dovrebbe restare WAKE_RACE prima della nuova epoca");
    T_ASSERT(cfg2.policy_pending == SCTH_POLICY_FIFO_STRICT,
             "policy_pending dovrebbe essere FIFO_STRICT subito");

    /* attendo almeno una nuova epoca per attivare davvero FIFO */
    usleep(1300000);

    T_CHECK_RC(scth_get_cfg(&cfg3));
    T_INFO("cfg dopo una nuova epoca:");
    scth_print_cfg(&cfg3);

    T_ASSERT(cfg3.policy_active == SCTH_POLICY_FIFO_STRICT,
             "policy_active dovrebbe diventare FIFO_STRICT dopo la nuova epoca");
    T_ASSERT(cfg3.policy_pending == SCTH_POLICY_FIFO_STRICT,
             "policy_pending dovrebbe restare FIFO_STRICT");

    /* se la migrazione funziona, tutti devono finire entro tempi ragionevoli */
    T_CHECK_RC(wait_all_timeout(pids, 12, 10000));

        T_CHECK_RC(scth_get_stats(&st));
    scth_print_stats(&st);

    T_ASSERT(st.policy_active == SCTH_POLICY_FIFO_STRICT,
             "stats.policy_active attesa FIFO_STRICT");
    T_ASSERT(st.total_tracked >= 12,
             "tracked atteso almeno 12, got %" PRIu64,
             (uint64_t)st.total_tracked);
    T_ASSERT(st.total_immediate + st.total_delayed >= 12,
             "immediate+delayed atteso almeno 12");
    T_ASSERT(st.total_aborted == 0,
             "aborted atteso 0");
    T_ASSERT(st.total_delayed > 0,
             "delayed atteso > 0");
    T_ASSERT(st.peak_fifo_qlen > 0,
             "peak_fifo_qlen atteso > 0: i waiter WAKE_RACE dovrebbero migrare in FIFO");

    /*
     * Le stats sono globali al monitor: in suite potrebbe esserci uno straggler
     * esterno/residuo che matcha "uname" e tiene current_fifo_qlen > 0 proprio
     * nell'istante del campionamento. Faccio quindi quiesce esplicito prima
     * dell'assert finale sulla coda corrente.
     */
    T_CHECK_RC(scth_off());
    usleep(200000);

    T_CHECK_RC(scth_get_stats(&st));
    T_ASSERT(st.current_fifo_qlen == 0,
             "current_fifo_qlen atteso 0 dopo quiesce");

    T_CHECK_RC(scth_cleanup_common());

    T_PASS();
}