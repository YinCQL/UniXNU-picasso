# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host tests for NativeWlan. `make test` builds with the system compiler and runs; no hardware is touched.
# Optional real-file cross-check: make test REAL=/tmp/wlan-probe/real/ipa_fws
CC ?= cc
CXX ?= c++
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
CPPFLAGS += -I..
REAL ?=
OBJECTS = main.o WlanFirmware.o WlanBoard.o
HEADERS = ../WlanBoard.h ../WlanFirmware.h ../WlanSmem.h

test: $(OBJECTS) Makefile
	$(CXX) -o wlan_tests $(OBJECTS)
	./wlan_tests $(REAL)
	@rm -f wlan_tests main.o WlanFirmware.o WlanBoard.o

main.o: main.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c main.c -o main.o

WlanFirmware.o: ../WlanFirmware.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c ../WlanFirmware.c -o WlanFirmware.o

WlanBoard.o: ../WlanBoard.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c ../WlanBoard.cpp -o WlanBoard.o

clean:
	rm -f wlan_tests main.o WlanFirmware.o WlanBoard.o

.PHONY: test clean
