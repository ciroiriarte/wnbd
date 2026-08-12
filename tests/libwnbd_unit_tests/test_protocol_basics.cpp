/*
 * Copyright (c) 2026 Ciro Iriarte
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "pch.h"

TEST(NbdProtocolUnit, WireStructuresRemainPacked)
{
    EXPECT_EQ(28u, sizeof(NBD_REQUEST));
    EXPECT_EQ(16u, sizeof(NBD_REPLY));
    EXPECT_EQ(16u, sizeof(NBD_HANDSHAKE_REQ));
    EXPECT_EQ(20u, sizeof(NBD_HANDSHAKE_RPL));

    EXPECT_EQ(0u, offsetof(NBD_REQUEST, Magic));
    EXPECT_EQ(4u, offsetof(NBD_REQUEST, Type));
    EXPECT_EQ(8u, offsetof(NBD_REQUEST, Handle));
    EXPECT_EQ(16u, offsetof(NBD_REQUEST, From));
    EXPECT_EQ(24u, offsetof(NBD_REQUEST, Length));
}

TEST(NbdProtocolUnit, RequestTypeNamesAreStable)
{
    EXPECT_STREQ("NBD_CMD_READ", NbdRequestTypeStr(NBD_CMD_READ));
    EXPECT_STREQ("NBD_CMD_WRITE", NbdRequestTypeStr(NBD_CMD_WRITE));
    EXPECT_STREQ("NBD_CMD_DISC", NbdRequestTypeStr(NBD_CMD_DISC));
    EXPECT_STREQ("NBD_CMD_FLUSH", NbdRequestTypeStr(NBD_CMD_FLUSH));
    EXPECT_STREQ("NBD_CMD_TRIM", NbdRequestTypeStr(NBD_CMD_TRIM));
    EXPECT_STREQ("UNKNOWN", NbdRequestTypeStr((NbdRequestType) 0xffff));
}

TEST(NbdProtocolUnit, TransmissionFlagHelpersRequireHasFlags)
{
    EXPECT_FALSE(CHECK_NBD_READONLY(0));
    EXPECT_FALSE(CHECK_NBD_SEND_FUA(NBD_FLAG_SEND_FUA));
    EXPECT_FALSE(CHECK_NBD_SEND_TRIM(NBD_FLAG_SEND_TRIM));
    EXPECT_FALSE(CHECK_NBD_SEND_FLUSH(NBD_FLAG_SEND_FLUSH));

    const UINT16 allSupported = NBD_FLAG_HAS_FLAGS |
        NBD_FLAG_READ_ONLY |
        NBD_FLAG_SEND_FUA |
        NBD_FLAG_SEND_TRIM |
        NBD_FLAG_SEND_FLUSH;

    EXPECT_TRUE(CHECK_NBD_READONLY(allSupported));
    EXPECT_TRUE(CHECK_NBD_SEND_FUA(allSupported));
    EXPECT_TRUE(CHECK_NBD_SEND_TRIM(allSupported));
    EXPECT_TRUE(CHECK_NBD_SEND_FLUSH(allSupported));
}
