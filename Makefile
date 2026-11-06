# bfmem -- Linux(VFIO)の試験環境のビルド。
#
#   make            build/libbfmem.a と build/bfmem_test
#   make vxcheck    VxWorks の移植層を、手元の gcc と仮のヘッダ(tools/vxstub)で文法だけ確かめる
#   make clean
#
# VxWorks では src/*.c と port/vxworks/port_vxworks.c を VxWorks のビルド(DKM / VSB の
# ソースとして)に入れる。BFM_PORT_LINUX は定義しないこと。

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -g
ALL_CFLAGS := -std=gnu11 -Wall -Wextra -Wshadow -Wstrict-prototypes -Iinclude $(CFLAGS)
BUILD   := build

LIB_SRCS := src/bfm_config.c src/bfm_core.c src/bfm_mem.c src/bfm_pci.c \
            src/mlx5_cmd.c src/mlx5_hca.c src/mlx5_qp.c src/bfm_cm.c src/bfm_ops.c
LINUX_SRCS := port/linux_vfio/port_linux.c
LIB_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRCS) $(LINUX_SRCS))

all: $(BUILD)/libbfmem.a $(BUILD)/bfmem_test

$(BUILD)/%.o: %.c include/*.h src/bfm_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -DBFM_PORT_LINUX -c $< -o $@

$(BUILD)/libbfmem.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/bfmem_test: $(BUILD)/test/bfm_test.o $(BUILD)/libbfmem.a
	$(CC) $(ALL_CFLAGS) -o $@ $^

vxcheck:
	@for f in $(LIB_SRCS) port/vxworks/port_vxworks.c; do \
	    $(CC) -std=gnu11 -Wall -Wextra -Iinclude -Itools/vxstub -fsyntax-only $$f || exit 1; \
	done; echo "vxcheck: OK"

clean:
	rm -rf $(BUILD)

.PHONY: all vxcheck clean
