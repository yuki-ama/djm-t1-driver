# bridge/Makefile — DJM-T1 bridge daemon

CXX     ?= c++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 $(shell pkg-config --cflags libusb-1.0)
LDFLAGS  := $(shell pkg-config --libs libusb-1.0)

TARGET  := djmt1-bridge
SRCS    := main.cpp usb_device.cpp iso_engine.cpp
OBJS    := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Header dependencies
main.o: shm_protocol.h usb_device.h iso_engine.h
usb_device.o: usb_device.h
iso_engine.o: iso_engine.h shm_protocol.h

clean:
	rm -f $(TARGET) $(OBJS)
