# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host tests for NativeNovatekTouch. `make test` builds with the system C compiler and runs; no hardware is touched.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CPPFLAGS += -I..
LDLIBS += -lm
SOURCES = main.c ../NvtProtocol.c ../NvtTouchCore.c
HEADERS = ../NvtProtocol.h ../NvtTouchCore.h ../TouchBoard.h ../TouchPublish.h

test: $(SOURCES) $(HEADERS) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -o nvt_tests $(SOURCES) $(LDLIBS)
	./nvt_tests
	@rm -f nvt_tests

clean:
	rm -f nvt_tests

.PHONY: test clean
