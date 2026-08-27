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

# Codegen: generate gpu_tables.rs from offsets.yaml + script (single source
# of truth for all GPU offset tables).
gpu_tables.rs: offsets.yaml gen_offsets.py
	python3 gen_offsets.py $< $@

.PHONY: modules modules_install clean ide

# IDE support: generate rust-project.json for rust-analyzer with the kernel's
# own scripts/generate_rust_analyzer.py (same invocation as kbuild's
# `rust-analyzer` target in $(KDIR)/rust/Makefile, this dir passed as OOT
# exttree). Edition/cfgs derived from CONFIG_RUSTC_VERSION like rust/Makefile
# does. Installed RPM trees ship prebuilt rmeta only (no .rs sources), so when
# $(KDIR) lacks them we clone the matching v<maj.min> tag into .ide-src/
# (git-ignored, ~1GB) and generate against that; tools/ra_postprocess.py then
# drops source-less sysroot crates (RA falls back to its bundled std by name)
# and adds direct dep edges the generator missed (e.g. `use bindings::...`).
KTAG    := v$(shell echo $(KVER) | cut -d. -f1,2)
IDE_SRC ?= $(PWD)/.ide-src/$(KTAG)

ide: gpu_tables.rs
	@[ -n "$(KDIR)" ] || { echo "ERROR: KDIR not set"; exit 1; }
	@test -f "$(KDIR)/scripts/generate_rust_analyzer.py" || \
	  { echo "ERROR: $(KDIR) has no scripts/generate_rust_analyzer.py (kernel too old?)"; exit 1; }
	@SRC="$(KDIR)"; [ -f "$$SRC/rust/kernel/lib.rs" ] || SRC="$(IDE_SRC)"; \
	if [ ! -f "$$SRC/rust/kernel/lib.rs" ]; then \
	  echo "INFO: $(KDIR) has no rust sources (prebuilt RPM); cloning torvalds/linux $(KTAG) into $$SRC ..."; \
	  mkdir -p "$$(dirname "$$SRC")"; \
	  git clone --quiet --depth 1 --branch "$(KTAG)" https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git "$$SRC" || exit 1; \
	fi; \
	mkdir -p "$$SRC/include/generated"; \
	cp "$(KDIR)/include/generated/rustc_cfg" "$$SRC/include/generated/" 2>/dev/null || true; \
	RUSTCV=$$(grep -m1 '^CONFIG_RUSTC_VERSION=' "/boot/config-$(KVER)" | cut -d= -f2); \
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
	  "$$SRC" "$$SRC" "$(KDIR)/rust" "$$SYSROOT/lib/rustlib/src/rust/library" "$(PWD)" > rust-project.json
	@# First-time setup of the in-repo source clone: kbuild-generated files
	@# (bindings_generated.rs, uapi_generated.rs, ...) are absent from a fresh
	@# clone and their missing include!()s flood the IDE with errors. Generate
	@# them via `make prepare` (needs only headers + bindgen, not a full build).
	@SRC="$(KDIR)"; [ -f "$$SRC/rust/kernel/lib.rs" ] || SRC="$(IDE_SRC)"; \
	if [ "$$SRC" = "$(IDE_SRC)" ] && [ ! -f "$$SRC/rust/uapi/uapi_generated.rs" ]; then \
	  echo "INFO: generating kbuild rust files in $(IDE_SRC) (~2 min) ..."; \
	  [ -f "$$SRC/.config" ] || cp "/boot/config-$(KVER)" "$$SRC/.config"; \
	  ( cd "$$SRC" && make olddefconfig >/dev/null 2>&1 && make -j"$$(nproc)" prepare ) || \
	    echo "WARN: kbuild prepare failed; IDE will show errors in kernel crates until it succeeds."; fi && \
	python3 tools/ra_postprocess.py rust-project.json "$(KDIR)/rust" && \
	SRV=""; \
	for c in "$(KDIR)/rust/libexec/rust-analyzer-proc-macro-srv" \
	         "$$(rustc --print sysroot)/libexec/rust-analyzer-proc-macro-srv" \
	         "$$(rustc --print sysroot)/lib/rust-analyzer-proc-macro-srv"; do \
	  [ -x "$$c" ] && SRV="$$c" && break; done; \
	if [ -n "$$SRV" ]; then \
	  mkdir -p .vscode && \
	  printf '{\n  "rust-analyzer.linkedProjects": ["rust-project.json"],\n  "rust-analyzer.procMacro.server": "%s"\n}\n' "$$SRV" > .vscode/settings.json && \
	  echo "  .vscode/settings.json -> procMacro.server=$$SRV"; \
	else \
	  echo "WARN: rust-analyzer-proc-macro-srv not found; set rust-analyzer.procMacro.server in .vscode/settings.json manually."; fi && \
	echo "rust-project.json written (sources: $$SRC); reload your IDE window to pick it up."

modules: gpu_tables.rs
	@[ -n "$(KVER)" ] || { echo "ERROR: KVER not set"; exit 1; }
	@[ -n "$(KDIR)" ] || { echo "ERROR: KDIR not set"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	rm -f gpu_tables.rs
	@if [ -n "$(KDIR)" ] && [ -d "$(KDIR)" ]; then \
		$(MAKE) -C $(KDIR) M=$(PWD) clean; \
	fi
