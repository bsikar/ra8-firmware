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

rsip_ret_t r_rsip_p75f (const uint32_t InData_Cmd[],
                        const uint32_t InData_Msg[],
                        const uint32_t InData_MAC[],
                        const uint32_t InData_length[],
                        uint32_t       MAX_CNT,
                        uint32_t       OutData_MAC[])
{
    uint32_t iLoop = 0U;
    uint32_t jLoop = 0U;

    WAIT_STS(REG_2030H, 0, 1);

    WR1_PROG(REG_1444H, 0x00020064U);

    for (iLoop = 0U; iLoop < (MAX_CNT & 0xfffffff0U); )
    {
        WAIT_STS(REG_1444H, 31, 1);
        WR16_ADDR(REG_1420H, &InData_Msg[iLoop]);
        iLoop = iLoop + 16U;
    }

    WR1_PROG(REG_1458H, 0x00000000U);

    WAIT_STS(REG_1444H, 31, 1);
    for (iLoop = (MAX_CNT & 0xfffffff0U); iLoop < MAX_CNT; iLoop++)
    {
        WR1_PROG(REG_1420H, InData_Msg[iLoop]);
    }

    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1444H, 0x00000000U);
    WAIT_STS(REG_2030H, 8, 0);
    WR1_PROG(REG_143CH, 0x00001600U);

    WAIT_STS(REG_2030H, 4, 1);

    WR1_PROG(REG_1600H, 0x38008880U);
    WR1_PROG(REG_1600H, 0x00000003U);
    WR1_PROG(REG_1600H, 0x1000b480U);
    WR1_PROG(REG_1600H, 0x00000001U);

    WR1_PROG(REG_1600H, 0x38008880U);
    WR1_PROG(REG_1600H, 0x00000004U);
    WR1_PROG(REG_1600H, 0x1000b480U);
    WR1_PROG(REG_1600H, 0x00000002U);
    WR1_PROG(REG_1444H, 0x000000c7U);
    WR1_PROG(REG_1608H, 0x80010000U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, InData_Cmd[0]);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1600H, 0x38000c00U);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00260000U);

    r_rsip_func100(bswap_32big(0x8f26de5cU),
                   bswap_32big(0x3584c737U),
                   bswap_32big(0xaa67e0a2U),
                   bswap_32big(0x26998ff0U));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        r_rsip_func100(bswap_32big(0xac24d026U), bswap_32big(0x2fdd6c90U), bswap_32big(0x65ad857eU),
                       bswap_32big(0x22b1bdf0U));

        WR1_PROG(REG_1600H, 0x00007c04U);
        WR1_PROG(REG_143CH, 0x00600000U);
        WR1_PROG(REG_1458H, 0x00000000U);

        if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000001U)
        {
            r_rsip_func100(bswap_32big(0xed864825U), bswap_32big(0x68261803U), bswap_32big(0xcb5836f6U),
                           bswap_32big(0xb755dc1eU));
            WR1_PROG(REG_1408H, 0x0000401eU);
            WAIT_STS(REG_1408H, 30, 1);
            RD7_ADDR(REG_1420H, &OutData_MAC[0]);

            r_rsip_func102(bswap_32big(0xddbf612bU), bswap_32big(0xa582587aU), bswap_32big(0x7ed46e28U),
                           bswap_32big(0x93cd04efU));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000002U)
        {
            r_rsip_func100(bswap_32big(0x4539a7adU), bswap_32big(0x0baa0303U), bswap_32big(0x8ae5339bU),
                           bswap_32big(0xfccb6369U));
            WR1_PROG(REG_1408H, 0x00004022U);
            WAIT_STS(REG_1408H, 30, 1);
            RD8_ADDR(REG_1420H, &OutData_MAC[0]);

            r_rsip_func102(bswap_32big(0x92643616U), bswap_32big(0xfb9e9561U), bswap_32big(0xf3a2423bU),
                           bswap_32big(0xf640f66bU));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000005U)
        {
            r_rsip_func100(bswap_32big(0x54da3e9dU), bswap_32big(0x033d8ff1U), bswap_32big(0x94c3793fU),
                           bswap_32big(0x21c066d9U));
            WR1_PROG(REG_1408H, 0x00004032U);
            WAIT_STS(REG_1408H, 30, 1);
            RD12_ADDR(REG_1420H, &OutData_MAC[0]);

            r_rsip_func102(bswap_32big(0xc797cbf0U), bswap_32big(0x63825071U), bswap_32big(0xa95a50ffU),
                           bswap_32big(0x3d356e9eU));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000006U)
        {
            r_rsip_func100(bswap_32big(0x1c2533eeU), bswap_32big(0x0b48c304U), bswap_32big(0x4a5ca286U),
                           bswap_32big(0x47bac3dfU));
            WR1_PROG(REG_1408H, 0x00004042U);
            WAIT_STS(REG_1408H, 30, 1);
            RD16_ADDR(REG_1420H, &OutData_MAC[0]);

            r_rsip_func102(bswap_32big(0x3cf02eafU), bswap_32big(0x62e61407U), bswap_32big(0x3bc6445eU),
                           bswap_32big(0xa3943767U));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);
        }

        return RSIP_RET_PASS;
    }
    else
    {
        r_rsip_func100(bswap_32big(0x27b493a5U), bswap_32big(0x11583282U), bswap_32big(0x360126acU),
                       bswap_32big(0x9656790fU));

        WR1_PROG(REG_1444H, 0x000000c7U);
        WR1_PROG(REG_1608H, 0x80010020U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, InData_length[0]);
        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1600H, 0x00007c04U);
        WR1_PROG(REG_143CH, 0x00600000U);
        WR1_PROG(REG_1458H, 0x00000000U);

        if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000001U)
        {
            WR1_PROG(REG_1600H, 0x0000b440U);
            WR1_PROG(REG_1600H, 0x0000001cU);

            r_rsip_func101(bswap_32big(0xbdcf856aU), bswap_32big(0x7d3b9b31U), bswap_32big(0x72a9c0d0U),
                           bswap_32big(0xffdd2e6dU));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000002U)
        {
            WR1_PROG(REG_1600H, 0x0000b440U);
            WR1_PROG(REG_1600H, 0x00000020U);

            r_rsip_func101(bswap_32big(0xa34bc379U), bswap_32big(0x65f649baU), bswap_32big(0x18a303e4U),
                           bswap_32big(0xc6b0f1a7U));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000005U)
        {
            WR1_PROG(REG_1600H, 0x0000b440U);
            WR1_PROG(REG_1600H, 0x00000030U);

            r_rsip_func101(bswap_32big(0x463509c2U), bswap_32big(0x8277a129U), bswap_32big(0x18ca3529U),
                           bswap_32big(0x4c3da5d5U));
        }
        else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000006U)
        {
            WR1_PROG(REG_1600H, 0x0000b440U);
            WR1_PROG(REG_1600H, 0x00000040U);

            r_rsip_func101(bswap_32big(0xf99cd1e9U), bswap_32big(0x311aa5e3U), bswap_32big(0x3dd24839U),
                           bswap_32big(0xc1a85e88U));
        }

        WR1_PROG(REG_1600H, 0x3420a820U);
        WR1_PROG(REG_1600H, 0x00000004U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00260000U);

        WR1_PROG(REG_1600H, 0x34202841U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00260000U);

        r_rsip_func100(bswap_32big(0x288cc714U), bswap_32big(0xb2576d89U), bswap_32big(0x51b3dbd5U),
                       bswap_32big(0x74dda411U));
        WR1_PROG(REG_143CH, 0x00400000U);

        if (CHCK_STS(REG_143CH, 22, 1))
        {
            r_rsip_func102(bswap_32big(0xa591d5d6U), bswap_32big(0x0db6ba0aU), bswap_32big(0xc58cd089U),
                           bswap_32big(0xe475a057U));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);

            return RSIP_RET_FAIL;
        }
        else
        {
            r_rsip_func100(bswap_32big(0x8baea9e4U), bswap_32big(0x7d429282U), bswap_32big(0x074aeb80U),
                           bswap_32big(0xbe27aba0U));

            WR1_PROG(REG_1600H, 0x000008c6U);

            WR1_PROG(REG_1600H, 0x000008a5U);

            WR1_PROG(REG_1600H, 0x0000a440U);
            WR1_PROG(REG_1600H, 0x0000000fU);
            WR1_PROG(REG_1600H, 0x00046842U);
            WR1_PROG(REG_1600H, 0x00026c42U);

            WR1_PROG(REG_1608H, 0x81010040U);
            WR1_PROG(REG_1408H, 0x00005006U);
            WAIT_STS(REG_1408H, 30, 1);
            RD1_ADDR(REG_1420H, &S_RAM[0]);
            S_RAM[0] = bswap_32big(S_RAM[0]);

            for (iLoop = 0U; iLoop < S_RAM[0]; )
            {
                r_rsip_func100(bswap_32big(0xda719471U),
                               bswap_32big(0x7b6e23daU),
                               bswap_32big(0x84d9cf3bU),
                               bswap_32big(0x9e8619abU));

                WR1_PROG(REG_1600H, 0x000008e7U);

                WR1_PROG(REG_1600H, 0x00000863U);

                WR1_PROG(REG_1600H, 0x380088c0U);
                WR1_PROG(REG_1600H, 0x00000004U);
                WR1_PROG(REG_1600H, 0x2000d060U);

                WR1_PROG(REG_1600H, 0x38008880U);
                WR1_PROG(REG_1600H, 0x00000001U);
                WR1_PROG(REG_1600H, 0x2000d060U);

                WR1_PROG(REG_1600H, 0x00007c03U);
                WR1_PROG(REG_143CH, 0x00600000U);
                WR1_PROG(REG_1458H, 0x00000000U);

                if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000000U)
                {
                    WR1_PROG(REG_1608H, 0x80830007U);
                    WR1_PROG(REG_1400H, 0x0345000dU);

                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1444H, 0x000002c1U);
                    WR1_PROG(REG_1824H, 0x08000045U);
                    WAIT_STS(REG_1444H, 31, 1);
                    WR3_ADDR(REG_1420H, &InData_MAC[iLoop]);
                    WR1_PROG(REG_1444H, 0x000000a1U);
                    WAIT_STS(REG_1444H, 31, 1);
                    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));

                    r_rsip_func101(bswap_32big(0x29f7ab0aU),
                                   bswap_32big(0xad41feb8U),
                                   bswap_32big(0x65713280U),
                                   bswap_32big(0xfd39715eU));
                }
                else
                {
                    WR1_PROG(REG_1608H, 0x80840007U);
                    WR1_PROG(REG_1400H, 0x03450011U);

                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1444H, 0x000003c1U);
                    WR1_PROG(REG_1824H, 0x08000045U);
                    WAIT_STS(REG_1444H, 31, 1);
                    WR4_ADDR(REG_1420H, &InData_MAC[iLoop]);

                    r_rsip_func101(bswap_32big(0x771de06aU),
                                   bswap_32big(0x427323c3U),
                                   bswap_32big(0x5f6f85f0U),
                                   bswap_32big(0x57bd697cU));
                }

                WR1_PROG(REG_1458H, 0x00000000U);

                WR1_PROG(REG_1600H, 0x00000800U);
                for (jLoop = 0U; jLoop < 16U; jLoop++)
                {
                    WR1_PROG(REG_1600H, 0x3c0028a1U);
                    WR1_PROG(REG_1600H, 0x12003c07U);
                    WR1_PROG(REG_1600H, 0x00002ce0U);
                    WR1_PROG(REG_1600H, 0x00002ca0U);
                }

                WR1_PROG(REG_1458H, 0x00000000U);

                WR1_PROG(REG_1824H, 0x9c000005U);
                WR1_PROG(REG_1600H, 0x000008e7U);
                WR1_PROG(REG_1608H, 0x81840007U);
                WR1_PROG(REG_1400H, 0x00490011U);
                WAIT_STS(REG_1404H, 30, 0);
                WR1_PROG(REG_143CH, 0x00001800U);

                WR1_PROG(REG_1600H, 0x0000a4c0U);
                WR1_PROG(REG_1600H, 0x00000004U);

                r_rsip_func101(bswap_32big(0x298e2ab0U),
                               bswap_32big(0x9a2d1b27U),
                               bswap_32big(0xec3c2ceaU),
                               bswap_32big(0xd524cbf6U));
                iLoop = iLoop + 4U;
            }

            WR1_PROG(REG_1600H, 0x000008c2U);

            WR1_PROG(REG_1600H, 0x00007c06U);
            WR1_PROG(REG_143CH, 0x00602000U);
            WR1_PROG(REG_1458H, 0x00000000U);

            r_rsip_func100(bswap_32big(0xaf6f5e67U), bswap_32big(0x773f2216U), bswap_32big(0x094b1d18U),
                           bswap_32big(0x8548d3f2U));
            WR1_PROG(REG_143CH, 0x00400000U);

            if (CHCK_STS(REG_143CH, 22, 1))
            {
                r_rsip_func102(bswap_32big(0x252b31a1U),
                               bswap_32big(0x91cc3fbfU),
                               bswap_32big(0x7447417eU),
                               bswap_32big(0x435514d4U));
                WR1_PROG(REG_14BCH, 0x00000040U);
                WAIT_STS(REG_142CH, 12, 0);

                return RSIP_RET_FAIL;
            }
            else
            {
                r_rsip_func102(bswap_32big(0x5b1ad2ecU),
                               bswap_32big(0x7e0c8e9cU),
                               bswap_32big(0x85adf7f4U),
                               bswap_32big(0xe831d6deU));
                WR1_PROG(REG_14BCH, 0x00000040U);
                WAIT_STS(REG_142CH, 12, 0);

                return RSIP_RET_PASS;
            }
        }
    }
}
