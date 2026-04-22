# Build lsh as a small, statically-linked /bin/sh for jvmlab minimal Linux.
#
# Default toolchain is musl (musl-gcc). Override with `make CC=cc` or via
# jvmlab-build's JVMLAB_CC=... environment variable. CFLAGS / LDFLAGS may
# be overridden from the environment if the caller needs to retune.

# Honour an explicit CC from the command line or environment, but ignore
# make's built-in default of `cc` so a bare `make` picks musl-gcc.
ifeq ($(origin CC),default)
CC      := musl-gcc
endif
# Hardening flags, rationale:
#   -fstack-protector-strong    Canaries on any function with a local
#                               buffer or pointer-typed local.
#   -fstack-clash-protection    Probe guard pages on large stack
#                               allocations so a VLA / alloca cannot
#                               jump over the guard into adjacent VM.
#   -fcf-protection=full        Intel CET: endbr64 at indirect-call
#                               targets (IBT) + shadow-stack annotations.
#                               Zero-cost NOPs on non-CET CPUs.
#   -ftrivial-auto-var-init=zero
#                               Zero every uninitialised stack variable
#                               on entry (pairs with the kernel's
#                               CONFIG_INIT_STACK_ALL_ZERO=y).
#   -fPIE                       Emit position-independent code so the
#                               link step can produce a PIE binary.
#   -Wformat=2 -Werror=format-security
#                               Refuse to compile user-supplied format
#                               strings (classic printf-exploit hole).
#   -D_FORTIFY_SOURCE=2         Compile-time bounds checks on mem/str*.
#
# Linker flags:
#   -static-pie                 Static binary that is ALSO relocatable,
#                               so the kernel ASLRs it on every exec.
#                               Requires musl >= 1.1.20 (we target
#                               rolling-release distros; fine on CI's
#                               ubuntu-24.04 and on CachyOS).
#   -Wl,-z,noexecstack          Mark GNU_STACK non-executable.
#   -Wl,-z,relro -Wl,-z,now     Full RELRO: resolve all relocations at
#                               load time, then map GOT/.data.rel.ro RO.
#   -Wl,--gc-sections           Drop unreferenced sections (works with
#                               -ffunction-sections/-fdata-sections).
CFLAGS  ?= -std=c11 -Os \
           -ffunction-sections -fdata-sections \
           -Wall -Wextra -Wformat=2 -Werror=format-security \
           -fstack-protector-strong \
           -fstack-clash-protection \
           -fcf-protection=full \
           -ftrivial-auto-var-init=zero \
           -fPIE \
           -D_FORTIFY_SOURCE=2
LDFLAGS ?= -static-pie \
           -Wl,-z,noexecstack \
           -Wl,-z,relro -Wl,-z,now \
           -Wl,--gc-sections

SRC     := src/main.c
TARGET  := lsh

DESTDIR ?=
BINDIR  ?= /bin

.PHONY: all clean run install smoke

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

# Install lsh and a /bin/sh symlink pointing at it.
install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	ln -sf $(TARGET) $(DESTDIR)$(BINDIR)/sh

smoke: $(TARGET)
	LSH=$(abspath $(TARGET)) sh tests/smoke.sh

clean:
	rm -f $(TARGET)
