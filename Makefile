obj-m := gddr7_temp.o

KVER  ?=
KDIR  ?=

# Codegen: generate gpu_offsets_generated.h from offsets.yaml + script.
gpu_offsets_generated.h: offsets.yaml gen_offsets.py
	python3 gen_offsets.py $< $@

.PHONY: modules modules_install clean ide

modules: gpu_offsets_generated.h
	@[ -n "$(KVER)" ] || { echo "ERROR: KVER not set"; exit 1; }
	@[ -n "$(KDIR)" ] || { echo "ERROR: KDIR not set"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# Generate compile_commands.json for clangd/IDE support.
ide:
	@REAL_KDIR=$$(readlink -f /lib/modules/$$(uname -r)/build); \
	python3 gen_compile_commands.py $(PWD) "$$REAL_KDIR"; \
	echo "Generated compile_commands.json (KDIR=$$REAL_KDIR)"

clean:
	rm -f gpu_offsets_generated.h compile_commands.json
	$(MAKE) -C $(KDIR) M=$(PWD) clean
