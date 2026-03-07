CC=gcc
CFLAGS=-O2 -Wall -Wextra -I../include

all: scthctl

scthctl: scthctl.o scthctl_cmds.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f *.o scthctl