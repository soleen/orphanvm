MAKEFLAGS += --no-print-directory

ARCH ?= x86_64

ifeq ($(ARCH),aarch64)
  LINUX_ARCH = arm64
else
  LINUX_ARCH = $(ARCH)
endif

ifeq ($(LINUX_ARCH),arm64)
  CROSS_COMPILE ?= aarch64-linux-gnu-
  KERNEL_ARCH = arm64
  KERNEL_IMG_NAME = Image
else
  CROSS_COMPILE ?=
  KERNEL_ARCH = x86
  KERNEL_IMG_NAME = bzImage
endif

OUT_DIR = output_$(LINUX_ARCH)
export PATH := $(CURDIR)/$(OUT_DIR)/buildroot_guest/host/bin:$(CURDIR)/$(OUT_DIR)/buildroot_host/host/bin:$(PATH)

GUEST_KERNEL_IMAGE = $(OUT_DIR)/linux_guest/arch/$(KERNEL_ARCH)/boot/$(KERNEL_IMG_NAME)
HOST_KERNEL_IMAGE  = $(OUT_DIR)/linux_host/arch/$(KERNEL_ARCH)/boot/$(KERNEL_IMG_NAME)

GUEST_ROOTFS = $(OUT_DIR)/buildroot_guest/images/rootfs.cpio.lz4
HOST_ROOTFS  = $(OUT_DIR)/buildroot_host/images/rootfs.cpio.lz4
ifeq ($(LINUX_ARCH),arm64)
KERNEL_DISK  = $(OUT_DIR)/kernel_disk_arm.ext4 $(OUT_DIR)/kernel_disk.ext4
else
KERNEL_DISK  = $(OUT_DIR)/kernel_disk_amd.ext4 $(OUT_DIR)/kernel_disk_intel.ext4 $(OUT_DIR)/kernel_disk_simics.ext4 $(OUT_DIR)/kernel_disk.ext4
endif

GUEST_OVERLAY_SRCS = $(shell find overlays/buildroot_guest -type f 2>/dev/null)
GUEST_CONFIG_FRAG  = configs/$(LINUX_ARCH)/buildroot_guest_config_fragment

HOST_OVERLAY_SRCS  = $(shell find overlays/buildroot_host -type f 2>/dev/null)
HOST_CONFIG_FRAG   = configs/$(LINUX_ARCH)/buildroot_host_config_fragment
HOST_SCRIPTS_SRCS  = scripts/ovm_guest scripts/ovm_guest_qemu scripts/ovm_guest_launcher

SCRIPTS = ovm_guest ovm_guest_qemu ovm_guest_launcher ovm_simics ovm_simics_vmx ovm_qemu ovm_remote ovm_kexec ovm_ssh ovm_scp ovm_console ovm_test ovm_demo ovm_build_llama ovm_package_demo
SCRIPTS_BINS = $(addprefix $(OUT_DIR)/bin/,$(SCRIPTS))

.PHONY: all submodules linux linux-host linux_host linux-guest linux_guest buildroot buildroot-guest buildroot-host nanovmm clean test

all: submodules $(HOST_KERNEL_IMAGE) $(HOST_ROOTFS) $(KERNEL_DISK) $(SCRIPTS_BINS)

submodules:
	@if [ ! -f linux/Makefile ] || [ ! -f buildroot/Makefile ]; then \
		echo "Initializing git submodules..."; \
		git submodule update --init --recursive; \
	fi

linux: linux-guest linux-host
linux_host: linux-host
linux_guest: linux-guest

define build_kernel
	@mkdir -p $(CURDIR)/$(OUT_DIR)/$(1)
	@if [ ! -f $(CURDIR)/$(OUT_DIR)/$(1)/.config ] || [ configs/$(LINUX_ARCH)/$(2) -nt $(CURDIR)/$(OUT_DIR)/$(1)/.config ]; then \
		$(MAKE) -C linux ARCH=$(LINUX_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) O=$(CURDIR)/$(OUT_DIR)/$(1) -s defconfig; \
		(cd linux && scripts/kconfig/merge_config.sh -Q -m -O $(CURDIR)/$(OUT_DIR)/$(1) $(CURDIR)/$(OUT_DIR)/$(1)/.config ../configs/$(LINUX_ARCH)/$(2) >/dev/null); \
		$(MAKE) -C linux ARCH=$(LINUX_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) O=$(CURDIR)/$(OUT_DIR)/$(1) -s olddefconfig; \
	fi
	@$(MAKE) -C linux ARCH=$(LINUX_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) O=$(CURDIR)/$(OUT_DIR)/$(1) -s olddefconfig
	@$(MAKE) -j$(shell nproc) -C linux ARCH=$(LINUX_ARCH) CROSS_COMPILE=$(CROSS_COMPILE) O=$(CURDIR)/$(OUT_DIR)/$(1) -s
endef

