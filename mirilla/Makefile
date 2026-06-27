obj-m += mirilla.o

# Automatically include all .c files from src/ directory
mirilla-objs := $(patsubst %.c,%.o,$(wildcard src/*.c))

# Add include directory
ccflags-y := -I$(src)/include

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

all: module test

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test-suite compile_commands.json

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test/test-suite: test/test-suite.c
	gcc -o test-suite test/test-suite.c -I. -Wall -Wextra -std=c11

test: test/test-suite
	@echo "Test suites built successfully!"
	@echo "  - test/test-suite          : Basic functionality tests"
	@echo ""
	@echo "Make sure the mirilla module is loaded first."
	cp test-suite mirilla.ko ~/virtual-machine/shared/

# --- Developer Experience (DX) Targets ---

# Generates compile_commands.json for Clangd
language-server: clean
	bear -- $(MAKE) all

.PHONY: all clean install test module language-server
