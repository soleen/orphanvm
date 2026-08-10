# SPDX-License-Identifier: GPL-2.0-only
CC ?= gcc
CFLAGS ?= -g -Os -static -Wall -Wextra
LDFLAGS ?=

ARCH ?= x86_64
ifeq ($(ARCH),aarch64)
  BUILD_ARCH = arm64
else ifeq ($(ARCH),arm64)
  BUILD_ARCH = arm64
else
  BUILD_ARCH = $(ARCH)
endif

OUT_DIR ?= .

TARGET_AGENT = $(OUT_DIR)/ovm_agent
TARGET_MONITOR = $(OUT_DIR)/ovm_monitor
TARGET_IPI = $(OUT_DIR)/ovm_ipi_test

all: $(TARGET_AGENT) $(TARGET_MONITOR) $(TARGET_IPI)

$(TARGET_AGENT): ovm_agent.c ovm_workload.h
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ ovm_agent.c $(LDFLAGS)

$(TARGET_MONITOR): ovm_monitor.c ovm_workload.h ovm_llama_stream.h
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ ovm_monitor.c $(LDFLAGS)

$(TARGET_IPI): ovm_ipi_test.c ovm_workload.h
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ ovm_ipi_test.c $(LDFLAGS) -lpthread

clean:
	rm -f $(TARGET_AGENT) $(TARGET_MONITOR) $(TARGET_IPI) $(OUT_DIR)/guest_agent $(OUT_DIR)/guest_monitor

.PHONY: all clean