define br_host_flags
HOST_CFLAGS="-O2 -I$(CURDIR)/$(OUT_DIR)/$(1)/host/include -Wno-implicit-function-declaration" HOST_CXXFLAGS="-O2 -I$(CURDIR)/$(OUT_DIR)/$(1)/host/include -Wno-implicit-function-declaration"
endef

define build_buildroot
	@mkdir -p $(CURDIR)/$(OUT_DIR)/$(1)
	@cp $(2) $(CURDIR)/$(OUT_DIR)/$(1)/$(notdir $(2))
	@echo 'BR2_ROOTFS_OVERLAY="$(3)"' >> $(CURDIR)/$(OUT_DIR)/$(1)/$(notdir $(2))
	@if [ ! -f $(CURDIR)/$(OUT_DIR)/$(1)/.config ]; then \
		$(MAKE) -C buildroot O=$(CURDIR)/$(OUT_DIR)/$(1) -s BR2_DEFCONFIG=$(CURDIR)/$(OUT_DIR)/$(1)/$(notdir $(2)) defconfig; \
	else \
		sed -i '/BR2_ROOTFS_OVERLAY=/d' $(CURDIR)/$(OUT_DIR)/$(1)/.config; \
		echo 'BR2_ROOTFS_OVERLAY="$(3)"' >> $(CURDIR)/$(OUT_DIR)/$(1)/.config; \
	fi
	@$(MAKE) -C buildroot O=$(CURDIR)/$(OUT_DIR)/$(1) $(call br_host_flags,$(1)) -s
	@if [ -f $(CURDIR)/$(OUT_DIR)/$(1)/images/rootfs.cpio ]; then \
		PATH="$(CURDIR)/$(OUT_DIR)/$(1)/host/bin:$$PATH" lz4 -l -9 -f $(CURDIR)/$(OUT_DIR)/$(1)/images/rootfs.cpio $(CURDIR)/$(4); \
	fi
	@touch -c $(4)
endef

linux-host: submodules $(HOST_KERNEL_IMAGE)

linux-guest: submodules $(GUEST_KERNEL_IMAGE)

$(HOST_KERNEL_IMAGE): submodules configs/$(LINUX_ARCH)/linux_host_config_fragment .FORCE
	@$(call build_kernel,linux_host,linux_host_config_fragment,$@)

$(GUEST_KERNEL_IMAGE): submodules configs/$(LINUX_ARCH)/linux_guest_config_fragment $(GUEST_ROOTFS) .FORCE
	@$(call build_kernel,linux_guest,linux_guest_config_fragment,$@)

.FORCE:
.PHONY: .FORCE

$(OUT_DIR)/headers/.installed: submodules
	@mkdir -p $(CURDIR)/$(OUT_DIR)/headers $(dir $@)
	@$(MAKE) --no-print-directory -C linux ARCH=$(LINUX_ARCH) O=$(CURDIR)/$(OUT_DIR)/linux_guest headers_install INSTALL_HDR_PATH=$(CURDIR)/$(OUT_DIR)/headers
	@touch $@

