/* SPDX-License-Identifier: GPL-3.0-only */
/* Only what third-party code needs from <string.h>; libc.c has the definitions. */
#pragma once
void *memset(void *d, int c, unsigned n);
