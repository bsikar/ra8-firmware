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

void r_rsip_func413 (void)
{
    uint32_t iLoop = 0U;

    r_rsip_func100(bswap_32big(0xfd3d3f18U),
                   bswap_32big(0x8df77f17U),
                   bswap_32big(0xe919f16aU),
                   bswap_32big(0x3cfb0934U));
    WR1_PROG(REG_1600H, 0x00002d60U);
    WR1_PROG(REG_1600H, 0x0002696bU);

    WR1_PROG(REG_1600H, 0x0000094aU);

    WR1_PROG(REG_1600H, 0x0000b7e0U);
    WR1_PROG(REG_1600H, 0xa7cd232fU);

    WR1_PROG(REG_1608H, 0x81010160U);
    WR1_PROG(REG_1408H, 0x00005006U);
    WAIT_STS(REG_1408H, 30, 1);
    RD1_ADDR(REG_1420H, &S_RAM[0]);
    S_RAM[0] = bswap_32big(S_RAM[0]);

    for (iLoop = 0U; iLoop < S_RAM[0]; iLoop++)
    {
        WR1_PROG(REG_1824H, 0x08000045U);
        WR1_PROG(REG_1608H, 0x81840002U);
        WR1_PROG(REG_1400H, 0x00490011U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        WR1_PROG(REG_1608H, 0x8184000cU);
        WR1_PROG(REG_1824H, 0x9c000005U);
        WR1_PROG(REG_1400H, 0x00490011U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        r_rsip_func100(bswap_32big(0x0005ebc0U), bswap_32big(0xc5c3958eU), bswap_32big(0x25e95314U),
                       bswap_32big(0x110b4716U));
        WR1_PROG(REG_143CH, 0x00400000U);

        if (CHCK_STS(REG_143CH, 22, 1))
        {
            WR1_PROG(REG_1600H, 0x00000bffU);

            r_rsip_func101(bswap_32big(0xf4ffa9f2U), bswap_32big(0xd1efa523U), bswap_32big(0xd0923962U),
                           bswap_32big(0x1582c03bU));
        }
        else
        {
            r_rsip_func101(bswap_32big(0x31d8d996U), bswap_32big(0xa8d3178cU), bswap_32big(0xbd90e088U),
                           bswap_32big(0x6e3cabfcU));
        }

        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1600H, 0x0000a440U);
        WR1_PROG(REG_1600H, 0x00000010U);
        WR1_PROG(REG_1600H, 0x0000a580U);
        WR1_PROG(REG_1600H, 0x00000010U);

        WR1_PROG(REG_1600H, 0x00002d40U);

        r_rsip_func101(bswap_32big(0x70ad61f9U), bswap_32big(0x98bb70bcU), bswap_32big(0x54a3f725U),
                       bswap_32big(0x5efcf094U));
    }

    WR1_PROG(REG_1600H, 0x3800094bU);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00260000U);
    WR1_PROG(REG_143CH, 0x00402000U);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1600H, 0x00007c1dU);
    WR1_PROG(REG_143CH, 0x00602000U);
    WR1_PROG(REG_1458H, 0x00000000U);
}
