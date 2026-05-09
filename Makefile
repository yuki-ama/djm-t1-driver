# DJM-T1 Audio Driver — Top-level Makefile

.PHONY: all bridge plugin tools install uninstall pkg clean

all: bridge plugin

bridge:
	$(MAKE) -C bridge

plugin:
	$(MAKE) -C plugin

tools:
	$(MAKE) -C tools

install: all
	@install/install.sh

uninstall:
	@install/uninstall.sh

pkg: all
	@install/build-pkg.sh

clean:
	$(MAKE) -C bridge clean
	$(MAKE) -C plugin clean
	$(MAKE) -C tools clean
