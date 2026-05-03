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

rsip_ret_t r_rsip_pefr (const uint32_t InData_HashType[], const uint32_t InData_State[])
{
    uint32_t iLoop = 0U;

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
        WR1_PROG(REG_2004H, 0x00001050U);

        r_rsip_func101(bswap_32big(0x37efd5ddU), bswap_32big(0x37788243U), bswap_32big(0x0037f01eU),
                       bswap_32big(0x050e767fU));
    }
    else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000001U)
    {
        WR1_PROG(REG_2004H, 0x000010a0U);

        r_rsip_func101(bswap_32big(0x079d5e22U), bswap_32big(0xb63ec1b4U), bswap_32big(0x102acff8U),
                       bswap_32big(0x0509a0bcU));
    }
    else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000002U)
    {
        WR1_PROG(REG_2004H, 0x000010b0U);

        r_rsip_func101(bswap_32big(0x39926437U), bswap_32big(0x0d874dc5U), bswap_32big(0x33a9bf3aU),
                       bswap_32big(0xbf225884U));
    }

    WR1_PROG(REG_1600H, 0x0000b420U);
    WR1_PROG(REG_1600H, 0x00000010U);
    WR1_PROG(REG_1600H, 0x00003c01U);

    WR1_PROG(REG_1444H, 0x00000040U);
    WR1_PROG(REG_2014H, InData_State[18]);
    WR1_PROG(REG_1444H, 0x00000040U);
    WR1_PROG(REG_2010H, InData_State[19]);

    for (iLoop = 0U; iLoop < 18U; iLoop++)
    {
        WR1_PROG(REG_1444H, 0x00000040U);
        WR1_PROG(REG_2028H, InData_State[iLoop]);
    }

    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x681ce34dU),
                   bswap_32big(0xb18679bfU),
                   bswap_32big(0x550b1484U),
                   bswap_32big(0x557c0af6U));

    return RSIP_RET_PASS;
}
