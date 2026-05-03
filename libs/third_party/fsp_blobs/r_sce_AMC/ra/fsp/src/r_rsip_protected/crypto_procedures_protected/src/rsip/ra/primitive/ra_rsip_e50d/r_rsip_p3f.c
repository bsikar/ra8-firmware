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

rsip_ret_t r_rsip_p3f (uint32_t OutData_KeyIndex[])
{
    if (RD1_MASK(REG_14BCH, 0x0000001fU) != 0)
    {
        return RSIP_RET_RESOURCE_CONFLICT;
    }

    WR1_PROG(REG_1B00H, 0x003f0001U);
    WR1_PROG(REG_144CH, 0x00000000U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00003f01U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xe5cc8770U),
                   bswap_32big(0x21210170U),
                   bswap_32big(0xccbf9c84U),
                   bswap_32big(0x496e916dU));
    r_rsip_func103();
    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x0c200104U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));

    WR1_PROG(REG_1608H, 0x80010000U);
    WR1_PROG(REG_1400H, 0x03420005U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);
    WR1_PROG(REG_1400H, 0x0002000dU);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    WR1_PROG(REG_1600H, 0x000034e0U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x0000003fU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x7c2987a4U),
                   bswap_32big(0x9f00c16fU),
                   bswap_32big(0xe9ff9eb8U),
                   bswap_32big(0x9a15de96U));
    r_rsip_func043();

    WR1_PROG(REG_1600H, 0x0000b4e0U);
    WR1_PROG(REG_1600H, 0x0000002bU);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x0000003fU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xb1687ba5U),
                   bswap_32big(0xf2a3f9a3U),
                   bswap_32big(0xdd4d8b50U),
                   bswap_32big(0x4214c5d6U));
    r_rsip_func044();

    r_rsip_func100(bswap_32big(0xcd0e607dU),
                   bswap_32big(0xecf00df6U),
                   bswap_32big(0x2b2deb84U),
                   bswap_32big(0xa08e4ed0U));
    WR1_PROG(REG_1608H, 0x81010000U);
    WR1_PROG(REG_1408H, 0x00005006U);
    WAIT_STS(REG_1408H, 30, 1);
    RD1_ADDR(REG_1420H, &OutData_KeyIndex[0]);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00003f02U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x63fe714aU),
                   bswap_32big(0x5fc32757U),
                   bswap_32big(0xb43d4275U),
                   bswap_32big(0x36e6823dU));
    r_rsip_func103();
    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x0c200104U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));
    WR1_PROG(REG_1608H, 0x80040000U);
    WR1_PROG(REG_1400H, 0x03420011U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00003f03U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x4b0d7a04U),
                   bswap_32big(0x17159db3U),
                   bswap_32big(0x23c00ee8U),
                   bswap_32big(0xd3780adbU));
    r_rsip_func103();
    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x0c200104U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));
    WR1_PROG(REG_1608H, 0x80040080U);
    WR1_PROG(REG_1400H, 0x03420011U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00003f04U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xc676fb8dU),
                   bswap_32big(0xba8c37c3U),
                   bswap_32big(0x3b357521U),
                   bswap_32big(0x87cf6e19U));
    r_rsip_func103();
    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x0c200104U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));
    WR1_PROG(REG_1608H, 0x80040100U);
    WR1_PROG(REG_1400H, 0x03420011U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00003f05U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xb5f740e3U),
                   bswap_32big(0xb9ce332cU),
                   bswap_32big(0xb5fa3856U),
                   bswap_32big(0xff6a9f74U));
    r_rsip_func103();
    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x0c200104U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));
    WR1_PROG(REG_1608H, 0x80040180U);
    WR1_PROG(REG_1400H, 0x03420011U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    r_rsip_func100(bswap_32big(0xe634a46cU),
                   bswap_32big(0x7a387a14U),
                   bswap_32big(0xebd962e2U),
                   bswap_32big(0x59490732U));

    WR1_PROG(REG_1A2CH, 0x40000300U);
    WR1_PROG(REG_1A24H, 0xe7009d47U);
    WR1_PROG(REG_1608H, 0x81100000U);
    WR1_PROG(REG_1400H, 0x00890031U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    WR1_PROG(REG_1408H, 0x00002032U);
    WAIT_STS(REG_1408H, 30, 1);
    RD4_ADDR(REG_1420H, &OutData_KeyIndex[1]);
    WAIT_STS(REG_1408H, 30, 1);
    RD4_ADDR(REG_1420H, &OutData_KeyIndex[5]);
    WAIT_STS(REG_1408H, 30, 1);
    RD4_ADDR(REG_1420H, &OutData_KeyIndex[9]);

    WR1_PROG(REG_1400H, 0x00890011U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    r_rsip_func100(bswap_32big(0xdde29597U),
                   bswap_32big(0xdb98060aU),
                   bswap_32big(0xb4c8220bU),
                   bswap_32big(0x83e8ec4eU));
    WR1_PROG(REG_1444H, 0x000003a2U);
    WR1_PROG(REG_1A2CH, 0x40000000U);
    WR1_PROG(REG_1A24H, 0x09108105U);
    WAIT_STS(REG_1444H, 31, 1);
    WR4_PROG(REG_1420H, bswap_32big(0x00000000U), bswap_32big(0x00000000U), bswap_32big(0x00000000U),
             bswap_32big(0x00000004U));

    WR1_PROG(REG_1408H, 0x00002022U);
    WAIT_STS(REG_1408H, 30, 1);
    RD4_ADDR(REG_1420H, &OutData_KeyIndex[13]);
    WAIT_STS(REG_1408H, 30, 1);
    RD4_ADDR(REG_1420H, &OutData_KeyIndex[17]);

    r_rsip_func102(bswap_32big(0xc033813fU),
                   bswap_32big(0xa72fd4eaU),
                   bswap_32big(0xd4e2e703U),
                   bswap_32big(0x7c992189U));
    WR1_PROG(REG_14BCH, 0x00000040U);
    WAIT_STS(REG_142CH, 12, 0);

    return RSIP_RET_PASS;
}
