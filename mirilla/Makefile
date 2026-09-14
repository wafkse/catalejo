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

ifeq ($(MIRILLA_KUNIT),1)
ccflags-y += -DMIRILLA_KUNIT
ifneq ($(filter y m,$(CONFIG_KUNIT)),)
$(MIRILLA_MODULE_NAME)-objs += test/kunit/test-exception.o test/kunit/test-slab.o
endif
endif

ccflags-y += -DMIRILLA_DEVICE_DEFAULT_NAME=\"$(MIRILLA_DEVICE_NAME)\"

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

# --- Userspace Integration Test ---

CATALEJO_SYS_DIR := ../catalejo-sys/c
CATALEJO_SYS_SRCS := $(wildcard $(CATALEJO_SYS_DIR)/src/*.c)

TEST_CFLAGS := -I. -Iinclude -I$(CATALEJO_SYS_DIR)/include -Wall -Wextra -std=c11 -pthread
TEST_PROGRAM := test/mirilla-test
TEST_SOURCES := test/mirilla-test.c test/test-suite.c test/test-self.c test/test-concurrency.c \
                test/test-invariants.c test/test-ioctl.c test/test-layout.c test/test-exception.c
TEST_OBJECTS := $(TEST_SOURCES:.c=.o)

all: module test

clean:
	cp $(MIRILLA_SYMBOL_LIST) $(MIRILLA_SYMBOL_LIST).preserve
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	mv $(MIRILLA_SYMBOL_LIST).preserve $(MIRILLA_SYMBOL_LIST)
	rm -f $(TEST_PROGRAM) $(TEST_OBJECTS) compile_commands.json test/compile_commands.json \
		../catalejo-sys/compile_commands.json include/*.generated.h

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

module:
ifeq ($(MIRILLA_STEALTH_MODE),1)
	$(MAKE) -C $(KDIR) M=$(PWD) CONFIG_DEBUG_INFO_BTF_MODULES= modules
	$(MIRILLA_STEALTH_STRIP) --strip-unneeded $(MIRILLA_MODULE_NAME).ko
else
	$(MAKE) -C $(KDIR) M=$(PWD) modules
endif

$(TEST_OBJECTS): test/%.o: test/%.c $(wildcard test/*.h) $(wildcard include/*.h)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_PROGRAM): $(TEST_OBJECTS) $(CATALEJO_SYS_SRCS) $(wildcard $(CATALEJO_SYS_DIR)/include/*.h)
	$(CC) -o $@ $(TEST_OBJECTS) $(CATALEJO_SYS_SRCS) $(TEST_CFLAGS)

# Build only. Running the integration test requires a loaded Mirilla module.
test: $(TEST_PROGRAM)

# --- Developer Experience (DX) Targets ---

# Formats C sources and headers using clang-format
format:
	clang-format -style=file -i $(wildcard src/*.c include/*.h test/*.c test/*.h test/kunit/*.c)

# Generate distinct clangd databases for kernel and userspace source trees.
language-server: clean
	@set -eu; \
	database_directory=$$(mktemp -d); \
	trap 'rm -rf "$$database_directory"' EXIT; \
	bear --output "$$database_directory/kernel.json" -- $(MAKE) module; \
	bear --output "$$database_directory/userspace.json" -- $(MAKE) test; \
	python3 tools/clangd-database.py --input "$$database_directory/kernel.json" \
		--output compile_commands.json --root src --sanitize-kernel; \
	python3 tools/clangd-database.py --input "$$database_directory/userspace.json" \
		--output test/compile_commands.json --root test; \
	python3 tools/clangd-database.py --input "$$database_directory/userspace.json" \
		--output ../catalejo-sys/compile_commands.json --root ../catalejo-sys/c

.PHONY: all clean install test module format language-server FORCE

FORCE:
