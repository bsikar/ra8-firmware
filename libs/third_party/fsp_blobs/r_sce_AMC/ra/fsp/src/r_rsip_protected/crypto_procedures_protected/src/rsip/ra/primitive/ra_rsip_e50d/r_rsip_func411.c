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

void r_rsip_func411 (const uint32_t ARG1[])
{
    uint32_t iLoop = 0U;

    r_rsip_func100(bswap_32big(0x3d914b24U),
                   bswap_32big(0xae6f621cU),
                   bswap_32big(0x791a6637U),
                   bswap_32big(0x427e17fdU));

    WR1_PROG(REG_1600H, 0x3420a8a0U);
    WR1_PROG(REG_1600H, 0x00000006U);
    WR1_PROG(REG_1600H, 0x2000b4a0U);
    WR1_PROG(REG_1600H, 0x00000005U);

    WR1_PROG(REG_1600H, 0x000034c5U);

    WR1_PROG(REG_1600H, 0x380088c0U);
    WR1_PROG(REG_1600H, 0x00000002U);
    WR1_PROG(REG_1600H, 0x100008c6U);

    WR1_PROG(REG_1600H, 0x380088c0U);
    WR1_PROG(REG_1600H, 0x00000003U);
    WR1_PROG(REG_1600H, 0x1000b4c0U);
    WR1_PROG(REG_1600H, 0x00000001U);

    WR1_PROG(REG_1600H, 0x00000800U);

    WR1_PROG(REG_1600H, 0x0000b420U);
    WR1_PROG(REG_1600H, 0x00000080U);

    WR1_PROG(REG_1608H, 0x80b10000U);
    WR1_PROG(REG_1400H, 0x034000c5U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    WR1_PROG(REG_1600H, 0x00007c06U);
    WR1_PROG(REG_143CH, 0x00600000U);
    WR1_PROG(REG_1458H, 0x00000000U);

    if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000000U)
    {
        WR1_PROG(REG_1600H, 0x0000b560U);
        WR1_PROG(REG_1600H, 0x00000007U);

        WR1_PROG(REG_1600H, 0x0000b580U);
        WR1_PROG(REG_1600H, 0x000000a4U);

        r_rsip_func101(bswap_32big(0x7460c75bU), bswap_32big(0x1258518dU), bswap_32big(0x324d7cf4U),
                       bswap_32big(0x1e5b74aeU));
    }
    else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000001U)
    {
        WR1_PROG(REG_1600H, 0x0000b560U);
        WR1_PROG(REG_1600H, 0x00000008U);

        WR1_PROG(REG_1600H, 0x0000b580U);
        WR1_PROG(REG_1600H, 0x000000a0U);

        r_rsip_func101(bswap_32big(0x6317df35U), bswap_32big(0xcff9f1f8U), bswap_32big(0x2c54476cU),
                       bswap_32big(0x584205cfU));
    }
    else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000004U)
    {
        WR1_PROG(REG_1600H, 0x0000b560U);
        WR1_PROG(REG_1600H, 0x0000000cU);

        WR1_PROG(REG_1600H, 0x0000b580U);
        WR1_PROG(REG_1600H, 0x00000090U);

        r_rsip_func101(bswap_32big(0x1281f3fbU), bswap_32big(0xb37e1668U), bswap_32big(0x8547a55fU),
                       bswap_32big(0x4f7f175eU));
    }
    else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000005U)
    {
        WR1_PROG(REG_1600H, 0x0000b560U);
        WR1_PROG(REG_1600H, 0x00000010U);

        WR1_PROG(REG_1600H, 0x0000b580U);
        WR1_PROG(REG_1600H, 0x00000080U);

        r_rsip_func101(bswap_32big(0x4fb97af4U), bswap_32big(0x704ed688U), bswap_32big(0x4ddab710U),
                       bswap_32big(0xb1f9bce5U));
    }

    WR1_PROG(REG_1600H, 0x0000094aU);

    r_rsip_func100(bswap_32big(0xb5f98613U),
                   bswap_32big(0xcd0b6ee0U),
                   bswap_32big(0x69a5d2acU),
                   bswap_32big(0xefeee76fU));
    WR1_PROG(REG_1608H, 0x81010160U);
    WR1_PROG(REG_1408H, 0x00005006U);
    WAIT_STS(REG_1408H, 30, 1);
    RD1_ADDR(REG_1420H, &S_RAM[0]);
    S_RAM[0] = bswap_32big(S_RAM[0]);

    for (iLoop = 0U; iLoop < S_RAM[0]; iLoop++)
    {
        WR1_PROG(REG_1444H, 0x000000c7U);
        WR1_PROG(REG_1608H, 0x80810000U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, ARG1[iLoop]);
        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1600H, 0x0000a400U);
        WR1_PROG(REG_1600H, 0x00000004U);

        WR1_PROG(REG_1600H, 0x00002d40U);
        r_rsip_func101(bswap_32big(0x5de7b4e7U), bswap_32big(0x9f9cfc36U), bswap_32big(0xe8e9dc27U),
                       bswap_32big(0x543f4245U));
    }

    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1600H, 0x3800094bU);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00260000U);
    WR1_PROG(REG_143CH, 0x00402000U);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1600H, 0x00007c1dU);
    WR1_PROG(REG_143CH, 0x00602000U);
    WR1_PROG(REG_1458H, 0x00000000U);
}
