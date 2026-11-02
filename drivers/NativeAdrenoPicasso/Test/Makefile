# SPDX-License-Identifier: MIT
# Host tests for NativeAdrenoPicasso. `make test` builds with the system
# compilers and runs; no hardware is touched. `make build` keeps the binary so
# ADRENO620_VENDOR_DIR=<extracted vendor files> can validate the real blobs.
CC ?= cc
CXX ?= c++
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
CPPFLAGS += -I..
OBJECTS = main.o Adreno620Board.o Adreno620ProbeCore.o Adreno620Firmware.o

test: build
	./adreno620_tests
	@rm -f $(OBJECTS) adreno620_tests

build: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o adreno620_tests $(OBJECTS)

main.o: main.cpp ../Adreno620Board.h ../Adreno620Firmware.h ../Adreno620ProbeCore.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c main.cpp -o $@

Adreno620Board.o: ../Adreno620Board.cpp ../Adreno620Board.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c ../Adreno620Board.cpp -o $@

Adreno620ProbeCore.o: ../Adreno620ProbeCore.cpp ../Adreno620ProbeCore.h ../Adreno620Board.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c ../Adreno620ProbeCore.cpp -o $@

Adreno620Firmware.o: ../Adreno620Firmware.c ../Adreno620Firmware.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c ../Adreno620Firmware.c -o $@

clean:
	rm -f $(OBJECTS) adreno620_tests

.PHONY: test build clean
