# XPadMac — top-level build.
#
#   make test             shared-core unit tests (Tests/)
#   make tool             build Tools/xpadctl/xpadctl
#   make project          generate XPadMac.xcodeproj from project.yml (xcodegen)
#   make build-unsigned   build the app + driver with no signing (any Mac, no account)
#   make build            build signed (Apple Development), registering this Mac and
#                         creating the profiles as needed: needs your own TEAM and PREFIX
#   make check-product    check the built app's embedded dext and its personalities
#   make install          copy the built app to /Applications/XPadMac.app and open it
#   make uninstall        remove /Applications/XPadMac.app
#   make clean            remove build products
#
# TEAM (DEVELOPMENT_TEAM) and PREFIX (bundle ID prefix, ends with a dot) come
# from Config.xcconfig, overridden by Config.local.xcconfig (git-ignored: put
# your own team and prefix there), or from the command line / environment:
#   make build TEAM=ABCDE12345 PREFIX=com.yourname.
# Also overridable: CONFIG (Debug or Release), BUILD_NUMBER (CFBundleVersion of
# `make build`), APP_PROFILE (sign the app with this portal profile; see
# Config.xcconfig), APP_DEVID_PROFILE (re-sign the app with this installed
# Developer ID profile).
#
# SPDX-License-Identifier: GPL-3.0-or-later

# The last `NAME = value` line for NAME in Config.xcconfig, then in
# Config.local.xcconfig if it exists (comments and blank values allowed).
xcconfig_value = $(strip $(shell sed -n 's|^[[:space:]]*$(1)[[:space:]]*=[[:space:]]*\([^/[:space:]]*\).*|\1|p' \
                     Config.xcconfig $(wildcard Config.local.xcconfig) 2>/dev/null | tail -n 1))
ifeq ($(origin TEAM),undefined)
TEAM := $(call xcconfig_value,DEVELOPMENT_TEAM)
endif
ifeq ($(origin PREFIX),undefined)
PREFIX := $(call xcconfig_value,BUNDLE_ID_PREFIX)
endif
PLACEHOLDER_PREFIX := com.example.
override PREFIX := $(or $(PREFIX),$(PLACEHOLDER_PREFIX))
CONFIG  ?= Debug
# sysextd replaces an installed system extension only when the new one has
# another version ("activateDecision found existing entry of same version"
# otherwise: the request succeeds and the old driver keeps running). So
# every `make build` gets its own CFBundleVersion, from the UTC time:
# DAYS.HOUR.MINUTE, DAYS counted from 2020-01-01 (e.g. 2503.8.17). A dext's
# CFBundleVersion must be a kext version (kernelmanagerd: "must be a valid
# kext version": at most 9999.99.99), so a plain counter such as seconds
# since 1970 is refused; this one stays valid until 2047 and only repeats
# for two builds in the same minute. Evaluated once per make run (:=), so
# the app and the dext carry the same number.
BUILD_NUMBER := $(or $(BUILD_NUMBER),$(shell date -u '+%s %H %M' | awk '{ printf "%d.%d.%d", int($$1 / 86400) - 18262, $$2, $$3 }'))

XCODEGEN   ?= xcodegen
XCODEBUILD ?= xcodebuild
PYTHON     ?= python3

PROJECT     := XPadMac.xcodeproj
SCHEME      := XPadMac
DERIVED     := build/DerivedData
PRODUCTS    := $(DERIVED)/Build/Products/$(CONFIG)
APP         := $(PRODUCTS)/XPadMac.app
DEXT        := $(APP)/Contents/Library/SystemExtensions/$(PREFIX)xpadmac.driver.dext
INSTALL_APP := /Applications/XPadMac.app

XCODEBUILD_FLAGS := -project $(PROJECT) -scheme $(SCHEME) -configuration $(CONFIG) \
                    -derivedDataPath $(DERIVED) -destination 'generic/platform=macOS'

# TEAM and PREFIX reach xcodebuild through build/make.xcconfig, which
# Config.xcconfig includes last, not as command-line build settings: with
# BUNDLE_ID_PREFIX overridden on the command line, Xcode builds the dext under
# the new name but embeds it under the xcconfig's ("no such file").
MAKE_XCCONFIG := build/make.xcconfig
define write_make_xcconfig
	@mkdir -p $(dir $(MAKE_XCCONFIG))
	@printf '%s\n' '// Written by make (see Makefile): the TEAM and PREFIX of the last build.' \
	    'DEVELOPMENT_TEAM = $(TEAM)' 'BUNDLE_ID_PREFIX = $(PREFIX)' > $(MAKE_XCCONFIG)
endef

.DEFAULT_GOAL := help
.PHONY: help test tool project build-unsigned build check-signing check-product check-profiles install uninstall clean

help:
	@sed -n '3,23p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'
	@echo "Now: TEAM=$(or $(TEAM),(none)) PREFIX=$(PREFIX) CONFIG=$(CONFIG)"

test:
	$(MAKE) -C Tests

tool:
	$(MAKE) -C Tools/xpadctl

