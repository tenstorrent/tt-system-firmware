# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

# Makefile for tooling scripts that need direct TLB access.

OUTDIR ?= .
TOOLS = $(addprefix $(OUTDIR)/, tt-fw-terminal tt-tracing)

.PHONY: all clean $(notdir $(TOOLS))

all: $(notdir $(TOOLS))

CFLAGS += -O0 -g -static -Wall -Wextra -Werror -std=gnu11 -I ../../include

tt-fw-terminal: console.c vuart.c rescan.c
	$(CC) $(CFLAGS) -o $(OUTDIR)/$@ $^

tt-tracing: tracing.c vuart.c
	$(CC) $(CFLAGS) -o $(OUTDIR)/$@ $^

clean:
	rm -f $(TOOLS)
