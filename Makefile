TRDP_DIR := import/3.0.0.0
TRDP_OUT := $(TRDP_DIR)/bld/output/linux-rel
APP := HMI
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -DPOSIX -DMD_SUPPORT=1
INCLUDES := -Iinclude -I$(TRDP_DIR)/src/api -I$(TRDP_DIR)/src/vos/api
LDFLAGS := -L$(TRDP_OUT)
LDLIBS := -ltrdp -lpthread -lm -lrt -luuid

.PHONY: help trdp-help trdp-config trdp-lib app clean

help:
	@echo "HMI build targets"
	@echo "  make trdp-help    # show TRDP stack help"
	@echo "  make trdp-lib     # build TRDP static libraries"
	@echo "  make app          # build HMI executable (default)"
	@echo "  make clean        # clean app and TRDP build artifacts"

trdp-help:
	@$(MAKE) -C $(TRDP_DIR) help

trdp-config:
	@$(MAKE) -C $(TRDP_DIR) LINUX_config

trdp-lib: trdp-config
	@$(MAKE) -C $(TRDP_DIR) libtrdp
	@$(MAKE) -C $(TRDP_DIR) libtrdpap

$(APP): src/hmi_main.c include/hmi_trdp.h | trdp-lib
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) $(LDLIBS)

app: $(APP)

clean:
	@rm -f $(APP)
	@$(MAKE) -C $(TRDP_DIR) clean || true
