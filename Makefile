# VitalsGuard-RT - build for QNX 8.0 (x86_64 QEMU target)
#
#   source ~/qnxinstall/qnx800/qnxsdp-env.sh
#   make            # builds vitalsguard for QNX x86_64
#   make clean

QNX_TARGET_ARCH ?= x86_64
CC      = qcc -Vgcc_nto$(QNX_TARGET_ARCH)
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -D_QNX_SOURCE
LDFLAGS = -lm -lsocket

SRC = src/main.c src/vitalsd.c src/analyzer.c src/alarmd.c src/httpd.c src/loadhog.c
BIN = vitalsguard

all: $(BIN)

$(BIN): $(SRC) src/vitals.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
