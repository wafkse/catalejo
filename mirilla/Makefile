obj-m += mirilla.o

# Automatically include all .c files from src/ directory
mirilla-objs := $(patsubst %.c,%.o,$(wildcard src/*.c))

# Add include directory
ccflags-y := -I$(src)/include

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

# --- Test Suite Build ---

# The suites read peephole views through the catalejo-fault subsystem, so
# its C sources are compiled straight into every test binary.
CATALEJO_FAULT_DIR := ../catalejo-fault/c
CATALEJO_FAULT_SRCS := $(wildcard $(CATALEJO_FAULT_DIR)/src/*.c)

TEST_CFLAGS := -I. -I$(CATALEJO_FAULT_DIR)/include -Wall -Wextra -std=c11

TEST_SUITES := test/test-suite test/test-self test/test-concurrency \
	       test/test-invariants test/test-ioctl

all: module tests

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TEST_SUITES) compile_commands.json

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test/test-concurrency: TEST_CFLAGS += -pthread

test/%: test/%.c test/test-harness.h $(wildcard include/*.h) $(CATALEJO_FAULT_SRCS)
	gcc -o $@ $< $(CATALEJO_FAULT_SRCS) $(TEST_CFLAGS)

tests: $(TEST_SUITES)
	@echo "Test suites built successfully!"
	@echo "  - test/test-suite          : Basic functionality tests"
	@echo "  - test/test-self           : Self-observation tests"
	@echo "  - test/test-concurrency    : Concurrency and race tests"
	@echo "  - test/test-invariants     : Mapping invariant tests"
	@echo "  - test/test-ioctl          : Ioctl error-path tests"
	@echo ""
	@echo "Make sure the mirilla module is loaded first."

test: tests

run-tests: tests
	cd test && ./run-tests.sh

# --- Developer Experience (DX) Targets ---

# Generates compile_commands.json for Clangd
language-server: clean
	bear -- $(MAKE) all

.PHONY: all clean install test tests run-tests module language-server
