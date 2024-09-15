CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -g

mysh: mysh.c
	$(CC) $(CFLAGS) -o $@ $<

test: mysh
	bash tests/run_tests.sh
	python3 tests/test_jobcontrol.py

clean:
	rm -f mysh

.PHONY: test clean
