# tools/Makefile — DJM-T1 tools

CC      ?= cc
CXX     ?= c++
CFLAGS  := -Wall -Wextra -O2 $(shell pkg-config --cflags libusb-1.0)
LDFLAGS := $(shell pkg-config --libs libusb-1.0)

.PHONY: all clean

all: djmt1-iso-test djmt1-latency

djmt1-iso-test: djmt1-iso-test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

djmt1-latency: djmt1-latency.cpp
	$(CXX) -std=c++17 -Wall -Wextra -O2 -I../bridge -o $@ $< \
		-framework CoreAudio -framework CoreFoundation

clean:
	rm -f djmt1-iso-test djmt1-latency capture.raw
