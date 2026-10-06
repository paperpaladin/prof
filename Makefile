# prof - sampling CPU profiler on perf_event_open(2). C11, gcc, no libraries.
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -g

all: prof

prof: prof.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f prof

.PHONY: all clean
