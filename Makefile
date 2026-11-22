CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99

.PHONY: all test test-loop clean
all: diskmedic

diskmedic: diskmedic.c
	$(CC) $(CFLAGS) $< -o $@

test:
	./tests/run_tests.sh

test-loop:
	sudo sh ./tests/run_loop_tests.sh
	sudo sh ./tests/run_sector_tests.sh
	sudo sh ./tests/run_nvme_tests.sh

clean:
	rm -f diskmedic
