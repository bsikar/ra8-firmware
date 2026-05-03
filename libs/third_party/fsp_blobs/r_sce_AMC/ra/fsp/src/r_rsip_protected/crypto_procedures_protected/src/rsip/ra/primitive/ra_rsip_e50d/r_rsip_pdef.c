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

rsip_ret_t r_rsip_pdef (const uint32_t InData_SignatureType[],
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
    WR1_PROG(REG_1420H, bswap_32big(0x000000deU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x51501677U),
                   bswap_32big(0x788eb155U),
                   bswap_32big(0x4fafaa48U),
                   bswap_32big(0xf2e0412eU));
    r_rsip_func411(InData_MsgDgst);

    WR1_PROG(REG_1600H, 0x38000c84U);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00260000U);

    r_rsip_func100(bswap_32big(0x7c257e73U),
                   bswap_32big(0x83d9f88cU),
                   bswap_32big(0x5b476ba1U),
                   bswap_32big(0x1bef4b48U));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        WR1_PROG(REG_1404H, 0x17f80000U);
        WR1_PROG(REG_1608H, 0x80900001U);
        WR1_PROG(REG_1400H, 0x03430041U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        WR1_PROG(REG_1600H, 0x00000842U);

        r_rsip_func101(bswap_32big(0x1367afd4U), bswap_32big(0xb70f41c6U), bswap_32big(0x0a10027fU),
                       bswap_32big(0x513ae345U));
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
        WR1_PROG(REG_1420H, bswap_32big(0x000000deU));
        WR1_PROG(REG_1458H, 0x00000000U);

        r_rsip_func101(bswap_32big(0x766f0198U), bswap_32big(0xe29b7d5aU), bswap_32big(0xcbe0ff70U),
                       bswap_32big(0xb829c445U));
        r_rsip_func412(InData_Salt);

        WR1_PROG(REG_1600H, 0x38008bc0U);
        WR1_PROG(REG_1600H, 0x76bea7b0U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00A60000U);
        r_rsip_func100(bswap_32big(0xe3fad6c4U), bswap_32big(0x60d28969U), bswap_32big(0xe680f67cU),
                       bswap_32big(0x1a0c7474U));
        WR1_PROG(REG_143CH, 0x00400000U);

        if (CHCK_STS(REG_143CH, 22, 1))
        {
            r_rsip_func102(bswap_32big(0x38b6f92fU), bswap_32big(0xe78aaa48U), bswap_32big(0xd680122fU),
                           bswap_32big(0x1568e958U));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);

            return RSIP_RET_FAIL;
        }
        else
        {
            r_rsip_func101(bswap_32big(0xc50d6f41U), bswap_32big(0x00258b12U), bswap_32big(0x0746a245U),
                           bswap_32big(0x69b107c3U));
        }
    }

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x000000deU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x7dba4a96U),
                   bswap_32big(0x9b1babf5U),
                   bswap_32big(0x97b319f9U),
                   bswap_32big(0x9e196780U));
    r_rsip_func413();

    WR1_PROG(REG_1600H, 0x38008be0U);
    WR1_PROG(REG_1600H, 0xa7cd232fU);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00A60000U);
    r_rsip_func100(bswap_32big(0xefb77471U),
                   bswap_32big(0x34f7e1bfU),
                   bswap_32big(0x022cb347U),
                   bswap_32big(0x01c0df09U));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        r_rsip_func102(bswap_32big(0x04d9a659U), bswap_32big(0xd946ca73U), bswap_32big(0x3ede3354U),
                       bswap_32big(0xe69cba12U));
        WR1_PROG(REG_14BCH, 0x00000040U);
        WAIT_STS(REG_142CH, 12, 0);

        return RSIP_RET_FAIL;
    }
    else
    {
        WR1_PROG(REG_1444H, 0x000000a7U);
        WR1_PROG(REG_1608H, 0x800103a0U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, bswap_32big(0x000000deU));
        WR1_PROG(REG_1458H, 0x00000000U);

        r_rsip_func101(bswap_32big(0x2cbc719fU), bswap_32big(0xcc4fcd46U), bswap_32big(0xc2812bb7U),
                       bswap_32big(0xbea885a9U));
        r_rsip_func414(OutData_EncCertificateInfo);

        r_rsip_func102(bswap_32big(0x992234b3U), bswap_32big(0x99aa94a4U), bswap_32big(0x00361d97U),
                       bswap_32big(0xcc03ef5eU));
        WR1_PROG(REG_14BCH, 0x00000040U);
        WAIT_STS(REG_142CH, 12, 0);

        return RSIP_RET_PASS;
    }
}
