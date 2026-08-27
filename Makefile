# gddr7_temp — Rust for Linux kernel module (issue #14)
#
# This Makefile doubles as the kbuild Kbuild file: `obj-m` below selects
# gddr7_temp.rs, which kbuild compiles with its .rs rules. Requires a kernel
# built with CONFIG_RUST=y and a rustc whose version matches the kernel's
# CONFIG_RUSTC_VERSION_TEXT (Fedora ships both in lockstep). bindgen/libclang
# are only needed if the kernel tree does not ship prebuilt Rust bindings.

obj-m := gddr7_temp.o

# Defaults: running kernel + Fedora's installed source tree. Override with
# KVER=<ver> / KDIR=<path> for other targets (e.g. building against v7.2).
KVER  ?= $(shell uname -r)
KDIR  ?= /usr/src/kernels/$(KVER)

# Codegen: generate gpu_tables.inc from offsets.yaml + script (single source
# of truth for all GPU offset tables).
gpu_tables.inc: offsets.yaml gen_offsets.py
	python3 gen_offsets.py $< $@

.PHONY: modules modules_install clean ide

# IDE support: generate rust-project.json for rust-analyzer with the kernel's
# own scripts/generate_rust_analyzer.py — the same invocation as kbuild's
# `rust-analyzer` target in $(KDIR)/rust/Makefile, plus this directory passed
# as the OOT exttree so our module crate is included. Edition and cfgs are
# derived from CONFIG_RUSTC_VERSION exactly like rust/Makefile does (2024 at
# >= 108700; proc_macro span cfgs at >= 108800), so this keeps working across
# kernel updates. Requires the rust-src component for core/alloc/std sources.
ide: gpu_tables.inc
	@[ -n "$(KVER)" ] || { echo "ERROR: KVER not set"; exit 1; }
	@[ -n "$(KDIR)" ] || { echo "ERROR: KDIR not set"; exit 1; }
	@test -f "$(KDIR)/scripts/generate_rust_analyzer.py" || \
	  { echo "ERROR: $(KDIR) has no scripts/generate_rust_analyzer.py (kernel too old?)"; exit 1; }
	@RUSTCV=$$(grep -m1 '^CONFIG_RUSTC_VERSION=' "/boot/config-$(KVER)" | cut -d= -f2); \
	CORE_EDITION=$$([ "$$RUSTCV" -ge 108700 ] && echo 2024 || echo 2021); \
	SPAN_CFG=""; [ "$$RUSTCV" -ge 108800 ] && SPAN_CFG='proc_macro_span_file proc_macro_span_location'; \
	SYSROOT=$$(rustc --print sysroot); \
	RUSTC=rustc python3 "$(KDIR)/scripts/generate_rust_analyzer.py" \
	  --cfgs='core=no_fp_fmt_parse' "$$CORE_EDITION" \
	  --cfgs="proc_macro2=feature=\"proc-macro\" wrap_proc_macro $$SPAN_CFG" \
	  --cfgs='quote=feature="proc-macro"' \
	  --cfgs='syn=feature="clone-impls" feature="derive" feature="full" feature="parsing" feature="printing" feature="proc-macro" feature="visit-mut"' \
	  --cfgs='pin_init_internal=kernel USE_RUSTC_FEATURES' \
	  --cfgs='pin_init=kernel USE_RUSTC_FEATURES' \
	  "$(KDIR)" "$(KDIR)" "$$SYSROOT" "$$SYSROOT/lib/rustlib/src/rust/library" "$(PWD)" > rust-project.json
	@echo "rust-project.json written ($(KVER)); reload your IDE window to pick it up."

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