# Always regenerated (it takes a second): xcodegen lists the source folders
# when it runs, so a file added to App/, Driver/ or Shared/ needs a new project.
project:
	$(XCODEGEN) generate --spec project.yml

build-unsigned: project
	$(write_make_xcconfig)
	$(XCODEBUILD) $(XCODEBUILD_FLAGS) CODE_SIGNING_ALLOWED=NO build
	@$(MAKE) --no-print-directory check-product
	@echo "Built (unsigned): $(APP)"

# The signing style is per target (project.yml): the dext is always signed
# automatically; the app too, unless APP_PROFILE names a portal profile.
APP_PROFILE ?=
APP_DEVID_PROFILE ?=
APP_SIGNING := $(if $(APP_PROFILE),XPAD_APP_SIGN_STYLE=Manual XPAD_APP_PROFILE="$(APP_PROFILE)")

# A signed build needs your own team, and a prefix of your own: an App ID is
# unique across all teams, so com.example.xpadmac is not yours to register.
check-signing:
	@if [ -z "$(TEAM)" ]; then \
	  echo "error: no Apple Developer team set (DEVELOPMENT_TEAM is empty)." >&2; \
	  echo "  Put your Team ID and bundle ID prefix in Config.local.xcconfig (git-ignored):" >&2; \
	  echo "      DEVELOPMENT_TEAM = ABCDE12345" >&2; \
	  echo "      BUNDLE_ID_PREFIX = com.yourname." >&2; \
	  echo "  or run: make build TEAM=ABCDE12345 PREFIX=com.yourname." >&2; \
	  echo "  (make build-unsigned needs neither, but its driver cannot be installed.)" >&2; \
	  exit 1; \
	fi
	@if [ "$(PREFIX)" = "$(PLACEHOLDER_PREFIX)" ]; then \
	  echo "error: BUNDLE_ID_PREFIX is the placeholder $(PLACEHOLDER_PREFIX): set your own (it ends with a dot)" >&2; \
	  echo "  in Config.local.xcconfig (BUNDLE_ID_PREFIX = com.yourname.) or run make build PREFIX=com.yourname." >&2; \
	  exit 1; \
	fi
	@case "$(PREFIX)" in *.) ;; *) echo "error: PREFIX '$(PREFIX)' must end with a dot" >&2; exit 1 ;; esac

build: check-signing project
	$(write_make_xcconfig)
	$(XCODEBUILD) $(XCODEBUILD_FLAGS) $(APP_SIGNING) CURRENT_PROJECT_VERSION=$(BUILD_NUMBER) \
	    -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
	$(if $(APP_DEVID_PROFILE),$(PYTHON) -I Tools/resign-app.py "$(APP)" "$(APP_DEVID_PROFILE)" $(TEAM))
	@$(MAKE) --no-print-directory check-product check-profiles
	@echo "Built (signed for team $(TEAM), build $(BUILD_NUMBER)): $(APP)"

# AMFI runs code with restricted entitlements (the app's system-extension
# install, the dext's DriverKit ones) only if its profile lists this Mac by
# its Provisioning UDID (System Information > Hardware) and grants every
# one of them: each bundle's signed entitlements against its profile's.
check-profiles:
	@$(PYTHON) -I Tests/check_profiles.py "$(APP)" "$(DEXT)"

# The dext must sit where sysextd looks for it, and its expanded
# personalities must still be ones IOUSBHostFamily can match.
check-product:
	@test -f "$(DEXT)/Info.plist" || { echo "No $(DEXT) in the app bundle." >&2; exit 1; }
	@$(PYTHON) -I Tests/check_personalities.py "$(DEXT)/Info.plist"

# macOS activates a system extension only from an app in /Applications, and
# only from one signed with a provisioning profile (an Apple Development
# build): a `make build-unsigned` app installs, but its driver cannot.
install:
	@test -d "$(APP)" || { echo "No $(APP): run 'make build' first." >&2; exit 1; }
	@if [ ! -f "$(APP)/Contents/embedded.provisionprofile" ] || \
	    ! ls "$(APP)"/Contents/Library/SystemExtensions/*.dext/embedded.provisionprofile >/dev/null 2>&1; then \
	  echo "warning: $(APP) is not signed with a provisioning profile (a 'make build-unsigned' build?):" >&2; \
	  echo "warning: macOS will refuse to install its driver. Build it with 'make build' for that." >&2; \
	fi
	rm -rf "$(INSTALL_APP)"
	ditto "$(APP)" "$(INSTALL_APP)"
	open "$(INSTALL_APP)"

# Removing the app does not remove an activated driver: turn it off in the app
# first (XPadMac > Driver, switch off, "Turn Off Driver"); deactivation needs
# the app, SIP stays on.
uninstall:
	@echo "Deactivate the driver first: XPadMac > Driver, turn the switch off and confirm with \"Turn Off Driver\"."
	rm -rf "$(INSTALL_APP)"

clean:
	rm -rf build
	-$(MAKE) -C Tests clean
	-$(MAKE) -C Tools/xpadctl clean
