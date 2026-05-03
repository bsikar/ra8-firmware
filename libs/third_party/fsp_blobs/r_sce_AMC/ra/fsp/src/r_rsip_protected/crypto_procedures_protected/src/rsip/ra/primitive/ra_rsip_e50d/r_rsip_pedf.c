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

rsip_ret_t r_rsip_pedf (const uint32_t InData_SignatureType[],
                        const uint32_t InData_HashType[],
                        const uint32_t InData_MsgDgst[],
                        const uint32_t InData_Salt[],
                        const uint32_t InData_SaltLength[],
                        uint32_t       OutData_EncCertificateInfo[])
{
    WR1_PROG(REG_1444H, 0x000001c7U);
    WR1_PROG(REG_1608H, 0x80020080U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, InData_SignatureType[0]);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, InData_HashType[0]);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x000000edU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x119a90ecU),
                   bswap_32big(0xf48f43d3U),
                   bswap_32big(0x4871d32aU),
                   bswap_32big(0x06dd0ad5U));
    r_rsip_func411(InData_MsgDgst);

    WR1_PROG(REG_1600H, 0x38000c84U);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00260000U);

    r_rsip_func100(bswap_32big(0xf476ba07U),
                   bswap_32big(0xb79247e3U),
                   bswap_32big(0xb0700e22U),
                   bswap_32big(0x9c73f442U));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        WR1_PROG(REG_1404H, 0x17f80000U);
        WR1_PROG(REG_1608H, 0x80900001U);
        WR1_PROG(REG_1400H, 0x03430041U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        WR1_PROG(REG_1600H, 0x00000842U);

        r_rsip_func101(bswap_32big(0x9c2078feU), bswap_32big(0x36121301U), bswap_32big(0xa7f98603U),
                       bswap_32big(0xa74187a3U));
    }
    else
    {
        WR1_PROG(REG_1444H, 0x000000c7U);
        WR1_PROG(REG_1608H, 0x800100e0U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, InData_SaltLength[0]);
        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1444H, 0x000000a7U);
        WR1_PROG(REG_1608H, 0x800103a0U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, bswap_32big(0x000000edU));
        WR1_PROG(REG_1458H, 0x00000000U);

        r_rsip_func101(bswap_32big(0x7b499d4eU), bswap_32big(0xe87e721dU), bswap_32big(0x4e9d922eU),
                       bswap_32big(0x84088891U));
        r_rsip_func412(InData_Salt);

        WR1_PROG(REG_1600H, 0x38008bc0U);
        WR1_PROG(REG_1600H, 0x76bea7b0U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00A60000U);
        r_rsip_func100(bswap_32big(0x21d29c39U), bswap_32big(0xbdddcf99U), bswap_32big(0x9ff56c68U),
                       bswap_32big(0x01fe4173U));
        WR1_PROG(REG_143CH, 0x00400000U);

        if (CHCK_STS(REG_143CH, 22, 1))
        {
            r_rsip_func102(bswap_32big(0xdad8131aU), bswap_32big(0xd1de2085U), bswap_32big(0xe063d185U),
                           bswap_32big(0xec438aceU));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);

            return RSIP_RET_FAIL;
        }
        else
        {
            r_rsip_func101(bswap_32big(0x6ea209beU), bswap_32big(0x15d5fa98U), bswap_32big(0xc9242a93U),
                           bswap_32big(0x9b2e0604U));
        }
    }

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x000000edU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xc3577c62U),
                   bswap_32big(0x854953e1U),
                   bswap_32big(0x7892cb3fU),
                   bswap_32big(0x71a0862bU));
    r_rsip_func413();

    WR1_PROG(REG_1600H, 0x38008be0U);
    WR1_PROG(REG_1600H, 0xa7cd232fU);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00A60000U);
    r_rsip_func100(bswap_32big(0x6c291004U),
                   bswap_32big(0x053d7ed7U),
                   bswap_32big(0xcfa4e109U),
                   bswap_32big(0xf5ff32bdU));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        r_rsip_func102(bswap_32big(0x17836626U), bswap_32big(0xcaf969bdU), bswap_32big(0xfe180470U),
                       bswap_32big(0x1799ae3fU));
        WR1_PROG(REG_14BCH, 0x00000040U);
        WAIT_STS(REG_142CH, 12, 0);

        return RSIP_RET_FAIL;
    }
    else
    {
        WR1_PROG(REG_1444H, 0x000000a7U);
        WR1_PROG(REG_1608H, 0x800103a0U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, bswap_32big(0x000000edU));
        WR1_PROG(REG_1458H, 0x00000000U);

        r_rsip_func101(bswap_32big(0x8e408bc7U), bswap_32big(0x95c9b571U), bswap_32big(0xcbbeefdfU),
                       bswap_32big(0x244c5209U));
        r_rsip_func414(OutData_EncCertificateInfo);

        r_rsip_func102(bswap_32big(0x019717a2U), bswap_32big(0xbf98c470U), bswap_32big(0x2bd50f39U),
                       bswap_32big(0x5aa3a128U));
        WR1_PROG(REG_14BCH, 0x00000040U);
        WAIT_STS(REG_142CH, 12, 0);

        return RSIP_RET_PASS;
    }
}
