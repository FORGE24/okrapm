# Pack compile products with OPSIS as part of a Make build.
# Set the variables below, include this file, then add opsis-package to all:
#
#   OPSIS ?= opsis
#   OPSIS_PKG_NAMESPACE ?= Okra
#   OPSIS_PKG_NAME = lunar
#   OPSIS_PKG_VERSION = 0.1.0
#   OPSIS_PKG_DESCRIPTION = Lunar package manager
#   OPSIS_PKG_SCRIPT = package/lunar.opsis
#   OPSIS_PKG_OUTPUT = lunar.oaa
#   OPSIS_PKG_DEPS = lunar
#   OPSIS_BUILD_DIR ?= $(CURDIR)
#   OPSIS_BIN_LUNAR = $(CURDIR)/lunar
#   include /path/to/opsis/opsis-package.mk
#   all: opsis-package

ifeq ($(OPSIS_PKG_NAME),)
$(error opsis-package.mk needs OPSIS_PKG_NAME, OPSIS_PKG_VERSION, OPSIS_PKG_SCRIPT and OPSIS_PKG_OUTPUT)
endif
ifeq ($(OPSIS_PKG_VERSION),)
$(error opsis-package.mk needs OPSIS_PKG_VERSION)
endif
ifeq ($(OPSIS_PKG_SCRIPT),)
$(error opsis-package.mk needs OPSIS_PKG_SCRIPT)
endif
ifeq ($(OPSIS_PKG_OUTPUT),)
$(error opsis-package.mk needs OPSIS_PKG_OUTPUT)
endif

OPSIS ?= opsis
OPSIS_PKG_NAMESPACE ?= Okra
OPSIS_BUILD_DIR ?= $(CURDIR)

.PHONY: opsis-package
opsis-package: $(OPSIS_PKG_OUTPUT)

$(OPSIS_PKG_OUTPUT): $(OPSIS_PKG_SCRIPT) $(OPSIS_PKG_DEPS)
	OPSIS_PKG_NAMESPACE='$(OPSIS_PKG_NAMESPACE)' \
	OPSIS_PKG_NAME='$(OPSIS_PKG_NAME)' \
	OPSIS_PKG_VERSION='$(OPSIS_PKG_VERSION)' \
	OPSIS_PKG_DESCRIPTION='$(OPSIS_PKG_DESCRIPTION)' \
	OPSIS_PKG_OUTPUT='$(OPSIS_PKG_OUTPUT)' \
	OPSIS_BUILD_DIR='$(OPSIS_BUILD_DIR)' \
	OPSIS_BIN_LUNAR='$(OPSIS_BIN_LUNAR)' \
	OPSIS_BIN_OPSIS='$(OPSIS_BIN_OPSIS)' \
	$(OPSIS_PACK_EXTRA) \
	OPSIS_ALLOW_NONROOT=1 \
	$(OPSIS) pack --allow-nonroot '$(OPSIS_PKG_SCRIPT)'
