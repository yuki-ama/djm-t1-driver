# plugin/Makefile — DJM-T1 AudioServerPlugin (HAL plug-in)
#
# Builds DJMT1AudioPlugin.driver bundle using libASPL.
# Install to /Library/Audio/Plug-Ins/HAL/ and restart coreaudiod.

CXX      ?= c++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -fPIC \
            -Ivendor/libASPL/include \
            -Ivendor/libASPL/src \
            -I../bridge

LDFLAGS  := -bundle \
            -framework CoreAudio \
            -framework CoreFoundation \
            -framework AudioToolbox

# Plugin sources
PLUGIN_SRCS := plugin_entry.cpp djmt1_io_handler.cpp

# libASPL sources
LIBASPL_SRCS := $(wildcard vendor/libASPL/src/*.cpp)

ALL_SRCS := $(PLUGIN_SRCS) $(LIBASPL_SRCS)
ALL_OBJS := $(patsubst %.cpp,build/%.o,$(ALL_SRCS))

BUNDLE     := build/DJMT1AudioPlugin.driver
BUNDLE_BIN := $(BUNDLE)/Contents/MacOS/DJMT1AudioPlugin

.PHONY: all clean install uninstall

all: $(BUNDLE_BIN) $(BUNDLE)/Contents/Info.plist

# Link bundle binary
$(BUNDLE_BIN): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

# Copy Info.plist
$(BUNDLE)/Contents/Info.plist: Info.plist
	@mkdir -p $(dir $@)
	cp $< $@

# Compile plugin sources
build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Compile libASPL sources
build/vendor/libASPL/src/%.o: vendor/libASPL/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Install to HAL plugins directory (requires sudo)
install: all
	sudo mkdir -p "/Library/Audio/Plug-Ins/HAL"
	sudo cp -R $(BUNDLE) "/Library/Audio/Plug-Ins/HAL/"
	sudo launchctl kickstart -k system/com.apple.audio.coreaudiod

# Uninstall
uninstall:
	sudo rm -rf "/Library/Audio/Plug-Ins/HAL/DJMT1AudioPlugin.driver"
	sudo launchctl kickstart -k system/com.apple.audio.coreaudiod

clean:
	rm -rf build/
