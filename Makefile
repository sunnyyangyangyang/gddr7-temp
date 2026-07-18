# Makefile.akmod — used inside per-kernel build dirs the kmod spec
# creates (_kmod_build_<kver>/). The spec invokes this with:
#   make KVER=... KDIR=... modules
# from within the build directory.

obj-m := gddr7_temp.o

KVER  ?=
KDIR  ?=

.PHONY: modules modules_install clean

modules:
	@[ -n "$(KVER)" ] || { echo "ERROR: KVER not set"; exit 1; }
	@[ -n "$(KDIR)" ] || { echo "ERROR: KDIR not set"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
