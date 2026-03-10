#include <stdio.h>
#include <string.h>

int cmd_run(int argc, char **argv);

static void usage(const char *p)
{
    printf("Usage:\n");
    printf("  %s on | off\n", p);
    printf("  %s setmax <max_per_sec>\n", p);
    printf("  %s setpolicy <0|1>   (0=FIFO_STRICT, 1=WAKE_RACE)\n", p);
    printf("  %s resetstats\n", p);
    printf("  %s status\n", p);
    printf("  %s stats\n", p);
    printf("  %s addprog <comm> | delprog <comm> | listprog\n", p);
    printf("  %s adduid <euid>  | deluid <euid>  | listuid\n", p);
    printf("  %s addsys <nr>    | delsys <nr>    | listsys\n", p);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }

    int rc = cmd_run(argc, argv);
    if (rc == -2) {
        usage(argv[0]);
        return 1;
    }
    return rc;
}