#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Benchmark di confronto prestazionale tra policy FIFO e WAKE&RACE */

struct worker_arg {
    int calls;
    pthread_barrier_t *bar;
    uint64_t errors;
};

static void *worker_fn(void *arg_)
{
    struct worker_arg *arg = (struct worker_arg *)arg_;
    struct utsname u;
    memset(&u, 0, sizeof(u));

    pthread_barrier_wait(arg->bar);

    for (int i = 0; i < arg->calls; i++) {
        long rc = syscall(SYS_uname, &u);
        if (rc != 0)
            arg->errors++;
    }

    return NULL;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int threads = 300;
    int calls = 300;

    if (argc >= 2)
        threads = atoi(argv[1]);
    if (argc >= 3)
        calls = atoi(argv[2]);

    if (threads <= 0 || calls <= 0) {
        fprintf(stderr, "usage: %s [threads] [calls_per_thread]\n", argv[0]);
        return 1;
    }

    pthread_t *th = calloc((size_t)threads, sizeof(*th));
    struct worker_arg *args = calloc((size_t)threads, sizeof(*args));
    pthread_barrier_t bar;

    if (!th || !args) {
        perror("calloc");
        free(th);
        free(args);
        return 1;
    }

    if (pthread_barrier_init(&bar, NULL, (unsigned)threads + 1) != 0) {
        perror("pthread_barrier_init");
        free(th);
        free(args);
        return 1;
    }

    for (int i = 0; i < threads; i++) {
        args[i].calls = calls;
        args[i].bar = &bar;
        args[i].errors = 0;

        int rc = pthread_create(&th[i], NULL, worker_fn, &args[i]);
        if (rc != 0) {
            errno = rc;
            perror("pthread_create");
            return 1;
        }
    }

    double t0 = now_sec();
    pthread_barrier_wait(&bar);

    uint64_t total_errors = 0;
    for (int i = 0; i < threads; i++) {
        pthread_join(th[i], NULL);
        total_errors += args[i].errors;
    }
    double t1 = now_sec();

    printf("bench_uname done: threads=%d calls=%d total=%lld elapsed=%.3f errors=%llu\n",
           threads,
           calls,
           (long long)threads * (long long)calls,
           t1 - t0,
           (unsigned long long)total_errors);

    pthread_barrier_destroy(&bar);
    free(th);
    free(args);
    return total_errors ? 2 : 0;
}