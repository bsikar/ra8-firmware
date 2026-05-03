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

void r_rsip_func412 (const uint32_t ARG1[])
{
    uint32_t iLoop = 0U;

    WR1_PROG(REG_1600H, 0x00003527U);

    WR1_PROG(REG_1600H, 0x000035abU);
    WR1_PROG(REG_1600H, 0x00026dadU);

    WR1_PROG(REG_1600H, 0x0000252dU);
    WR1_PROG(REG_1600H, 0x0000a520U);
    WR1_PROG(REG_1600H, 0x00000002U);

    WR1_PROG(REG_1600H, 0x3420a920U);
    WR1_PROG(REG_1600H, 0x00000101U);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00A60000U);

    r_rsip_func100(bswap_32big(0x49b35c1fU),
                   bswap_32big(0x5cba1ceeU),
                   bswap_32big(0x4ebc4a2fU),
                   bswap_32big(0x6b0fc75aU));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        WR1_PROG(REG_1600H, 0x00000bdeU);

        r_rsip_func101(bswap_32big(0x42df8067U), bswap_32big(0x6d925ba4U), bswap_32big(0xcd2675e8U),
                       bswap_32big(0xd50dcb87U));
    }
    else
    {
        r_rsip_func100(bswap_32big(0xf64b3322U), bswap_32big(0x3bb05adaU), bswap_32big(0x671b252cU),
                       bswap_32big(0x3e41cfaaU));
        WR1_PROG(REG_1404H, 0x17f00000U);
        WR1_PROG(REG_1400H, 0x1003001dU);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);
        WR1_PROG(REG_1608H, 0x80900001U);
        WR1_PROG(REG_1400H, 0x13430101U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);
        WR1_PROG(REG_1400H, 0x10030005U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        WR1_PROG(REG_1600H, 0x0000b440U);
        WR1_PROG(REG_1600H, 0x00000040U);

        WR1_PROG(REG_2000H, 0x00000001U);

        WR1_PROG(REG_1600H, 0x00007c05U);
        WR1_PROG(REG_143CH, 0x00600000U);
        WR1_PROG(REG_1458H, 0x00000000U);

        if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000000U)
        {
            WR1_PROG(REG_2004H, 0x00000040U);

            r_rsip_func101(bswap_32big(0x4194bb25U), bswap_32big(0xec617981U), bswap_32big(0x1ca437f4U),
                           bswap_32big(0xf577fe95U));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000001U)
        {
            WR1_PROG(REG_2004H, 0x00000050U);

            r_rsip_func101(bswap_32big(0x8ccd031cU), bswap_32big(0x327c0a97U), bswap_32big(0xcc56b935U),
                           bswap_32big(0x6f7e854dU));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000002U)
        {
            WR1_PROG(REG_2004H, 0x00000080U);

            r_rsip_func101(bswap_32big(0x6a39f13cU), bswap_32big(0xce3a67e6U), bswap_32big(0xc29687f1U),
                           bswap_32big(0x0aebe001U));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000003U)
        {
            WR1_PROG(REG_2004H, 0x00000090U);

            r_rsip_func101(bswap_32big(0x8ba8fc71U), bswap_32big(0x73c68ba6U), bswap_32big(0xf158ee9fU),
                           bswap_32big(0xb9b31ec1U));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000004U)
        {
            WR1_PROG(REG_2004H, 0x000000a0U);

            r_rsip_func101(bswap_32big(0x62107dbcU), bswap_32big(0xec9f7d4dU), bswap_32big(0x60c6583cU),
                           bswap_32big(0x680ee515U));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000005U)
        {
            WR1_PROG(REG_2004H, 0x000000b0U);

            r_rsip_func101(bswap_32big(0x088cf7e7U), bswap_32big(0x54b196c4U), bswap_32big(0x702d8402U),
                           bswap_32big(0x036d77eeU));
        }

        r_rsip_func100(bswap_32big(0x4f64ba93U), bswap_32big(0xee1a0cd7U), bswap_32big(0xe48fc452U),
                       bswap_32big(0xdfe1010eU));
        WR1_PROG(REG_1600H, 0x0000a520U);
        WR1_PROG(REG_1600H, 0x00000006U);

        WR1_PROG(REG_1600H, 0x00000908U);
        WR1_PROG(REG_1600H, 0x01836d09U);
        WR1_PROG(REG_1600H, 0x00036d29U);

        WR1_PROG(REG_1608H, 0x81020100U);
        WR1_PROG(REG_1408H, 0x0000500aU);
        WAIT_STS(REG_1408H, 30, 1);
        RD1_ADDR(REG_1420H, &S_RAM[0]);
        S_RAM[0] = bswap_32big(S_RAM[0]);
        WAIT_STS(REG_1408H, 30, 1);
        RD1_ADDR(REG_1420H, &S_RAM[0 + 1]);
        S_RAM[0 + 1] = bswap_32big(S_RAM[0 + 1]);

        r_rsip_func100(bswap_32big(0x5cd11614U), bswap_32big(0x086717a9U), bswap_32big(0x25382cfcU),
                       bswap_32big(0xef898493U));
        WR1_PROG(REG_1444H, 0x00000040U);
        WR1_PROG(REG_2014H, S_RAM[0]);
        WR1_PROG(REG_1444H, 0x00000040U);
        WR1_PROG(REG_2010H, S_RAM[0 + 1]);

        WR1_PROG(REG_1400H, 0x01400009U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        WR1_PROG(REG_1600H, 0x00000800U);

        WR1_PROG(REG_1600H, 0x0000094aU);

        WR1_PROG(REG_1608H, 0x81010160U);
        WR1_PROG(REG_1408H, 0x00005006U);
        WAIT_STS(REG_1408H, 30, 1);
        RD1_ADDR(REG_1420H, &S_RAM[0]);
        S_RAM[0] = bswap_32big(S_RAM[0]);

        for (iLoop = 0U; iLoop < S_RAM[0]; iLoop++)
        {
            WR1_PROG(REG_1608H, 0x81810000U);
            WR1_PROG(REG_1400H, 0x01490005U);
            WAIT_STS(REG_1404H, 30, 0);
            WR1_PROG(REG_143CH, 0x00001800U);

            WR1_PROG(REG_1600H, 0x0000a400U);
            WR1_PROG(REG_1600H, 0x00000004U);

            WR1_PROG(REG_1600H, 0x00002d40U);

            r_rsip_func101(bswap_32big(0xb5b3aeb2U), bswap_32big(0x7e888c23U), bswap_32big(0x8c72b6cdU),
                           bswap_32big(0x36610b58U));
        }

        WR1_PROG(REG_1458H, 0x00000000U);

        r_rsip_func100(bswap_32big(0x8a5fb0ecU), bswap_32big(0x395d42c3U), bswap_32big(0xd1808866U),
                       bswap_32big(0x361a4dbfU));
        WR1_PROG(REG_1600H, 0x3800094bU);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00260000U);
        WR1_PROG(REG_143CH, 0x00402000U);
        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1600H, 0x0000a4e0U);
        WR1_PROG(REG_1600H, 0x00000003U);
        WR1_PROG(REG_1600H, 0x000268e7U);

        WR1_PROG(REG_1600H, 0x0000094aU);

        WR1_PROG(REG_1608H, 0x810100e0U);
        WR1_PROG(REG_1408H, 0x00005006U);
        WAIT_STS(REG_1408H, 30, 1);
        RD1_ADDR(REG_1420H, &S_RAM[0 + 1]);
        S_RAM[0 + 1] = bswap_32big(S_RAM[0 + 1]);

        for (iLoop = 0U; iLoop < S_RAM[0 + 1]; iLoop++)
        {
            WR1_PROG(REG_1444H, 0x000000c4U);
            WAIT_STS(REG_1444H, 31, 1);
            WR1_PROG(REG_1420H, ARG1[iLoop]);

            WR1_PROG(REG_1600H, 0x00002d40U);

            r_rsip_func101(bswap_32big(0x21efd131U), bswap_32big(0x9d92195fU), bswap_32big(0x5e44ba19U),
                           bswap_32big(0xc539849cU));
        }

        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1600H, 0x38000947U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00260000U);
        WR1_PROG(REG_143CH, 0x00402000U);
        WR1_PROG(REG_1458H, 0x00000000U);

        WAIT_STS(REG_2030H, 8, 0);
        WR1_PROG(REG_143CH, 0x00001600U);

        WAIT_STS(REG_2030H, 4, 1);

        WR1_PROG(REG_1600H, 0x0000094aU);

        for (iLoop = 0U; iLoop < S_RAM[0]; iLoop++)
        {
            WR1_PROG(REG_1608H, 0x80810002U);
            WR1_PROG(REG_1400H, 0x03450005U);

            WAIT_STS(REG_1404H, 30, 0);
            WR1_PROG(REG_143CH, 0x00001800U);

            WR1_PROG(REG_1600H, 0x0000a440U);
            WR1_PROG(REG_1600H, 0x00000004U);

            WR1_PROG(REG_1600H, 0x00002d40U);

            r_rsip_func101(bswap_32big(0x28742ea9U), bswap_32big(0x303ad91aU), bswap_32big(0x9e8d4dbfU),
                           bswap_32big(0x95a3e0c7U));
        }

        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1600H, 0x3800094bU);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00260000U);
        WR1_PROG(REG_143CH, 0x00402000U);
        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1600H, 0x0000b440U);
        WR1_PROG(REG_1600H, 0x00000040U);

        WR1_PROG(REG_1600H, 0x0000b7c0U);
        WR1_PROG(REG_1600H, 0x76bea7b0U);

        r_rsip_func101(bswap_32big(0xc069eea6U), bswap_32big(0x4bf33da8U), bswap_32big(0xa8582d13U),
                       bswap_32big(0x16856b74U));
    }

    WR1_PROG(REG_1600H, 0x00007c1dU);
    WR1_PROG(REG_143CH, 0x00602000U);
    WR1_PROG(REG_1458H, 0x00000000U);
}
