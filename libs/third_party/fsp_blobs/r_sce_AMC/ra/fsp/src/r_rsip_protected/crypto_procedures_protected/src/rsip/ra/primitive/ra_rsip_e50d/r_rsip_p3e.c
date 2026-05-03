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

rsip_ret_t r_rsip_p3e (uint32_t OutData_KeyIndex[])
{
    if (RD1_MASK(REG_14BCH, 0x0000001fU) != 0)
    {
        return RSIP_RET_RESOURCE_CONFLICT;
    }

    WR1_PROG(REG_1B00H, 0x003e0001U);
    WR1_PROG(REG_144CH, 0x00000000U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00003e01U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x06005ab1U),
                   bswap_32big(0xa36f5dd6U),
                   bswap_32big(0x1a1407b3U),
                   bswap_32big(0x65ffcb7dU));
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
    WR1_PROG(REG_1420H, bswap_32big(0x0000003eU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x46012c3cU),
                   bswap_32big(0xeef99d2bU),
                   bswap_32big(0x9f6e4701U),
                   bswap_32big(0x306da1d2U));
    r_rsip_func043();

    WR1_PROG(REG_1600H, 0x0000b4e0U);
    WR1_PROG(REG_1600H, 0x0000002aU);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x0000003eU));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xb4c24b93U),
                   bswap_32big(0xd29e9802U),
                   bswap_32big(0x61686ffeU),
                   bswap_32big(0x675d0b73U));
    r_rsip_func044();

    r_rsip_func100(bswap_32big(0x1ee61a43U),
                   bswap_32big(0x4939033bU),
                   bswap_32big(0x5a826d99U),
                   bswap_32big(0xc2330fabU));
    WR1_PROG(REG_1608H, 0x81010000U);
    WR1_PROG(REG_1408H, 0x00005006U);
    WAIT_STS(REG_1408H, 30, 1);
    RD1_ADDR(REG_1420H, &OutData_KeyIndex[0]);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00003e02U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xf8dc6d84U),
                   bswap_32big(0x067c4344U),
                   bswap_32big(0x3c8944fdU),
                   bswap_32big(0x1a11d635U));
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
    WR1_PROG(REG_1420H, bswap_32big(0x00003e03U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x9d92b024U),
                   bswap_32big(0xde638b7dU),
                   bswap_32big(0xbe7d87dfU),
                   bswap_32big(0xbea0fa4dU));
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
    WR1_PROG(REG_1420H, bswap_32big(0x00003e04U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x32828eb2U),
                   bswap_32big(0xa80426eaU),
                   bswap_32big(0xe4590065U),
                   bswap_32big(0xb7cacfdbU));
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
    WR1_PROG(REG_1420H, bswap_32big(0x00003e05U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xfa32de6eU),
                   bswap_32big(0x3ae4c743U),
                   bswap_32big(0xcf19ed49U),
                   bswap_32big(0xb25f3c4fU));
    r_rsip_func103();
    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x0c200104U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));
    WR1_PROG(REG_1608H, 0x80040180U);
    WR1_PROG(REG_1400H, 0x03420011U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    r_rsip_func100(bswap_32big(0x0cc6291bU),
                   bswap_32big(0xc96aba36U),
                   bswap_32big(0x637b929fU),
                   bswap_32big(0xa15ed7f4U));

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

    r_rsip_func100(bswap_32big(0x078583dbU),
                   bswap_32big(0xa339896aU),
                   bswap_32big(0x76412dd8U),
                   bswap_32big(0x257ee941U));
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

    r_rsip_func102(bswap_32big(0xe5afa511U),
                   bswap_32big(0xe1852370U),
                   bswap_32big(0xb8d9e32dU),
                   bswap_32big(0x61c0e8d6U));
    WR1_PROG(REG_14BCH, 0x00000040U);
    WAIT_STS(REG_142CH, 12, 0);

    return RSIP_RET_PASS;
}
