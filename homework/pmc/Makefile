ROOT := ../..
BUILD := $(ROOT)/.pmc-build/pmc-tests
CC = gcc
CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Werror
CPPFLAGS = -D_GNU_SOURCE -DHALO_PMC_STANDALONE -I$(ROOT)/src/include
CORE := $(ROOT)/src/backend/utils/misc/halo_pmc.c

.PHONY: all test benchmark sanitize
all: $(BUILD)/test_core $(BUILD)/bench_read

$(BUILD):
	mkdir -p $@

$(BUILD)/test_core: tests/test_core.c $(CORE) $(ROOT)/src/include/utils/halo_pmc.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE) -Wl,--wrap=syscall -Wl,--wrap=read -Wl,--wrap=__read_chk -Wl,--wrap=mmap -o $@

$(BUILD)/bench_read: tests/bench_read.c $(CORE) $(ROOT)/src/include/utils/halo_pmc.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE) -o $@

test: all
	$(BUILD)/test_core

benchmark: all
	$(BUILD)/bench_read

sanitize:
	$(MAKE) BUILD=$(ROOT)/.pmc-build/pmc-sanitize CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' test
