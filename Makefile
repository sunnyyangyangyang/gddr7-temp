obj-m := gddr7_temp.o

KVER  ?=
KDIR  ?=
BUILD_DIR = $(PWD)/build

# Ensure build directory exists with symlinks to source files
$(BUILD_DIR): | $(BUILD_DIR)/.symlink-stamp

$(BUILD_DIR)/.symlink-stamp:
	mkdir -p $(BUILD_DIR)
	ln -sf $(PWD)/gddr7_temp.c $(BUILD_DIR)/gddr7_temp.c
	ln -sf $(PWD)/gpu_offsets.h $(BUILD_DIR)/gpu_offsets.h
	ln -sf $(PWD)/Makefile $(BUILD_DIR)/Makefile
	touch $(BUILD_DIR)/.symlink-stamp

# Codegen: generate gpu_offsets_generated.h from offsets.yaml + script.
$(BUILD_DIR)/gpu_offsets_generated.h: offsets.yaml scripts/gen_offsets.py | $(BUILD_DIR)
	python3 scripts/gen_offsets.py $< $@

.PHONY: modules modules_install clean ide

modules: $(BUILD_DIR)/gpu_offsets_generated.h
	@[ -n "$(KVER)" ] || { echo "ERROR: KVER not set"; exit 1; }
	@[ -n "$(KDIR)" ] || { echo "ERROR: KDIR not set"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) modules_install

# Generate compile_commands.json for clangd/IDE support.
ide:
	@REAL_KDIR=$$(readlink -f /lib/modules/$$(uname -r)/build); \
	python3 scripts/gen_compile_commands.py $(PWD) "$$REAL_KDIR"; \
	echo "Generated compile_commands.json (KDIR=$$REAL_KDIR)"

clean:
	rm -rf $(BUILD_DIR) compile_commands.json
