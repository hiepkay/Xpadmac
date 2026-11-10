# XPadMac shared-core tests: `make -C Tests` builds them with host clang and
# runs them all, after checking that Shared/*.c builds for DriverKit, that
# Shared/xpad_xbox_desc.c is what the generator makes from the capture, that
# auth_vectors.h is what gen_auth_vectors.py makes (with OpenSSL's checks),
# that every driver personality is one IOUSBHostFamily can match, and that
# the dext's entitlements are the ones the driver needs.
#
#   make -C Tests              everything (the default)
#   make -C Tests test         unit tests only
#   make -C Tests driverkit    DriverKit build check of Shared/*.c only
#   make -C Tests personalities  Driver/Info.plist matching-key check only
#   make -C Tests entitlements Driver/XPadDriver.entitlements check only
#   make -C Tests desc         regenerate Shared/xpad_xbox_desc.c from the .bin
#   make -C Tests auth-vectors regenerate auth_vectors.h (python3 and openssl)
#   make -C Tests clean
#
#   SAN= (empty) builds without the address and undefined-behaviour sanitizers.
#   XPAD_TEST_VERBOSE=1 prints what the GIP code logs.
#
# SPDX-License-Identifier: GPL-3.0-or-later

ifeq ($(origin CC),default)
CC = clang
endif
PYTHON ?= python3
# gen_auth_vectors.py cross-checks RSA and ECDH with openssl (Homebrew's, or the system's LibreSSL).
OPENSSL ?= $(firstword $(wildcard /opt/homebrew/bin/openssl /usr/local/bin/openssl) openssl)

SHARED   := ../Shared
DRIVER   := ../Driver
RESEARCH := $(abspath ../docs/research)
B        := build

# The shared core is held to the strictest set; the tests to the usual one.
STD          := -std=c99 -pedantic
WARN         := -Wall -Wextra -Werror
SHARED_WARN  := $(WARN) -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes \
                -Wmissing-prototypes -Wcast-qual -Wformat=2
SAN          ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
OPT          := -O1 -g
CPPFLAGS     := -I$(SHARED) -I.

SHARED_SRC := $(SHARED)/xpad_core.c $(SHARED)/xpad_xbox_desc.c \
              $(SHARED)/xpad_x360.c $(SHARED)/gip.c $(SHARED)/gip_auth.c $(SHARED)/gip_crypto.c
SHARED_HDR := $(SHARED)/xpad.h $(SHARED)/gip.h $(SHARED)/gip_auth.h $(SHARED)/gip_crypto.h
CORE_OBJ   := $(B)/xpad_core.o $(B)/xpad_xbox_desc.o $(B)/gip.o $(B)/gip_auth.o $(B)/gip_crypto.o
HARNESS    := $(B)/test.o $(B)/xpad_log_host.o $(B)/hid_walk.o

TESTS := $(B)/test_desc $(B)/test_xbox $(B)/test_x360 $(B)/test_gip $(B)/test_crypto $(B)/test_auth

.PHONY: all test driverkit desc desc-check auth-vectors auth-vectors-check personalities entitlements clean
.DEFAULT_GOAL := all
.SECONDARY:                  # keep the objects of chained pattern rules

all: driverkit desc-check auth-vectors-check personalities entitlements test

test: $(TESTS)
	@echo "running tests"
	@set -e; \
	$(B)/test_desc $(RESEARCH); \
	$(B)/test_xbox; \
	$(B)/test_x360; \
	$(B)/test_gip $(RESEARCH); \
	$(B)/test_crypto; \
	$(B)/test_auth; \
	echo "all tests passed"

$(B):
	@mkdir -p $(B)

# ---- the shared core, as the driver and the tool compile it ------------------

$(B)/%.o: $(SHARED)/%.c $(SHARED_HDR) | $(B)
	$(CC) $(STD) $(OPT) $(SHARED_WARN) $(SAN) $(CPPFLAGS) -c $< -o $@

# ---- the harness and the tests ---------------------------------------------

$(B)/%.o: %.c test.h hid_walk.h $(SHARED_HDR) | $(B)
	$(CC) $(STD) $(OPT) $(WARN) $(SAN) $(CPPFLAGS) -c $< -o $@

