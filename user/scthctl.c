#include <stdio.h>

int cmd_run(int argc, char **argv);

int main(int argc, char **argv)
{
    return cmd_run(argc - 1, argv + 1);
}