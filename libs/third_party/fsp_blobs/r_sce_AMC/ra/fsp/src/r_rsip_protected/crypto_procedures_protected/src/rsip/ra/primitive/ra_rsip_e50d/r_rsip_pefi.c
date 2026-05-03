/*
* Copyright (c) 2020 - 2026 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "r_rsip_primitive.h"
#include "r_rsip_reg.h"
#include "r_rsip_util.h"

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

rsip_ret_t r_rsip_pefi (const uint32_t InData_HashType[], const uint32_t InData_MsgLen[])
{
    if (RD1_MASK(REG_14BCH, 0x0000001fU) != 0)
    {
        return RSIP_RET_RESOURCE_CONFLICT;
    }

    WR1_PROG(REG_1B00H, 0x00ef0001U);
    WR1_PROG(REG_144CH, 0x00000000U);

    WR1_PROG(REG_2000H, 0x00000001U);

    WR1_PROG(REG_1444H, 0x000000c7U);
    WR1_PROG(REG_1608H, 0x80010000U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, InData_HashType[0]);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1600H, 0x3000a800U);
    WR1_PROG(REG_1600H, 0x00000003U);
    WR1_PROG(REG_1600H, 0x00010020U);
    WR1_PROG(REG_1600H, 0x0000b400U);
    WR1_PROG(REG_1600H, 0x00000002U);
    WR1_PROG(REG_1600H, 0x00000080U);

    r_rsip_func100(bswap_32big(0x8e6ca25bU),
                   bswap_32big(0x3c27e3c7U),
                   bswap_32big(0x4e64c67bU),
                   bswap_32big(0x2f1c8ad3U));

    WR1_PROG(REG_1600H, 0x00007c00U);
    WR1_PROG(REG_143CH, 0x00600000U);
    WR1_PROG(REG_1458H, 0x00000000U);

    if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000000U)
    {
        WR1_PROG(REG_2004H, 0x00000050U);

        r_rsip_func101(bswap_32big(0xb6ac4c59U), bswap_32big(0xbae18f67U), bswap_32big(0x90109e3aU),
                       bswap_32big(0x6caee503U));
    }
    else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000001U)
    {
        WR1_PROG(REG_2004H, 0x000000a0U);

        r_rsip_func101(bswap_32big(0xc86161ebU), bswap_32big(0x72079603U), bswap_32big(0x915386d6U),
                       bswap_32big(0x7d9ec47aU));
    }
    else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000002U)
    {
        WR1_PROG(REG_2004H, 0x000000b0U);

        r_rsip_func101(bswap_32big(0x22aa8823U), bswap_32big(0x92a28bcfU), bswap_32big(0x32c93839U),
                       bswap_32big(0xaf3b9f17U));
    }

    WR1_PROG(REG_1600H, 0x0000b420U);
    WR1_PROG(REG_1600H, 0x00000010U);
    WR1_PROG(REG_1600H, 0x00003c01U);

    if ((InData_MsgLen[0] == 0) && (InData_MsgLen[1] == 0))
    {
        WR1_PROG(REG_200CH, 0x00000100U);

        r_rsip_func101(bswap_32big(0x9d4ee5a3U), bswap_32big(0x2574e687U), bswap_32big(0x7eaa84f1U),
                       bswap_32big(0x9bc9143eU));
    }
    else
    {
        WR1_PROG(REG_1444H, 0x00000040U);
        WR1_PROG(REG_2014H, InData_MsgLen[0]);
        WR1_PROG(REG_1444H, 0x00000040U);
        WR1_PROG(REG_2010H, InData_MsgLen[1]);

        r_rsip_func101(bswap_32big(0x87ecb52dU), bswap_32big(0x8ecb28a3U), bswap_32big(0x426d820aU),
                       bswap_32big(0xbd5801fcU));
    }

    return RSIP_RET_PASS;
}
