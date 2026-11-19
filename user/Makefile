# User-space programs for the SCHED_WRR assignment. They are linked
# statically so they run in the tiny test VM (busybox, no shared libraries).
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
CFLAGS  += -Iinclude
LDFLAGS += -static

OUT   ?= build
PROGS := wrr-test wrr-run wrr-weight spin wrr-share wrr-latency
BINS  := $(addprefix $(OUT)/,$(PROGS))

all: $(BINS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/wrr-test: tests/wrr-test.c include/wrr.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(OUT)/%: tools/%.c include/wrr.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(OUT)/%: experiments/%.c include/wrr.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf build $(OUT)

.PHONY: all clean
