#
# Makefile for besmart-energy-gw
#

SIL ?= @
MAKEFLAGS += --no-print-directory

TARGET ?= armv7m7-imxrt106x

include ../phoenix-rtos-build/Makefile.common
include ../phoenix-rtos-build/Makefile.$(TARGET_SUFF)


.PHONY: clean
clean:
	@echo "rm -rf $(BUILD_DIR)"

ifneq ($(filter clean,$(MAKECMDGOALS)),)
	$(shell rm -rf $(BUILD_DIR))
endif

T1 := $(filter-out clean all,$(MAKECMDGOALS))
ifneq ($(T1),)
	include $(T1)/Makefile
.PHONY: $(T1)
$(T1):
	@echo >/dev/null
else
	include gw/Makefile
	
endif
