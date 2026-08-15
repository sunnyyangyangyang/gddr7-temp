# gddr7_temp — Rust for Linux kernel module (issue #14)
#
# This Makefile doubles as the kbuild Kbuild file: `obj-m` below selects
# gddr7_temp.rs, which kbuild compiles with its .rs rules. Requires a kernel
# built with CONFIG_RUST=y and a rustc whose version matches the kernel's
# CONFIG_RUSTC_VERSION_TEXT (Fedora ships both in lockstep). bindgen/libclang
# are only needed if the kernel tree does not ship prebuilt Rust bindings.

obj-m := gddr7_temp.o

KVER  ?=
KDIR  ?=

# Codegen: generate gpu_tables.inc from offsets.yaml + script (single source
# of truth for all GPU offset tables).
gpu_tables.inc: offsets.yaml gen_offsets.py
	python3 gen_offsets.py $< $@

.PHONY: modules modules_install clean

modules: gpu_tables.inc
	@[ -n "$(KVER)" ] || { echo "ERROR: KVER not set"; exit 1; }
	@[ -n "$(KDIR)" ] || { echo "ERROR: KDIR not set"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	rm -f gpu_tables.inc
	@if [ -n "$(KDIR)" ] && [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	fi
