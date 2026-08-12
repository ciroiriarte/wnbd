/*
 * Copyright (c) 2026 Ciro Iriarte
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#pragma once

#include "gtest/gtest.h"

// GoogleTest 1.8.x does not have GTEST_SKIP(). Keep one compatibility point so
// dependency-gated tests become real skips as soon as the package is upgraded.
// On older packages, fail loudly instead of reporting a false pass.
#ifdef GTEST_SKIP
#define WNBD_GTEST_SKIP(message) GTEST_SKIP() << message
#else
#define WNBD_GTEST_SKIP(message)                                                \
    do {                                                                        \
        ADD_FAILURE() << "GoogleTest lacks GTEST_SKIP(); missing test "         \
                      << "prerequisite: " << message;                          \
        return;                                                                 \
    } while (0)
#endif
