# xpadctl — XPadMac's diagnostic tool, built with the host clang.
#
#   make                  build ./xpadctl (doctor, hid, gc)
#   make WITH_LIBUSB=1    also the `gip` command (Homebrew libusb, LIBUSB_PREFIX)
#   make syntax           compile-check the tool's own sources only (no Shared/*.c needed)
#   make clean
#
# Objects go to build/plain or build/libusb, so switching WITH_LIBUSB never
# mixes objects built with different flags; ./xpadctl is the last one built.
#
# SPDX-License-Identifier: GPL-3.0-or-later

ifeq ($(origin CC),default)
CC := clang
endif

SHARED        := ../../Shared
LIBUSB_PREFIX ?= /opt/homebrew

# The plain tool runs on what the app supports (macOS 14+). Homebrew's libusb
# is built for this Mac's macOS, so the libusb variant targets that instead
# (otherwise ld warns that the dylib is newer than the deployment target).
ifeq ($(WITH_LIBUSB),1)
VARIANT   := libusb
MACOS_MIN ?= $(shell sw_vers -productVersion | cut -d. -f1-2)
else
VARIANT   := plain
MACOS_MIN ?= 14.0
endif
BUILD := build/$(VARIANT)

TOOL_C  := main.c util.c xpad_log_host.c doctor.c hid.c
TOOL_M  := gc.m
SHARED_C := xpad_core.c xpad_xbox_desc.c gip.c gip_auth.c gip_crypto.c

WARN     := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
# _DARWIN_C_SOURCE: clock_gettime_nsec_np, strcasestr, strtok_r under strict C99.
CPPFLAGS += -I. -I$(SHARED) -D_DARWIN_C_SOURCE -MMD -MP
CFLAGS   ?= -O2 -g
COMMON   := -mmacosx-version-min=$(MACOS_MIN) $(WARN)
C_STD    := -std=c99
OBJCFLAGS := -fobjc-arc
LDLIBS   := -framework IOKit -framework CoreFoundation -framework Foundation -framework GameController \
            -framework Security

ifeq ($(WITH_LIBUSB),1)
TOOL_C   += gip_cmd.c
CPPFLAGS += -DXPADCTL_WITH_LIBUSB=1 -I$(LIBUSB_PREFIX)/include
LDLIBS   += -L$(LIBUSB_PREFIX)/lib -lusb-1.0
endif

TOOL_OBJ   := $(TOOL_C:%.c=$(BUILD)/%.o) $(TOOL_M:%.m=$(BUILD)/%.o)
SHARED_OBJ := $(SHARED_C:%.c=$(BUILD)/shared/%.o)
BIN        := $(BUILD)/xpadctl

.PHONY: all clean syntax xpadctl

all: xpadctl

# ./xpadctl is whichever variant was asked for last: phony, and copied only
# when it differs, so `make` then `make WITH_LIBUSB=1` then `make` is right.
xpadctl: $(BIN)
	@# A new file, never written over the old one: macOS (Apple silicon) keeps
	@# the code signature of a binary that ran cached by its vnode, and kills
	@# (SIGKILL, exit 137) a new one copied over the same inode.
	@cmp -s $(BIN) $@ || { echo "cp $(BIN) $@"; cp -f $(BIN) $@.tmp && mv -f $@.tmp $@; }

$(BIN): $(TOOL_OBJ) $(SHARED_OBJ)
	$(CC) $(COMMON) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(C_STD) $(COMMON) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.m | $(BUILD)
	$(CC) $(COMMON) $(OBJCFLAGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# The shared core is pure C99 and must stay warning-free for DriverKit too.
$(BUILD)/shared/%.o: $(SHARED)/%.c | $(BUILD)/shared
	$(CC) $(C_STD) $(COMMON) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/shared:
	mkdir -p $@

# Checks the tool's own files against the headers alone (both variants, so the
# libusb code is checked too when libusb's header is installed).
syntax:
	@for f in main.c util.c xpad_log_host.c doctor.c hid.c gip_cmd.c; do \
	  echo "syntax $$f"; \
	  $(CC) $(C_STD) $(COMMON) -I. -I$(SHARED) -D_DARWIN_C_SOURCE -fsyntax-only $$f || exit 1; \
	done
	@echo "syntax gc.m"; $(CC) $(COMMON) $(OBJCFLAGS) -I. -I$(SHARED) -fsyntax-only gc.m
	@if [ -f $(LIBUSB_PREFIX)/include/libusb-1.0/libusb.h ]; then \
	  echo "syntax gip_cmd.c main.c (WITH_LIBUSB)"; \
	  for f in gip_cmd.c main.c; do \
	    $(CC) $(C_STD) $(COMMON) -I. -I$(SHARED) -D_DARWIN_C_SOURCE -DXPADCTL_WITH_LIBUSB=1 \
	      -I$(LIBUSB_PREFIX)/include -fsyntax-only $$f || exit 1; \
	  done; \
	fi

clean:
	rm -rf build xpadctl

-include $(TOOL_OBJ:.o=.d) $(SHARED_OBJ:.o=.d)
