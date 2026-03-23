CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -I../include
LDFLAGS ?=

TESTS = \
	t01_no_match \
	t02_immediate_only \
	t03_fifo_burst \
	t04_wakerace_burst \
	t05_abort_off \
	t06_dynamic_setmax \
	t07_multisyscall \
	t08_permissions \
	t09_dup_config \
	t10_policy_transition \
	t11_nonroot_uid_match \
	t12_root_uid_nomatch \
	t13_setmax_invalid \
	t15_wakerace_to_fifo \
	t14_unload_during_wait

COMMON_OBJS = common.o

all: $(TESTS)

common.o: common.c common.h
	$(CC) $(CFLAGS) -c -o $@ common.c

t01_no_match: t01_no_match.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t02_immediate_only: t02_immediate_only.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t03_fifo_burst: t03_fifo_burst.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t04_wakerace_burst: t04_wakerace_burst.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t05_abort_off: t05_abort_off.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t06_dynamic_setmax: t06_dynamic_setmax.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t07_multisyscall: t07_multisyscall.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t08_permissions: t08_permissions.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t09_dup_config: t09_dup_config.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t10_policy_transition: t10_policy_transition.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t11_nonroot_uid_match: t11_nonroot_uid_match.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t12_root_uid_nomatch: t12_root_uid_nomatch.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t13_setmax_invalid: t13_setmax_invalid.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t15_wakerace_to_fifo: t15_wakerace_to_fifo.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

t14_unload_during_wait: t14_unload_during_wait.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

bench_uname: bench_uname.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f *.o $(TESTS)

.PHONY: all clean
