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

rsip_ret_t r_rsip_pdff (const uint32_t InData_SignatureType[],
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
    WR1_PROG(REG_1420H, bswap_32big(0x000000dfU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x48091e57U),
                   bswap_32big(0xfcee2fbeU),
                   bswap_32big(0x9136b80eU),
                   bswap_32big(0xecceff77U));
    r_rsip_func411(InData_MsgDgst);

    WR1_PROG(REG_1600H, 0x38000c84U);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00260000U);

    r_rsip_func100(bswap_32big(0x3393cb12U),
                   bswap_32big(0xfdb4c735U),
                   bswap_32big(0x75ea0a35U),
                   bswap_32big(0xf4ba8b1aU));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        WR1_PROG(REG_1404H, 0x17f80000U);
        WR1_PROG(REG_1608H, 0x80900001U);
        WR1_PROG(REG_1400H, 0x03430041U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        WR1_PROG(REG_1600H, 0x00000842U);

        r_rsip_func101(bswap_32big(0x7029b128U), bswap_32big(0xc7d024ecU), bswap_32big(0x12bd214aU),
                       bswap_32big(0x04ac9d5aU));
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
        WR1_PROG(REG_1420H, bswap_32big(0x000000dfU));
        WR1_PROG(REG_1458H, 0x00000000U);

        r_rsip_func101(bswap_32big(0x3a34ae5cU), bswap_32big(0x09260276U), bswap_32big(0xa2ef34a9U),
                       bswap_32big(0xd779b984U));
        r_rsip_func412(InData_Salt);

        WR1_PROG(REG_1600H, 0x38008bc0U);
        WR1_PROG(REG_1600H, 0x76bea7b0U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00A60000U);
        r_rsip_func100(bswap_32big(0xad806a3bU), bswap_32big(0x5cdbe7bcU), bswap_32big(0xaa2a3b53U),
                       bswap_32big(0xf665b3adU));
        WR1_PROG(REG_143CH, 0x00400000U);

        if (CHCK_STS(REG_143CH, 22, 1))
        {
            r_rsip_func102(bswap_32big(0x6539c8e6U), bswap_32big(0x568c99f3U), bswap_32big(0xd72b9450U),
                           bswap_32big(0x9f564166U));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);

            return RSIP_RET_FAIL;
        }
        else
        {
            r_rsip_func101(bswap_32big(0xc2d8d45fU), bswap_32big(0x3b1f7c24U), bswap_32big(0x963492ecU),
                           bswap_32big(0x8de58b05U));
        }
    }

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x000000dfU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xab3ab7d4U),
                   bswap_32big(0x781b94e9U),
                   bswap_32big(0x4afcea55U),
                   bswap_32big(0x1609f609U));
    r_rsip_func413();

    WR1_PROG(REG_1600H, 0x38008be0U);
    WR1_PROG(REG_1600H, 0xa7cd232fU);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00A60000U);
    r_rsip_func100(bswap_32big(0x8b276b40U),
                   bswap_32big(0x58fbbba4U),
                   bswap_32big(0xeaf25734U),
                   bswap_32big(0xf5fe96b6U));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        r_rsip_func102(bswap_32big(0xdd4ac8d9U), bswap_32big(0x7bae02ffU), bswap_32big(0x6fd89237U),
                       bswap_32big(0x3b135667U));
        WR1_PROG(REG_14BCH, 0x00000040U);
        WAIT_STS(REG_142CH, 12, 0);

        return RSIP_RET_FAIL;
    }
    else
    {
        WR1_PROG(REG_1444H, 0x000000a7U);
        WR1_PROG(REG_1608H, 0x800103a0U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, bswap_32big(0x000000dfU));
        WR1_PROG(REG_1458H, 0x00000000U);

        r_rsip_func101(bswap_32big(0x28da9d6cU), bswap_32big(0x0c771456U), bswap_32big(0xd1b030b1U),
                       bswap_32big(0x70f7eb30U));
        r_rsip_func414(OutData_EncCertificateInfo);

        r_rsip_func102(bswap_32big(0xa2eab718U), bswap_32big(0xb8f557f3U), bswap_32big(0x473fad4aU),
                       bswap_32big(0x50f5f19aU));
        WR1_PROG(REG_14BCH, 0x00000040U);
        WAIT_STS(REG_142CH, 12, 0);

        return RSIP_RET_PASS;
    }
}