$(B)/test_desc: $(B)/test_desc.o $(CORE_OBJ) $(HARNESS)
	$(CC) $(SAN) $^ -o $@

$(B)/test_xbox: $(B)/test_xbox.o $(CORE_OBJ) $(HARNESS)
	$(CC) $(SAN) $^ -o $@

$(B)/test_x360: $(B)/test_x360.o $(B)/xpad_x360.o $(CORE_OBJ) $(HARNESS)
	$(CC) $(SAN) $^ -o $@

$(B)/test_gip: $(B)/test_gip.o $(CORE_OBJ) $(HARNESS)
	$(CC) $(SAN) $^ -o $@

$(B)/test_crypto.o $(B)/test_auth.o: auth_vectors.h

$(B)/test_crypto: $(B)/test_crypto.o $(CORE_OBJ) $(HARNESS)
	$(CC) $(SAN) $^ -o $@

$(B)/test_auth: $(B)/test_auth.o $(CORE_OBJ) $(HARNESS)
	$(CC) $(SAN) $^ -o $@

# ---- DriverKit -----------------------------------------------------------------

driverkit:
	@sh driverkit_check.sh $(SHARED) $(B)/driverkit

# ---- the descriptor, generated from the capture -----------------------------

desc:
	$(PYTHON) -I gen_xbox_desc.py $(RESEARCH)/xbox_series_ble_0b13_descriptor.bin \
	    $(RESEARCH)/xbox_series_ble_0b13_descriptor_commented.c $(SHARED)/xpad_xbox_desc.c

desc-check: | $(B)
	@if command -v $(PYTHON) >/dev/null 2>&1; then \
	    $(PYTHON) -I gen_xbox_desc.py $(RESEARCH)/xbox_series_ble_0b13_descriptor.bin \
	        $(RESEARCH)/xbox_series_ble_0b13_descriptor_commented.c $(B)/xpad_xbox_desc.c && \
	    cmp -s $(B)/xpad_xbox_desc.c $(SHARED)/xpad_xbox_desc.c && \
	    echo "desc: Shared/xpad_xbox_desc.c matches the capture" || \
	    { echo "desc: Shared/xpad_xbox_desc.c is not what gen_xbox_desc.py makes; run make -C Tests desc" >&2; exit 1; }; \
	else echo "desc: no $(PYTHON); generator check skipped (test_desc still compares the bytes)"; fi

# ---- the authentication vectors, made from Tests/vectors/ and checked with OpenSSL ----

auth-vectors:
	OPENSSL=$(OPENSSL) $(PYTHON) -I gen_auth_vectors.py vectors auth_vectors.h

auth-vectors-check: | $(B)
	@if command -v $(PYTHON) >/dev/null 2>&1 && command -v $(OPENSSL) >/dev/null 2>&1; then \
	    OPENSSL=$(OPENSSL) $(PYTHON) -I gen_auth_vectors.py vectors $(B)/auth_vectors.h && \
	    cmp -s $(B)/auth_vectors.h auth_vectors.h && \
	    echo "auth-vectors: auth_vectors.h matches gen_auth_vectors.py (RSA and ECDH checked with $(OPENSSL))" || \
	    { echo "auth-vectors: auth_vectors.h is not what gen_auth_vectors.py makes; run make -C Tests auth-vectors" >&2; exit 1; }; \
	else echo "auth-vectors: no $(PYTHON) or $(OPENSSL); generator check skipped (the tests still use auth_vectors.h)"; fi

# ---- the driver's IOKitPersonalities ---------------------------------------------

# IOUSBHostFamily silently ignores a personality whose USB keys are not one
# of QA1076's combinations; check_personalities.py says which are.
personalities:
	@if command -v $(PYTHON) >/dev/null 2>&1; then \
	    $(PYTHON) -I check_personalities.py $(DRIVER)/Info.plist; \
	else echo "personalities: no $(PYTHON); check skipped"; fi

# ---- the dext's entitlements ------------------------------------------------------

entitlements:
	@if command -v $(PYTHON) >/dev/null 2>&1; then \
	    $(PYTHON) -I check_entitlements.py $(DRIVER)/XPadDriver.entitlements $(DRIVER)/Info.plist; \
	else echo "entitlements: no $(PYTHON); check skipped"; fi

clean:
	rm -rf $(B)
