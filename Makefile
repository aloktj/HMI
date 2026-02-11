# ============================================================
# HMI Web Application Makefile
#
# Dependencies:
#   - TRDP library (import/3.0.0.0)
#   - Crow C++ HTTP framework (import/Crow-master)
#   - Boost ASIO headers (required by Crow)
#   - pthreads, rt, uuid
# ============================================================

TRDP_DIR  := import/3.0.0.0
TRDP_OUT  := $(TRDP_DIR)/bld/output/linux-rel
CROW_DIR  := import/Crow-master
CROW_INC  := $(CROW_DIR)/include

APP       := hmi_webapp
CXX       ?= g++
CXXFLAGS  ?= -std=c++17 -Wall -Wextra -O2 -DPOSIX -DMD_SUPPORT=1 -DCROW_USE_BOOST
INCLUDES  := -Iinclude -I$(CROW_INC) -I$(TRDP_DIR)/src/api -I$(TRDP_DIR)/src/vos/api
LDFLAGS   := -L$(TRDP_OUT)
LDLIBS    := -ltrdp -lpthread -lm -lrt -luuid -lboost_system

.PHONY: help trdp-help trdp-config trdp-lib app run clean

help:
	@echo "HMI Web Application build targets"
	@echo "  make trdp-lib       Build TRDP static libraries"
	@echo "  make app            Build HMI web application (default)"
	@echo "  make run            Build and run with default settings"
	@echo "  make clean          Clean app and TRDP build artifacts"
	@echo ""
	@echo "Runtime:"
	@echo "  ./$(APP) [own_ip] [gw_ip] [mc_a] [mc_b] [web_port] [web_dir]"
	@echo "  Defaults: 192.168.56.2 192.168.56.1 239.192.0.1 239.192.0.2 8080 web"

trdp-help:
	@$(MAKE) -C $(TRDP_DIR) help

trdp-config:
	@$(MAKE) -C $(TRDP_DIR) LINUX_config

trdp-lib: trdp-config
	@$(MAKE) -C $(TRDP_DIR) libtrdp
	@$(MAKE) -C $(TRDP_DIR) libtrdpap

CROW_HEADER := $(CROW_INC)/crow.h

# Crow include sanity check (added under import/)

$(APP): src/hmi_main.cpp include/hmi_trdp.h $(CROW_HEADER) | trdp-lib
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) $(LDLIBS)

app: $(APP)

run: $(APP)
	./$(APP)

clean:
	@rm -f $(APP)
	@$(MAKE) -C $(TRDP_DIR) clean || true
