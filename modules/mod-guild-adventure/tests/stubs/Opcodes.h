// Test stub of AzerothCore's Opcodes.h: the two opcodes of core patch CP-5.
#ifndef GA_TEST_STUB_OPCODES_H
#define GA_TEST_STUB_OPCODES_H

#include "Define.h"

enum Opcodes : uint16
{
    CMSG_GA_CLIENT_MESSAGE = 0x521,
    SMSG_GA_SERVER_MESSAGE = 0x522
};

#endif