$(OUT_DIR)/bin/nvmm: $(OUT_DIR)/headers/.installed $(wildcard nanovmm/*.c nanovmm/*.h)
	@mkdir -p $(CURDIR)/$(OUT_DIR)/bin
	@$(MAKE) --no-print-directory -C nanovmm ARCH=$(LINUX_ARCH) CC="$(CROSS_COMPILE)gcc" OUT_DIR=$(CURDIR)/$(OUT_DIR)/bin

$(OUT_DIR)/bin/ovm_agent $(OUT_DIR)/bin/ovm_monitor $(OUT_DIR)/bin/ovm_ipi_test &: $(wildcard tools/*.c tools/*.h)
	@mkdir -p $(CURDIR)/$(OUT_DIR)/bin
	@$(MAKE) --no-print-directory -C tools ARCH=$(LINUX_ARCH) CC="$(CROSS_COMPILE)gcc" OUT_DIR=$(CURDIR)/$(OUT_DIR)/bin

$(OUT_DIR)/bin/%: scripts/%
	@mkdir -p $(CURDIR)/$(OUT_DIR)/bin
	@install -m 0755 $< $@

tools: submodules $(OUT_DIR)/bin/ovm_agent $(OUT_DIR)/bin/ovm_monitor $(OUT_DIR)/bin/ovm_ipi_test
nanovmm: submodules $(OUT_DIR)/bin/nvmm $(OUT_DIR)/bin/ovm_agent $(OUT_DIR)/bin/ovm_monitor $(OUT_DIR)/bin/ovm_ipi_test $(SCRIPTS_BINS)

buildroot: buildroot-guest buildroot-host

buildroot-guest: submodules $(GUEST_ROOTFS)

$(GUEST_ROOTFS): $(OUT_DIR)/bin/ovm_agent $(OUT_DIR)/bin/ovm_ipi_test $(GUEST_CONFIG_FRAG) $(GUEST_OVERLAY_SRCS)
	@mkdir -p $(CURDIR)/$(OUT_DIR)/guest_overlay/usr/bin
	@install -D -m 0755 $(OUT_DIR)/bin/ovm_agent $(CURDIR)/$(OUT_DIR)/guest_overlay/usr/bin/ovm_agent
	@install -D -m 0755 $(OUT_DIR)/bin/ovm_ipi_test $(CURDIR)/$(OUT_DIR)/guest_overlay/usr/bin/ovm_ipi_test
	@$(call build_buildroot,buildroot_guest,$(GUEST_CONFIG_FRAG),../overlays/buildroot_guest $(CURDIR)/$(OUT_DIR)/guest_overlay,$@)

buildroot-host: submodules $(HOST_ROOTFS) $(KERNEL_DISK)
kernel-disk: $(KERNEL_DISK)

$(OUT_DIR)/kernel_disk.ext4: $(HOST_KERNEL_IMAGE) $(HOST_ROOTFS)
	@mkdir -p $(dir $@)
	@truncate -s 64M $@
	@mkfs.ext4 -F -q -L boot $@
	@if command -v debugfs >/dev/null 2>&1; then \
		debugfs -w -R "write $(HOST_KERNEL_IMAGE) $(notdir $(HOST_KERNEL_IMAGE))" $@ >/dev/null 2>&1; \
		for img in "$(OUT_DIR)/buildroot_host/images/rootfs.ext4" "$(OUT_DIR)/buildroot_host/images/rootfs.ext2"; do \
			if [ -f "$$img" ]; then \
				debugfs -w -R "rm boot/$(notdir $(HOST_KERNEL_IMAGE))" "$$img" >/dev/null 2>&1 || true; \
				debugfs -w -R "write $(HOST_KERNEL_IMAGE) boot/$(notdir $(HOST_KERNEL_IMAGE))" "$$img" >/dev/null 2>&1 || true; \
			fi; \
		done; \
	fi

$(OUT_DIR)/kernel_disk%.ext4: $(HOST_KERNEL_IMAGE) $(HOST_ROOTFS)
	@mkdir -p $(dir $@)
	@truncate -s 64M $@
	@mkfs.ext4 -F -q -L boot $@
	@if command -v debugfs >/dev/null 2>&1; then \
		debugfs -w -R "write $(HOST_KERNEL_IMAGE) $(notdir $(HOST_KERNEL_IMAGE))" $@ >/dev/null 2>&1; \
	fi

$(HOST_ROOTFS): $(OUT_DIR)/bin/nvmm $(OUT_DIR)/bin/ovm_monitor $(HOST_KERNEL_IMAGE) $(GUEST_KERNEL_IMAGE) $(GUEST_ROOTFS) $(HOST_CONFIG_FRAG) $(HOST_OVERLAY_SRCS) $(HOST_SCRIPTS_SRCS)
	@mkdir -p $(CURDIR)/$(OUT_DIR)/host_overlay/usr/bin $(CURDIR)/$(OUT_DIR)/host_overlay/opt/guest $(CURDIR)/$(OUT_DIR)/host_overlay/boot
	@rm -rf $(CURDIR)/$(OUT_DIR)/buildroot_host/target/boot
	@rm -f $(CURDIR)/$(OUT_DIR)/buildroot_host/target/usr/bin/ovm* $(CURDIR)/$(OUT_DIR)/buildroot_host/target/usr/bin/nvmm
	@install -D -m 0755 $(OUT_DIR)/bin/nvmm $(CURDIR)/$(OUT_DIR)/host_overlay/usr/bin/nvmm
	@install -D -m 0755 $(OUT_DIR)/bin/ovm_monitor $(CURDIR)/$(OUT_DIR)/host_overlay/usr/bin/ovm_monitor
	@install -D -m 0644 $(HOST_KERNEL_IMAGE) $(CURDIR)/$(OUT_DIR)/host_overlay/boot/$(notdir $(HOST_KERNEL_IMAGE))
	@install -D -m 0644 $(GUEST_KERNEL_IMAGE) $(CURDIR)/$(OUT_DIR)/host_overlay/opt/guest/kernel
	@install -D -m 0644 $(GUEST_ROOTFS) $(CURDIR)/$(OUT_DIR)/host_overlay/opt/guest/rootfs.cpio.lz4
	@for script in ovm_guest ovm_guest_qemu ovm_guest_launcher; do \
		install -D -m 0755 scripts/$$script $(CURDIR)/$(OUT_DIR)/host_overlay/usr/bin/$$script; \
	done
	@$(call build_buildroot,buildroot_host,$(HOST_CONFIG_FRAG),../overlays/buildroot_host $(CURDIR)/$(OUT_DIR)/host_overlay,$@)

test:
	@./scripts/ovm_test $(if $(filter command line,$(origin ARCH)),-t $(if $(filter arm64 aarch64,$(LINUX_ARCH)),arm,amd),)

clean:
	@rm -rf output_*
	@$(MAKE) -C nanovmm clean OUT_DIR=.
	@$(MAKE) -C tools clean OUT_DIR=.
