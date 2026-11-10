# bfmem -- Linux(VFIO)の試験環境のビルド。
#
#   make            build/libbfmem.a と build/bfmem_test
#   make vxcheck    VxWorks の移植層を、手元の gcc で文法だけ確かめる(VxWorks 固有のヘッダは使わない)
#   make clean
#
# VxWorks では src/*.c・port/vxworks/port_vxworks.c・port/x86/bfm_x86.c・port/x86/bfm_x86_asm.S を
# VxWorks のビルド(DKM のソースとして)に入れ、-Iinclude -Iport/x86 を付ける。
# BFM_PORT_LINUX は定義しないこと。

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -g
WARN    := -std=gnu11 -Wall -Wextra -Wshadow -Wstrict-prototypes
ALL_CFLAGS := $(WARN) -Iinclude -Iport/x86 $(CFLAGS)
BUILD   := build

LIB_SRCS := src/bfm_config.c src/bfm_core.c src/bfm_mem.c src/bfm_pci.c \
            src/mlx5_cmd.c src/mlx5_hca.c src/mlx5_qp.c src/bfm_cm.c src/bfm_ops.c \
            src/bfm_dif.c src/bfm_async.c
X86_SRCS := port/x86/bfm_x86.c
LINUX_SRCS := port/linux_vfio/port_linux.c
LIB_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRCS) $(X86_SRCS) $(LINUX_SRCS)) \
            $(BUILD)/port/x86/bfm_x86_asm.o

all: $(BUILD)/libbfmem.a $(BUILD)/bfmem_test

$(BUILD)/%.o: %.c include/*.h port/x86/*.h src/bfm_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -DBFM_PORT_LINUX -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(BUILD)/libbfmem.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/bfmem_test: $(BUILD)/test/bfm_test.o $(BUILD)/libbfmem.a
	$(CC) $(ALL_CFLAGS) -pthread -o $@ $^

vxcheck:
	@for f in $(LIB_SRCS) $(X86_SRCS) port/vxworks/port_vxworks.c; do \
	    $(CC) $(WARN) -Iinclude -Iport/x86 -fsyntax-only $$f || exit 1; \
	done; \
	$(CC) -U__linux__ -c port/x86/bfm_x86_asm.S -o /dev/null && echo "vxcheck: OK"

clean:
	rm -rf $(BUILD)

.PHONY: all vxcheck clean
