# Add include directory
ccflags-y := -I$(src)/include
ccflags-y += -include $(src)/include/mirilla-symbol.h

# Set MIRILLA_STEALTH_MODE=1 to omit logging and rename Mirilla-owned symbols.
MIRILLA_STEALTH_MODE ?= 0
MIRILLA_SYMBOL_HEADER := include/mirilla-symbol.generated.h
MIRILLA_SYMBOL_LIST := mirilla-symbol.lst
MIRILLA_SYMBOL_GENERATOR := tools/mirilla-stealth.py
MIRILLA_STEALTH_STRIP ?= strip
no-clean-files += $(MIRILLA_SYMBOL_LIST)

ifeq ($(MIRILLA_STEALTH_MODE),1)
MIRILLA_DEVICE_NAME ?=
ifndef MIRILLA_STEALTH_SEED
$(error MIRILLA_STEALTH_SEED must be specified when MIRILLA_STEALTH_MODE=1)
endif

MIRILLA_MODULE_NAME := $(shell python3 $(MIRILLA_SYMBOL_GENERATOR) --seed '$(MIRILLA_STEALTH_SEED)' --module-name)

ccflags-y += -DMIRILLA_STEALTH_MODE

module: $(MIRILLA_SYMBOL_HEADER)

$(MIRILLA_SYMBOL_HEADER): FORCE $(MIRILLA_SYMBOL_LIST) $(MIRILLA_SYMBOL_GENERATOR)
	python3 $(MIRILLA_SYMBOL_GENERATOR) --seed '$(MIRILLA_STEALTH_SEED)' \
		--list $(MIRILLA_SYMBOL_LIST) --output $(MIRILLA_SYMBOL_HEADER)
else
MIRILLA_MODULE_NAME ?= mirilla
MIRILLA_DEVICE_NAME ?= mirilla
endif

obj-m += $(MIRILLA_MODULE_NAME).o
$(MIRILLA_MODULE_NAME)-objs := $(patsubst %.c,%.o,$(wildcard src/*.c))

ccflags-y += -DMIRILLA_DEVICE_DEFAULT_NAME=\"$(MIRILLA_DEVICE_NAME)\"

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

# --- Test Suite Build ---

# The suites read peephole views through the catalejo-fault subsystem, so
# its C sources are compiled straight into every test binary.
CATALEJO_FAULT_DIR := ../catalejo-fault/c
CATALEJO_FAULT_SRCS := $(wildcard $(CATALEJO_FAULT_DIR)/src/*.c)

# The layout suite uses the catalejo-sys C wrappers, which recover `-errno`
# from the raw ioctl so negative tests can assert the returned status directly.
CATALEJO_SYS_DIR := ../catalejo-sys/c
CATALEJO_SYS_SRCS := $(wildcard $(CATALEJO_SYS_DIR)/src/*.c)

TEST_CFLAGS := -I. -Iinclude -I$(CATALEJO_FAULT_DIR)/include -I$(CATALEJO_SYS_DIR)/include -Wall -Wextra -std=c11

TEST_SUITES := test/test-suite test/test-self test/test-concurrency \
           test/test-invariants test/test-ioctl test/test-layout

all: module tests

clean:
	cp $(MIRILLA_SYMBOL_LIST) $(MIRILLA_SYMBOL_LIST).preserve
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	mv $(MIRILLA_SYMBOL_LIST).preserve $(MIRILLA_SYMBOL_LIST)
	rm -f $(TEST_SUITES) compile_commands.json include/*.generated.h

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

module:
ifeq ($(MIRILLA_STEALTH_MODE),1)
	$(MAKE) -C $(KDIR) M=$(PWD) CONFIG_DEBUG_INFO_BTF_MODULES= modules
	$(MIRILLA_STEALTH_STRIP) --strip-unneeded $(MIRILLA_MODULE_NAME).ko
else
	$(MAKE) -C $(KDIR) M=$(PWD) modules
endif

test/test-concurrency: TEST_CFLAGS += -pthread

test/%: test/%.c test/test-harness.h $(wildcard include/*.h) $(CATALEJO_FAULT_SRCS) $(CATALEJO_SYS_SRCS)
	gcc -o $@ $< $(CATALEJO_FAULT_SRCS) $(CATALEJO_SYS_SRCS) $(TEST_CFLAGS)

tests: $(TEST_SUITES)
	@echo "Test suites built successfully!"
	@echo "  - test/test-suite          : Basic functionality tests"
	@echo "  - test/test-self           : Self-observation tests"
	@echo "  - test/test-concurrency    : Concurrency and race tests"
	@echo "  - test/test-invariants     : Mapping invariant tests"
	@echo "  - test/test-ioctl          : Ioctl error-path tests"
	@echo "  - test/test-layout         : Address space observation tests"
	@echo ""
	@echo "Make sure the mirilla module is loaded first."

test: tests

# The suites are run from the workspace `Justfile` (see `just mirilla-test`),
# which loads the module and drives them inside a mirilla-powered VM.

# --- Developer Experience (DX) Targets ---

# Formats C sources and headers using clang-format
format:
	clang-format -style=file -i $(wildcard src/*.c include/*.h test/*.c test/*.h)

# Generates compile_commands.json for Clangd
language-server: clean
	bear -- $(MAKE) all

.PHONY: all clean install test tests module format language-server FORCE

FORCE:
