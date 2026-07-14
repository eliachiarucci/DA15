// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/* SEGGER RTT stub for host-side unit tests: logging is a no-op. */

#ifndef SEGGER_RTT_STUB_H
#define SEGGER_RTT_STUB_H

static inline int SEGGER_RTT_printf(unsigned buffer_index, const char *fmt,
                                    ...) {
    (void)buffer_index;
    (void)fmt;
    return 0;
}

#endif // SEGGER_RTT_STUB_H
