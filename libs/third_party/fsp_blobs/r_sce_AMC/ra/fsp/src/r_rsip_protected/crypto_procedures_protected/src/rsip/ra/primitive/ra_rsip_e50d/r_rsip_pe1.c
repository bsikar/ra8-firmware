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

rsip_ret_t r_rsip_pe1 (const uint32_t InData_Sel_KeyType[],
                       const uint32_t InData_HashType[],
                       const uint32_t InData_Certificate[],
                       const uint32_t InData_CertificateLength[],
                       const uint32_t InData_CertificatePubKey[],
                       const uint32_t InData_EncCertificateInfo[],
                       uint32_t       OutData_KeyIndex[])
{
    uint32_t iLoop = 0U;
    uint32_t jLoop = 0U;
    uint32_t iTemp = 0U;

    if (RD1_MASK(REG_14BCH, 0x0000001fU) != 0)
    {
        return RSIP_RET_RESOURCE_CONFLICT;
    }

    WR1_PROG(REG_1B00H, 0x00e10001U);
    WR1_PROG(REG_144CH, 0x00000000U);

    WR1_PROG(REG_1444H, 0x000000c7U);
    WR1_PROG(REG_1608H, 0x800101e0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, InData_Sel_KeyType[0]);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1600H, 0x3420a9e0U);
    WR1_PROG(REG_1600H, 0x00000009U);
    WR1_PROG(REG_1600H, 0x2000b5e0U);
    WR1_PROG(REG_1600H, 0x00000008U);

    WR1_PROG(REG_1600H, 0x3000a9e0U);
    WR1_PROG(REG_1600H, 0x00000006U);
    WR1_PROG(REG_1600H, 0x00070020U);
    WR1_PROG(REG_1600H, 0x3000a9e0U);
    WR1_PROG(REG_1600H, 0x00000004U);
    WR1_PROG(REG_1600H, 0x00070020U);
    WR1_PROG(REG_1600H, 0x0000b5c0U);
    WR1_PROG(REG_1600H, 0x010f6caaU);

    WR1_PROG(REG_1600H, 0x0000b600U);
    WR1_PROG(REG_1600H, 0x00000008U);
    WR1_PROG(REG_1600H, 0x00030040U);
    WR1_PROG(REG_1600H, 0x0000b5c0U);
    WR1_PROG(REG_1600H, 0x01f6c222U);

    WR1_PROG(REG_1600H, 0x0000b600U);
    WR1_PROG(REG_1600H, 0x0000000cU);
    WR1_PROG(REG_1600H, 0x00050040U);
    WR1_PROG(REG_1600H, 0x0000b5c0U);
    WR1_PROG(REG_1600H, 0x013a8e02U);

    WR1_PROG(REG_1600H, 0x0000b600U);
    WR1_PROG(REG_1600H, 0x00000010U);
    WR1_PROG(REG_1600H, 0x00000080U);
    WR1_PROG(REG_1600H, 0x00000060U);
    WR1_PROG(REG_1600H, 0x3000a9e0U);
    WR1_PROG(REG_1600H, 0x00000007U);
    WR1_PROG(REG_1600H, 0x00070020U);
    WR1_PROG(REG_1600H, 0x0000b5c0U);
    WR1_PROG(REG_1600H, 0x010f6caaU);

    WR1_PROG(REG_1600H, 0x0000b600U);
    WR1_PROG(REG_1600H, 0x00000008U);
    WR1_PROG(REG_1600H, 0x00030040U);
    WR1_PROG(REG_1600H, 0x0000b5c0U);
    WR1_PROG(REG_1600H, 0x01f6c222U);

    WR1_PROG(REG_1600H, 0x0000b600U);
    WR1_PROG(REG_1600H, 0x0000000cU);
    WR1_PROG(REG_1600H, 0x00050040U);
    WR1_PROG(REG_1600H, 0x0000b5c0U);
    WR1_PROG(REG_1600H, 0x013a8e02U);

    WR1_PROG(REG_1600H, 0x0000b600U);
    WR1_PROG(REG_1600H, 0x00000010U);
    WR1_PROG(REG_1600H, 0x00000080U);
    WR1_PROG(REG_1600H, 0x00000080U);

    r_rsip_func100(bswap_32big(0xb402967aU),
                   bswap_32big(0x8a37a034U),
                   bswap_32big(0xa7ea0e94U),
                   bswap_32big(0x3e94e8ceU));
    WR1_PROG(REG_1608H, 0x81010200U);
    WR1_PROG(REG_1408H, 0x00005006U);
    WAIT_STS(REG_1408H, 30, 1);
    RD1_ADDR(REG_1420H, &S_RAM[0]);
    S_RAM[0] = bswap_32big(S_RAM[0]);

    WR1_PROG(REG_1444H, 0x000000c7U);
    WR1_PROG(REG_1608H, 0x800100e0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, InData_EncCertificateInfo[0]);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x0000e101U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0x0a04597dU),
                   bswap_32big(0xf2d8041eU),
                   bswap_32big(0xbc5e29f9U),
                   bswap_32big(0xfc2c346fU));
    r_rsip_func043();

    WR1_PROG(REG_1600H, 0x000034eeU);

    WR1_PROG(REG_1444H, 0x000000a7U);
    WR1_PROG(REG_1608H, 0x800103a0U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x0000e101U));
    WR1_PROG(REG_1458H, 0x00000000U);

    r_rsip_func101(bswap_32big(0xc9540137U),
                   bswap_32big(0xd995ff31U),
                   bswap_32big(0xb0a7cf4eU),
                   bswap_32big(0xc2e287a6U));
    r_rsip_func044();

    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x08000044U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));

    WR1_PROG(REG_1444H, 0x000000a2U);
    WR1_PROG(REG_1A24H, 0x08000054U);
    WAIT_STS(REG_1444H, 31, 1);
    WR1_PROG(REG_1420H, bswap_32big(0x00000000U));

    WR1_PROG(REG_1600H, 0x000008a5U);

    WR1_PROG(REG_1600H, 0x00000800U);

    for (iLoop = 0U; iLoop < S_RAM[0]; )
    {
        WR1_PROG(REG_1444H, 0x000003c2U);
        WR1_PROG(REG_1A2CH, 0x40000000U);
        WR1_PROG(REG_1A24H, 0xf7008d05U);
        WAIT_STS(REG_1444H, 31, 1);
        WR4_ADDR(REG_1420H, &InData_EncCertificateInfo[1 + iLoop]);

        WR1_PROG(REG_1608H, 0x80840000U);
        WR1_PROG(REG_1400H, 0x03420011U);
        WAIT_STS(REG_1404H, 30, 0);
        WR1_PROG(REG_143CH, 0x00001800U);

        WR1_PROG(REG_1600H, 0x0000a400U);
        WR1_PROG(REG_1600H, 0x00000010U);

        WR1_PROG(REG_1600H, 0x0000a4a0U);
        WR1_PROG(REG_1600H, 0x00000004U);

        r_rsip_func101(bswap_32big(0xe6724a0dU), bswap_32big(0x45c62eb2U), bswap_32big(0x2c607da4U),
                       bswap_32big(0x92de6debU));
        iLoop = iLoop + 4U;
    }

    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1600H, 0x38000a05U);
    WR1_PROG(REG_1608H, 0x00000080U);
    WR1_PROG(REG_143CH, 0x00260000U);
    WR1_PROG(REG_143CH, 0x00402000U);
    WR1_PROG(REG_1458H, 0x00000000U);

    WR1_PROG(REG_1444H, 0x000003c2U);
    WR1_PROG(REG_1A2CH, 0x40000000U);
    WR1_PROG(REG_1A24H, 0x07008d05U);
    WAIT_STS(REG_1444H, 31, 1);
    WR4_ADDR(REG_1420H, &InData_EncCertificateInfo[1 + iLoop]);

    WR1_PROG(REG_1A24H, 0x9c100005U);
    WR1_PROG(REG_1400H, 0x00820011U);
    WAIT_STS(REG_1404H, 30, 0);
    WR1_PROG(REG_143CH, 0x00001800U);

    r_rsip_func100(bswap_32big(0xbf71ef6cU),
                   bswap_32big(0xff5c0fdbU),
                   bswap_32big(0xc76f6c8bU),
                   bswap_32big(0x4a4ebcfcU));
    WR1_PROG(REG_143CH, 0x00400000U);

    if (CHCK_STS(REG_143CH, 22, 1))
    {
        r_rsip_func102(bswap_32big(0xab87a1a5U), bswap_32big(0xf5e9b4b2U), bswap_32big(0x4265aa06U),
                       bswap_32big(0x0db6f774U));
        WR1_PROG(REG_14BCH, 0x00000040U);
        WAIT_STS(REG_142CH, 12, 0);

        return RSIP_RET_FAIL;
    }
    else
    {
        WR1_PROG(REG_1444H, 0x000000c7U);
        WR1_PROG(REG_1608H, 0x800102c0U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, InData_CertificateLength[0]);
        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_1444H, 0x000003c7U);
        WR1_PROG(REG_1608H, 0x800402e0U);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, InData_CertificatePubKey[0]);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, InData_CertificatePubKey[1]);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, InData_CertificatePubKey[2]);
        WAIT_STS(REG_1444H, 31, 1);
        WR1_PROG(REG_1420H, InData_CertificatePubKey[3]);
        WR1_PROG(REG_1458H, 0x00000000U);

        WR1_PROG(REG_2000H, 0x00000001U);

        WR1_PROG(REG_1600H, 0x0000b7e0U);
        WR1_PROG(REG_1600H, 0x208e11f0U);

        WR1_PROG(REG_1600H, 0x3420a9e0U);
        WR1_PROG(REG_1600H, 0x00000003U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00260000U);

        r_rsip_func100(bswap_32big(0xa33c1fdeU), bswap_32big(0x731ae725U), bswap_32big(0x31065fccU),
                       bswap_32big(0x3c4f7d2cU));
        WR1_PROG(REG_143CH, 0x00400000U);

        if (CHCK_STS(REG_143CH, 22, 1))
        {
            r_rsip_func100(bswap_32big(0xce200afaU), bswap_32big(0x912432f8U), bswap_32big(0xefa74d9bU),
                           bswap_32big(0x206d6a2eU));

            WR1_PROG(REG_1444H, 0x000000c7U);
            WR1_PROG(REG_1608H, 0x80010080U);
            WAIT_STS(REG_1444H, 31, 1);
            WR1_PROG(REG_1420H, InData_HashType[0]);
            WR1_PROG(REG_1458H, 0x00000000U);

            WR1_PROG(REG_1600H, 0x3420a880U);
            WR1_PROG(REG_1600H, 0x00000006U);
            WR1_PROG(REG_1600H, 0x2000b480U);
            WR1_PROG(REG_1600H, 0x00000005U);

            WR1_PROG(REG_1600H, 0x00007c04U);
            WR1_PROG(REG_143CH, 0x00600000U);
            WR1_PROG(REG_1458H, 0x00000000U);

            if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000000U)
            {
                WR1_PROG(REG_2004H, 0x00000040U);

                r_rsip_func101(bswap_32big(0x26294db9U),
                               bswap_32big(0x715e900aU),
                               bswap_32big(0x2f1f1a30U),
                               bswap_32big(0xa66b7cf9U));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000001U)
            {
                WR1_PROG(REG_2004H, 0x00000050U);

                r_rsip_func101(bswap_32big(0x799404d4U),
                               bswap_32big(0xa0c7f57aU),
                               bswap_32big(0x2af59006U),
                               bswap_32big(0xcad971efU));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000002U)
            {
                WR1_PROG(REG_2004H, 0x00000080U);

                r_rsip_func101(bswap_32big(0xca12218eU),
                               bswap_32big(0x8ac39178U),
                               bswap_32big(0xd92077d5U),
                               bswap_32big(0x37a1cf71U));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000003U)
            {
                WR1_PROG(REG_2004H, 0x00000090U);

                r_rsip_func101(bswap_32big(0x5441e459U),
                               bswap_32big(0x99ebd00aU),
                               bswap_32big(0x4fb1e2b4U),
                               bswap_32big(0xcd47a6b4U));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000004U)
            {
                WR1_PROG(REG_2004H, 0x000000a0U);

                r_rsip_func101(bswap_32big(0xb3790030U),
                               bswap_32big(0xe65f6e44U),
                               bswap_32big(0x4bff579aU),
                               bswap_32big(0x622ab2ddU));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000005U)
            {
                WR1_PROG(REG_2004H, 0x000000b0U);

                r_rsip_func101(bswap_32big(0x6c2bb682U),
                               bswap_32big(0xaf30bba9U),
                               bswap_32big(0xdb97e219U),
                               bswap_32big(0xa4eacc42U));
            }

            WR1_PROG(REG_1600H, 0x300031e0U);
            WR1_PROG(REG_1600H, 0x00070020U);
            WR1_PROG(REG_1600H, 0x0000b400U);
            WR1_PROG(REG_1600H, 0x000000ffU);

            WR1_PROG(REG_1600H, 0x0000b760U);
            WR1_PROG(REG_1600H, 0x00000040U);

            WR1_PROG(REG_1600H, 0x0000b5c0U);
            WR1_PROG(REG_1600H, 0x0000000cU);
            WR1_PROG(REG_1600H, 0x00030040U);
            WR1_PROG(REG_1600H, 0x0000b400U);
            WR1_PROG(REG_1600H, 0x0000017fU);

            WR1_PROG(REG_1600H, 0x0000b760U);
            WR1_PROG(REG_1600H, 0x00000060U);

            WR1_PROG(REG_1600H, 0x0000b5c0U);
            WR1_PROG(REG_1600H, 0x0000000eU);
            WR1_PROG(REG_1600H, 0x00050040U);
            WR1_PROG(REG_1600H, 0x0000b400U);
            WR1_PROG(REG_1600H, 0x000001ffU);

            WR1_PROG(REG_1600H, 0x0000b760U);
            WR1_PROG(REG_1600H, 0x00000080U);

            WR1_PROG(REG_1600H, 0x0000b5c0U);
            WR1_PROG(REG_1600H, 0x00000010U);
            WR1_PROG(REG_1600H, 0x00000080U);

            WR1_PROG(REG_1600H, 0x0000b420U);
            WR1_PROG(REG_1600H, 0x00000003U);

            WR1_PROG(REG_1600H, 0x3c002b59U);
            WR1_PROG(REG_1600H, 0x20000bffU);

            WR1_PROG(REG_1600H, 0x00002439U);
            WR1_PROG(REG_1600H, 0x3c00283aU);
            WR1_PROG(REG_1600H, 0x20000bffU);

            r_rsip_func101(bswap_32big(0x0d93ee35U), bswap_32big(0x8e7d114aU), bswap_32big(0x392877c6U),
                           bswap_32big(0x1fcf1c0aU));
        }
        else
        {
            r_rsip_func100(bswap_32big(0xe5d267c4U), bswap_32big(0x094f10a9U), bswap_32big(0xa63a1694U),
                           bswap_32big(0x215cf415U));
            WR1_PROG(REG_1600H, 0x00007c0fU);
            WR1_PROG(REG_143CH, 0x00600000U);
            WR1_PROG(REG_1458H, 0x00000000U);

            if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000003U)
            {
                WR1_PROG(REG_1600H, 0x0000b480U);
                WR1_PROG(REG_1600H, 0x00000001U);

                WR1_PROG(REG_2004H, 0x00000050U);

                WR1_PROG(REG_1600H, 0x0000b400U);
                WR1_PROG(REG_1600H, 0x0000001fU);

                WR1_PROG(REG_1600H, 0x0000b420U);
                WR1_PROG(REG_1600H, 0x0000001fU);

                WR1_PROG(REG_1600H, 0x0000b760U);
                WR1_PROG(REG_1600H, 0x00000010U);

                WR1_PROG(REG_1600H, 0x0000b5c0U);
                WR1_PROG(REG_1600H, 0x00000016U);

                r_rsip_func101(bswap_32big(0x973b8d7fU),
                               bswap_32big(0x5b5f7d6dU),
                               bswap_32big(0xa7e666e4U),
                               bswap_32big(0x05dbfe4dU));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000004U)
            {
                WR1_PROG(REG_1600H, 0x0000b480U);
                WR1_PROG(REG_1600H, 0x00000004U);

                WR1_PROG(REG_2004H, 0x000000a0U);

                WR1_PROG(REG_1600H, 0x0000b400U);
                WR1_PROG(REG_1600H, 0x0000002fU);

                WR1_PROG(REG_1600H, 0x0000b420U);
                WR1_PROG(REG_1600H, 0x0000002fU);

                WR1_PROG(REG_1600H, 0x0000b760U);
                WR1_PROG(REG_1600H, 0x00000018U);

                WR1_PROG(REG_1600H, 0x0000b5c0U);
                WR1_PROG(REG_1600H, 0x00000018U);

                r_rsip_func101(bswap_32big(0x880ba5aeU),
                               bswap_32big(0x7edf426fU),
                               bswap_32big(0x98dcb40fU),
                               bswap_32big(0xc690cd49U));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000005U)
            {
                WR1_PROG(REG_1600H, 0x0000b480U);
                WR1_PROG(REG_1600H, 0x00000005U);

                WR1_PROG(REG_2004H, 0x000000b0U);

                WR1_PROG(REG_1600H, 0x0000b400U);
                WR1_PROG(REG_1600H, 0x00000041U);

                WR1_PROG(REG_1600H, 0x0000b420U);
                WR1_PROG(REG_1600H, 0x00000041U);

                WR1_PROG(REG_1600H, 0x0000b760U);
                WR1_PROG(REG_1600H, 0x00000028U);

                WR1_PROG(REG_1600H, 0x0000b5c0U);
                WR1_PROG(REG_1600H, 0x00000024U);

                r_rsip_func101(bswap_32big(0xfa2eb851U),
                               bswap_32big(0x97508001U),
                               bswap_32big(0x0eb07d73U),
                               bswap_32big(0xd57be4c5U));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000006U)
            {
                WR1_PROG(REG_1600H, 0x0000b480U);
                WR1_PROG(REG_1600H, 0x00000001U);

                WR1_PROG(REG_2004H, 0x00000050U);

                WR1_PROG(REG_1600H, 0x0000b400U);
                WR1_PROG(REG_1600H, 0x0000001fU);

                WR1_PROG(REG_1600H, 0x0000b420U);
                WR1_PROG(REG_1600H, 0x0000001fU);

                WR1_PROG(REG_1600H, 0x0000b760U);
                WR1_PROG(REG_1600H, 0x00000010U);

                WR1_PROG(REG_1600H, 0x0000b5c0U);
                WR1_PROG(REG_1600H, 0x0000001cU);

                r_rsip_func101(bswap_32big(0xd0f4b7a5U),
                               bswap_32big(0x68846f8cU),
                               bswap_32big(0x1f127327U),
                               bswap_32big(0x18664eb0U));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000007U)
            {
                WR1_PROG(REG_1600H, 0x0000b480U);
                WR1_PROG(REG_1600H, 0x00000004U);

                WR1_PROG(REG_2004H, 0x000000a0U);

                WR1_PROG(REG_1600H, 0x0000b400U);
                WR1_PROG(REG_1600H, 0x0000002fU);

                WR1_PROG(REG_1600H, 0x0000b420U);
                WR1_PROG(REG_1600H, 0x0000002fU);

                WR1_PROG(REG_1600H, 0x0000b760U);
                WR1_PROG(REG_1600H, 0x00000018U);

                WR1_PROG(REG_1600H, 0x0000b5c0U);
                WR1_PROG(REG_1600H, 0x0000001eU);

                r_rsip_func101(bswap_32big(0x1948eaaaU),
                               bswap_32big(0x6cc5e630U),
                               bswap_32big(0x5063e0e2U),
                               bswap_32big(0xc48f6f02U));
            }
            else if (RD1_MASK(REG_1440H, 0xffffffffU) == 0x00000008U)
            {
                WR1_PROG(REG_1600H, 0x0000b480U);
                WR1_PROG(REG_1600H, 0x00000005U);

                WR1_PROG(REG_2004H, 0x000000b0U);

                WR1_PROG(REG_1600H, 0x0000b400U);
                WR1_PROG(REG_1600H, 0x0000003fU);

                WR1_PROG(REG_1600H, 0x0000b420U);
                WR1_PROG(REG_1600H, 0x0000003fU);

                WR1_PROG(REG_1600H, 0x0000b760U);
                WR1_PROG(REG_1600H, 0x00000020U);

                WR1_PROG(REG_1600H, 0x0000b5c0U);
                WR1_PROG(REG_1600H, 0x00000020U);

                r_rsip_func101(bswap_32big(0x954b37a0U),
                               bswap_32big(0x1c9f495bU),
                               bswap_32big(0x5f866e9eU),
                               bswap_32big(0xee3d5d02U));
            }

            WR1_PROG(REG_1600H, 0x3c002b3aU);
            WR1_PROG(REG_1600H, 0x10000bffU);

            WR1_PROG(REG_1600H, 0x00002439U);
            WR1_PROG(REG_1600H, 0x0800283aU);
            WR1_PROG(REG_1600H, 0x20000bffU);

            r_rsip_func101(bswap_32big(0xadc2022aU), bswap_32big(0x0920a5ddU), bswap_32big(0x3861bd08U),
                           bswap_32big(0xc508ce01U));
        }

        WR1_PROG(REG_1600H, 0x3c002af8U);
        WR1_PROG(REG_1600H, 0x10000bffU);

        WR1_PROG(REG_1600H, 0x3c002b19U);
        WR1_PROG(REG_1600H, 0x10000bffU);

        WR1_PROG(REG_1600H, 0x00002417U);
        WR1_PROG(REG_1600H, 0x08002818U);
        WR1_PROG(REG_1600H, 0x20000bffU);

        WR1_PROG(REG_1600H, 0x3c002b56U);
        WR1_PROG(REG_1600H, 0x10000bffU);

        WR1_PROG(REG_1600H, 0x38008be0U);
        WR1_PROG(REG_1600H, 0x208e11f0U);
        WR1_PROG(REG_1608H, 0x00000080U);
        WR1_PROG(REG_143CH, 0x00A60000U);

        r_rsip_func100(bswap_32big(0x4876bf4cU), bswap_32big(0x45b7c2c9U), bswap_32big(0x95828be7U),
                       bswap_32big(0xa8fbcc75U));
        WR1_PROG(REG_143CH, 0x00400000U);

        if (CHCK_STS(REG_143CH, 22, 1))
        {
            r_rsip_func102(bswap_32big(0x32c9b1baU), bswap_32big(0x52d5d084U), bswap_32big(0xe2ab3eafU),
                           bswap_32big(0x99d46e4eU));
            WR1_PROG(REG_14BCH, 0x00000040U);
            WAIT_STS(REG_142CH, 12, 0);

            return RSIP_RET_FAIL;
        }
        else
        {
            r_rsip_func100(bswap_32big(0x24e83f8fU), bswap_32big(0xa4b6cbabU), bswap_32big(0xa5eb01b4U),
                           bswap_32big(0x95bbb57cU));
            WR1_PROG(REG_1600H, 0x00000800U);
            WR1_PROG(REG_1600H, 0x00003436U);

            WR1_PROG(REG_1600H, 0x01836c01U);
            WR1_PROG(REG_1600H, 0x00036c21U);

            WR1_PROG(REG_1608H, 0x81020000U);
            WR1_PROG(REG_1408H, 0x0000500aU);
            WAIT_STS(REG_1408H, 30, 1);
            RD1_ADDR(REG_1420H, &S_RAM[0]);
            S_RAM[0] = bswap_32big(S_RAM[0]);
            WAIT_STS(REG_1408H, 30, 1);
            RD1_ADDR(REG_1420H, &S_RAM[0 + 1]);
            S_RAM[0 + 1] = bswap_32big(S_RAM[0 + 1]);

            r_rsip_func100(bswap_32big(0xd46e31b6U), bswap_32big(0x1d90066fU), bswap_32big(0xf2d62e50U),
                           bswap_32big(0x310783baU));
            WR1_PROG(REG_1444H, 0x00000040U);
            WR1_PROG(REG_2014H, S_RAM[0]);
            WR1_PROG(REG_1444H, 0x00000040U);
            WR1_PROG(REG_2010H, S_RAM[0 + 1]);

            WR1_PROG(REG_1600H, 0x00003417U);
            WR1_PROG(REG_1600H, 0x00046800U);
            WR1_PROG(REG_1600H, 0x00026c00U);

            WR1_PROG(REG_1608H, 0x81010000U);
            WR1_PROG(REG_1408H, 0x00005006U);
            WAIT_STS(REG_1408H, 30, 1);
            RD1_ADDR(REG_1420H, &S_RAM[0]);
            S_RAM[0] = bswap_32big(S_RAM[0]);

            WR1_PROG(REG_1600H, 0x000008a5U);

            for (iLoop = 0U; iLoop < S_RAM[0]; )
            {
                WR1_PROG(REG_1444H, 0x000003c4U);
                WAIT_STS(REG_1444H, 31, 1);
                WR4_ADDR(REG_1420H, &InData_Certificate[iLoop]);

                WR1_PROG(REG_1600H, 0x0000a4a0U);
                WR1_PROG(REG_1600H, 0x00000004U);

                r_rsip_func101(bswap_32big(0x13591c52U),
                               bswap_32big(0x905bbe48U),
                               bswap_32big(0xc8638718U),
                               bswap_32big(0x2cfba1f2U));
                iLoop = iLoop + 4U;
            }

            WR1_PROG(REG_1458H, 0x00000000U);

            r_rsip_func100(bswap_32big(0x0e1580b9U), bswap_32big(0x233979bbU), bswap_32big(0x31f258e1U),
                           bswap_32big(0xa48e8808U));

            WR1_PROG(REG_1600H, 0x38000805U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);

            WR1_PROG(REG_143CH, 0x00402000U);
            WR1_PROG(REG_1458H, 0x00000000U);

            WR1_PROG(REG_1000H, 0x00010000U);
            WR1_PROG(REG_1404H, 0x10000000U);

            WR1_PROG(REG_1600H, 0x00003416U);
            WR1_PROG(REG_1600H, 0x0000a400U);
            WR1_PROG(REG_1600H, 0x00000003U);
            WR1_PROG(REG_1600H, 0x00026800U);

            WR1_PROG(REG_1608H, 0x81010000U);
            WR1_PROG(REG_1408H, 0x00005006U);
            WAIT_STS(REG_1408H, 30, 1);
            RD1_ADDR(REG_1420H, &S_RAM[0]);
            S_RAM[0] = bswap_32big(S_RAM[0]);

            WR1_PROG(REG_1600H, 0x00003445U);
            WR1_PROG(REG_1600H, 0x00026c42U);

            WR1_PROG(REG_1600H, 0x000034d6U);
            WR1_PROG(REG_1600H, 0x000030c0U);

            iTemp = iLoop;
            for (iLoop = iTemp; iLoop < S_RAM[0]; iLoop++)
            {
                WR1_PROG(REG_1444H, 0x000000c7U);
                WR1_PROG(REG_1608H, 0x80010120U);
                WAIT_STS(REG_1444H, 31, 1);
                WR1_PROG(REG_1420H, InData_Certificate[iLoop]);
                WR1_PROG(REG_1458H, 0x00000000U);

                WR1_PROG(REG_1600H, 0x00000821U);

                for (jLoop = 0U; jLoop < 4; jLoop++)
                {
                    WR1_PROG(REG_1600H, 0x00002c20U);

                    WR1_PROG(REG_1600H, 0x01886d09U);
                    WR1_PROG(REG_1600H, 0x00086d29U);

                    WR1_PROG(REG_1600H, 0x00000863U);
                    WR1_PROG(REG_1600H, 0x3c002859U);
                    WR1_PROG(REG_1600H, 0x20002c60U);
                    WR1_PROG(REG_1600H, 0x3c002b42U);
                    WR1_PROG(REG_1600H, 0x20002c60U);
                    WR1_PROG(REG_1600H, 0x38000c63U);
                    WR1_PROG(REG_1608H, 0x00000080U);
                    WR1_PROG(REG_143CH, 0x00260000U);

                    r_rsip_func100(bswap_32big(0x71450306U),
                                   bswap_32big(0x57fdef8dU),
                                   bswap_32big(0xaef2556fU),
                                   bswap_32big(0xb8b5f7c6U));
                    WR1_PROG(REG_143CH, 0x00400000U);

                    if (CHCK_STS(REG_143CH, 22, 1))
                    {
                        WR1_PROG(REG_1600H, 0x3420a9e0U);
                        WR1_PROG(REG_1600H, 0x00000003U);
                        WR1_PROG(REG_1608H, 0x00000080U);
                        WR1_PROG(REG_143CH, 0x00260000U);

                        r_rsip_func100(bswap_32big(0xf483fc54U),
                                       bswap_32big(0x220c0845U),
                                       bswap_32big(0x18062506U),
                                       bswap_32big(0x4dfec519U));
                        WR1_PROG(REG_143CH, 0x00400000U);

                        if (CHCK_STS(REG_143CH, 22, 1))
                        {
                            WR1_PROG(REG_1600H, 0x00186d08U);
                            WR1_PROG(REG_1600H, 0x01886e88U);
                            WR1_PROG(REG_1600H, 0x00186908U);

                            r_rsip_func101(bswap_32big(0xc674d7efU),
                                           bswap_32big(0x22406e0fU),
                                           bswap_32big(0xf29f06a5U),
                                           bswap_32big(0xa10e591fU));
                        }
                        else
                        {
                            WR1_PROG(REG_1608H, 0x81010100U);
                            WR1_PROG(REG_1400H, 0x1009000dU);
                            WAIT_STS(REG_1404H, 30, 0);
                            WR1_PROG(REG_143CH, 0x00001800U);
                            WR1_PROG(REG_1400H, 0x10c90005U);
                            WAIT_STS(REG_1404H, 30, 0);
                            WR1_PROG(REG_143CH, 0x00001800U);

                            r_rsip_func101(bswap_32big(0xe22aac5fU),
                                           bswap_32big(0xcabf4d08U),
                                           bswap_32big(0x0293845bU),
                                           bswap_32big(0x56efdb26U));
                        }

                        WR1_PROG(REG_1458H, 0x00000000U);
                    }

                    WR1_PROG(REG_1458H, 0x00000000U);

                    WR1_PROG(REG_1600H, 0x00000863U);
                    WR1_PROG(REG_1600H, 0x3c002857U);
                    WR1_PROG(REG_1600H, 0x20002c60U);
                    WR1_PROG(REG_1600H, 0x3c002b02U);
                    WR1_PROG(REG_1600H, 0x20002c60U);
                    WR1_PROG(REG_1600H, 0x38000c63U);
                    WR1_PROG(REG_1608H, 0x00000080U);
                    WR1_PROG(REG_143CH, 0x00260000U);

                    r_rsip_func100(bswap_32big(0xad22ce31U),
                                   bswap_32big(0xa50828b3U),
                                   bswap_32big(0x12e5326bU),
                                   bswap_32big(0xff1b0daeU));
                    WR1_PROG(REG_143CH, 0x00400000U);

                    if (CHCK_STS(REG_143CH, 22, 1))
                    {
                        WR1_PROG(REG_1608H, 0x81010100U);
                        WR1_PROG(REG_1400H, 0x1009000dU);
                        WAIT_STS(REG_1404H, 30, 0);
                        WR1_PROG(REG_143CH, 0x00001800U);
                        WR1_PROG(REG_1400H, 0x10c90005U);
                        WAIT_STS(REG_1404H, 30, 0);
                        WR1_PROG(REG_143CH, 0x00001800U);

                        r_rsip_func101(bswap_32big(0xd8065143U),
                                       bswap_32big(0x67c1794dU),
                                       bswap_32big(0x965c3d6aU),
                                       bswap_32big(0x8e11a4e9U));
                    }

                    WR1_PROG(REG_1600H, 0x00000863U);
                    WR1_PROG(REG_1600H, 0x3c0028c2U);
                    WR1_PROG(REG_1600H, 0x20002c60U);
                    WR1_PROG(REG_1600H, 0x38000c63U);
                    WR1_PROG(REG_1608H, 0x00000080U);
                    WR1_PROG(REG_143CH, 0x00260000U);

                    r_rsip_func100(bswap_32big(0x742d6fe5U),
                                   bswap_32big(0x3b79cafcU),
                                   bswap_32big(0x17264b0aU),
                                   bswap_32big(0x06bf85fbU));
                    WR1_PROG(REG_143CH, 0x00400000U);

                    if (CHCK_STS(REG_143CH, 22, 1))
                    {
                        WR1_PROG(REG_1608H, 0x81010100U);
                        WR1_PROG(REG_1400H, 0x1009000dU);
                        WAIT_STS(REG_1404H, 30, 0);
                        WR1_PROG(REG_143CH, 0x00001800U);
                        WR1_PROG(REG_1400H, 0x11490005U);
                        WAIT_STS(REG_1404H, 30, 0);
                        WR1_PROG(REG_143CH, 0x00001800U);

                        r_rsip_func101(bswap_32big(0xd4cd4943U),
                                       bswap_32big(0x3a839d96U),
                                       bswap_32big(0x6f558885U),
                                       bswap_32big(0x456b3c1bU));
                    }

                    WR1_PROG(REG_1600H, 0x00002c40U);

                    r_rsip_func101(bswap_32big(0xdca6f4f8U),
                                   bswap_32big(0xa26290cfU),
                                   bswap_32big(0x1e252c82U),
                                   bswap_32big(0x825d8dd7U));
                }

                WR1_PROG(REG_1600H, 0x38008820U);
                WR1_PROG(REG_1600H, 0x00000004U);
                WR1_PROG(REG_1608H, 0x00000080U);
                WR1_PROG(REG_143CH, 0x00260000U);
                WR1_PROG(REG_143CH, 0x00402000U);
                WR1_PROG(REG_1458H, 0x00000000U);

                WR1_PROG(REG_1600H, 0x00002ca0U);

                r_rsip_func101(bswap_32big(0x7e51fdb2U),
                               bswap_32big(0xe1ac117dU),
                               bswap_32big(0x26da5907U),
                               bswap_32big(0x8461c2c5U));
            }

            WR1_PROG(REG_1600H, 0x38000805U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);
            WR1_PROG(REG_143CH, 0x00402000U);
            WR1_PROG(REG_1458H, 0x00000000U);

            r_rsip_func100(bswap_32big(0x42ad859cU), bswap_32big(0xd7099bb7U), bswap_32big(0x3a32a82aU),
                           bswap_32big(0xfbfd699eU));
            WR1_PROG(REG_1600H, 0x00003416U);
            WR1_PROG(REG_1600H, 0x00008c00U);
            WR1_PROG(REG_1600H, 0x00000003U);

            WR1_PROG(REG_1600H, 0x0000b440U);
            WR1_PROG(REG_1600H, 0x00000004U);
            WR1_PROG(REG_1600H, 0x00002840U);
            WR1_PROG(REG_1600H, 0x00008c40U);
            WR1_PROG(REG_1600H, 0x00000003U);

            WR1_PROG(REG_1608H, 0x81010040U);
            WR1_PROG(REG_1408H, 0x00005006U);
            WAIT_STS(REG_1408H, 30, 1);
            RD1_ADDR(REG_1420H, &S_RAM[0]);
            S_RAM[0] = bswap_32big(S_RAM[0]);

            WR1_PROG(REG_1600H, 0x000008a5U);

            WR1_PROG(REG_1600H, 0x00000863U);

            for (iLoop = 0U; iLoop < S_RAM[0]; iLoop++)
            {
                WR1_PROG(REG_1608H, 0x81010060U);
                WR1_PROG(REG_1400H, 0x11490005U);
                WAIT_STS(REG_1404H, 30, 0);
                WR1_PROG(REG_143CH, 0x00001800U);
                WR1_PROG(REG_1400H, 0x1009000dU);
                WAIT_STS(REG_1404H, 30, 0);
                WR1_PROG(REG_143CH, 0x00001800U);

                WR1_PROG(REG_1600H, 0x00086c63U);

                WR1_PROG(REG_1600H, 0x00002ca0U);

                r_rsip_func101(bswap_32big(0x4690b214U),
                               bswap_32big(0xc8131e4fU),
                               bswap_32big(0x3b8eeb54U),
                               bswap_32big(0x36d6c04dU));
            }

            WR1_PROG(REG_1458H, 0x00000000U);

            WR1_PROG(REG_1600H, 0x38000845U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);
            WR1_PROG(REG_143CH, 0x00402000U);
            WR1_PROG(REG_1458H, 0x00000000U);

            WAIT_STS(REG_2030H, 4, 1);

            WR1_PROG(REG_1600H, 0x00000800U);

            WR1_PROG(REG_1600H, 0x38000c84U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);

            WR1_PROG(REG_1600H, 0x38008880U);
            WR1_PROG(REG_1600H, 0x00000002U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);

            r_rsip_func100(bswap_32big(0x054318f1U), bswap_32big(0xe1667fc6U), bswap_32big(0x8a7ecfc2U),
                           bswap_32big(0x762e6989U));
            WR1_PROG(REG_143CH, 0x00400000U);

            if (CHCK_STS(REG_143CH, 22, 1))
            {
                WR1_PROG(REG_1600H, 0x00003200U);

                r_rsip_func101(bswap_32big(0x1f2bfa65U),
                               bswap_32big(0x95a59903U),
                               bswap_32big(0x37c9b0b6U),
                               bswap_32big(0xc04a560aU));
            }

            r_rsip_func100(bswap_32big(0xa9c81b0bU), bswap_32big(0x77c18a3aU), bswap_32big(0xc494f4e3U),
                           bswap_32big(0xb88ecfc1U));
            WR1_PROG(REG_1608H, 0x81010200U);
            WR1_PROG(REG_1408H, 0x00005006U);
            WAIT_STS(REG_1408H, 30, 1);
            RD1_ADDR(REG_1420H, &S_RAM[0]);
            S_RAM[0] = bswap_32big(S_RAM[0]);

            WR1_PROG(REG_1600H, 0x000008a5U);

            WR1_PROG(REG_1600H, 0x0000b7e0U);
            WR1_PROG(REG_1600H, 0x208e11f0U);

            for (iLoop = 0U; iLoop < S_RAM[0]; iLoop++)
            {
                WR1_PROG(REG_1608H, 0x80010020U);
                WR1_PROG(REG_1400H, 0x03450005U);
                WAIT_STS(REG_1404H, 30, 0);
                WR1_PROG(REG_143CH, 0x00001800U);

                WR1_PROG(REG_1600H, 0x00003840U);

                WR1_PROG(REG_1600H, 0x08002822U);
                WR1_PROG(REG_1600H, 0x20000bffU);

                WR1_PROG(REG_1600H, 0x0000a400U);
                WR1_PROG(REG_1600H, 0x00000004U);

                WR1_PROG(REG_1600H, 0x00002ca0U);

                r_rsip_func101(bswap_32big(0x951208c1U),
                               bswap_32big(0x9beabaf8U),
                               bswap_32big(0xb1b7972cU),
                               bswap_32big(0xfc554aa9U));
            }

            WR1_PROG(REG_1458H, 0x00000000U);

            WR1_PROG(REG_1600H, 0x38000a05U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);
            WR1_PROG(REG_143CH, 0x00402000U);
            WR1_PROG(REG_1458H, 0x00000000U);

            WR1_PROG(REG_1600H, 0x38000c84U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);

            WR1_PROG(REG_1600H, 0x38008880U);
            WR1_PROG(REG_1600H, 0x00000002U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00260000U);

            r_rsip_func100(bswap_32big(0x1fb9702eU), bswap_32big(0x652a692eU), bswap_32big(0xdc0e4c52U),
                           bswap_32big(0xfdad16faU));
            WR1_PROG(REG_143CH, 0x00400000U);

            if (CHCK_STS(REG_143CH, 22, 1))
            {
                WR1_PROG(REG_1600H, 0x00003840U);

                WR1_PROG(REG_1600H, 0x38000c42U);
                WR1_PROG(REG_1600H, 0x20000bffU);

                r_rsip_func101(bswap_32big(0xa77c6bdeU),
                               bswap_32big(0xe5fff345U),
                               bswap_32big(0xaff87867U),
                               bswap_32big(0x2646d99bU));
            }

            WR1_PROG(REG_1600H, 0x38008be0U);
            WR1_PROG(REG_1600H, 0x208e11f0U);
            WR1_PROG(REG_1608H, 0x00000080U);
            WR1_PROG(REG_143CH, 0x00A60000U);

            r_rsip_func100(bswap_32big(0xb408ff10U), bswap_32big(0xc90828d2U), bswap_32big(0x78a0d6efU),
                           bswap_32big(0x470528f3U));
            WR1_PROG(REG_143CH, 0x00400000U);

            if (CHCK_STS(REG_143CH, 22, 1))
            {
                r_rsip_func102(bswap_32big(0xc14fff5cU),
                               bswap_32big(0x965aacfaU),
                               bswap_32big(0x84666b2dU),
                               bswap_32big(0x8c2463bbU));
                WR1_PROG(REG_14BCH, 0x00000040U);
                WAIT_STS(REG_142CH, 12, 0);

                return RSIP_RET_FAIL;
            }
            else
            {
                WR1_PROG(REG_1600H, 0x380089e0U);
                WR1_PROG(REG_1600H, 0x00000005U);
                WR1_PROG(REG_1608H, 0x00000080U);
                WR1_PROG(REG_143CH, 0x00260000U);

                r_rsip_func100(bswap_32big(0xeaacafecU),
                               bswap_32big(0xcd1900f5U),
                               bswap_32big(0x36eeb236U),
                               bswap_32big(0x0f2bd19bU));
                WR1_PROG(REG_143CH, 0x00400000U);

                if (CHCK_STS(REG_143CH, 22, 1))
                {
                    WR1_PROG(REG_1600H, 0x00000800U);

                    WR1_PROG(REG_1608H, 0x80a80000U);

                    WR1_PROG(REG_1400H, 0x13400039U);
                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1404H, 0x10000000U);
                    WR1_PROG(REG_1400H, 0x13430109U);
                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1400H, 0x13400039U);
                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1400H, 0x13430109U);
                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1404H, 0x10000000U);
                    WR1_PROG(REG_1608H, 0x81a80000U);
                    WR1_PROG(REG_1400H, 0x00c900a1U);
                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    r_rsip_func101(bswap_32big(0x7cd544faU),
                                   bswap_32big(0x0246ad23U),
                                   bswap_32big(0x5a00143bU),
                                   bswap_32big(0x605649a7U));
                }

                WR1_PROG(REG_1444H, 0x000000a7U);
                WR1_PROG(REG_1608H, 0x800103a0U);
                WAIT_STS(REG_1444H, 31, 1);
                WR1_PROG(REG_1420H, bswap_32big(0x000000e1U));
                WR1_PROG(REG_1458H, 0x00000000U);

                r_rsip_func101(bswap_32big(0x9b162d8eU),
                               bswap_32big(0xfe16eca8U),
                               bswap_32big(0x8ac7381dU),
                               bswap_32big(0x0a60e955U));
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
                WR1_PROG(REG_1420H, bswap_32big(0x0000e102U));
                WR1_PROG(REG_1458H, 0x00000000U);

                r_rsip_func101(bswap_32big(0xeb6f5947U),
                               bswap_32big(0x151c1644U),
                               bswap_32big(0x81c8cb11U),
                               bswap_32big(0x86206473U));
                r_rsip_func043();

                WR1_PROG(REG_1600H, 0x000034eeU);

                WR1_PROG(REG_1444H, 0x000000a7U);
                WR1_PROG(REG_1608H, 0x800103a0U);
                WAIT_STS(REG_1444H, 31, 1);
                WR1_PROG(REG_1420H, bswap_32big(0x0000e102U));
                WR1_PROG(REG_1458H, 0x00000000U);

                r_rsip_func101(bswap_32big(0xa77ef16fU),
                               bswap_32big(0xea9e995fU),
                               bswap_32big(0x91934b4aU),
                               bswap_32big(0x84159788U));
                r_rsip_func044();

                r_rsip_func100(bswap_32big(0xabe3b7a4U),
                               bswap_32big(0x9c77a9a4U),
                               bswap_32big(0xc8e6bf51U),
                               bswap_32big(0xbd315ed7U));
                WR1_PROG(REG_1444H, 0x000000a2U);
                WR1_PROG(REG_1A24H, 0x08000044U);
                WAIT_STS(REG_1444H, 31, 1);
                WR1_PROG(REG_1420H, bswap_32big(0x00000000U));

                WR1_PROG(REG_1444H, 0x000000a2U);
                WR1_PROG(REG_1A24H, 0x08000054U);
                WAIT_STS(REG_1444H, 31, 1);
                WR1_PROG(REG_1420H, bswap_32big(0x00000000U));

                WR1_PROG(REG_1608H, 0x81010360U);
                WR1_PROG(REG_1408H, 0x00005006U);
                WAIT_STS(REG_1408H, 30, 1);
                RD1_ADDR(REG_1420H, &S_RAM[0]);
                S_RAM[0] = bswap_32big(S_RAM[0]);

                WR1_PROG(REG_1600H, 0x00000884U);
                WR1_PROG(REG_1600H, 0x000008a5U);

                WR1_PROG(REG_1404H, 0x10000000U);
                for (iLoop = 0U; iLoop < S_RAM[0]; )
                {
                    WR1_PROG(REG_1600H, 0x342028bbU);
                    WR1_PROG(REG_1600H, 0x2000d080U);
                    WR1_PROG(REG_1600H, 0x00007c04U);
                    WR1_PROG(REG_143CH, 0x00602000U);
                    WR1_PROG(REG_1458H, 0x00000000U);

                    r_rsip_func100(bswap_32big(0x016b8e8cU),
                                   bswap_32big(0x05e9e3e6U),
                                   bswap_32big(0x7009f85dU),
                                   bswap_32big(0x4f281387U));
                    WR1_PROG(REG_1A2CH, 0x40000000U);
                    WR1_PROG(REG_1A24H, 0xe8008105U);
                    WR1_PROG(REG_1400H, 0x00830011U);
                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1408H, 0x00002012U);
                    WAIT_STS(REG_1408H, 30, 1);
                    RD4_ADDR(REG_1420H, &OutData_KeyIndex[1 + iLoop]);

                    WR1_PROG(REG_1600H, 0x0000a4a0U);
                    WR1_PROG(REG_1600H, 0x00000004U);

                    r_rsip_func101(bswap_32big(0xff204b58U),
                                   bswap_32big(0x42605f17U),
                                   bswap_32big(0x5c4fc156U),
                                   bswap_32big(0xf2a74e8fU));
                    iLoop = iLoop + 4U;
                }

                WR1_PROG(REG_1458H, 0x00000000U);

                WR1_PROG(REG_1600H, 0x380008bbU);
                WR1_PROG(REG_1608H, 0x00000080U);
                WR1_PROG(REG_143CH, 0x00260000U);
                WR1_PROG(REG_143CH, 0x00402000U);
                WR1_PROG(REG_1458H, 0x00000000U);

                WR1_PROG(REG_1600H, 0x3420a9e0U);
                WR1_PROG(REG_1600H, 0x00000003U);
                WR1_PROG(REG_1608H, 0x00000080U);
                WR1_PROG(REG_143CH, 0x00260000U);

                r_rsip_func100(bswap_32big(0x4ed6c8c5U),
                               bswap_32big(0xaf8eb071U),
                               bswap_32big(0xd4ca9cadU),
                               bswap_32big(0xe053a9c6U));
                WR1_PROG(REG_143CH, 0x00400000U);

                if (CHCK_STS(REG_143CH, 22, 1))
                {
                    r_rsip_func100(bswap_32big(0x1db47114U),
                                   bswap_32big(0x32808e97U),
                                   bswap_32big(0xe150c690U),
                                   bswap_32big(0x55923281U));
                    WR1_PROG(REG_1A2CH, 0x40000000U);
                    WR1_PROG(REG_1A24H, 0xe8008104U);
                    WR1_PROG(REG_1608H, 0x81010280U);
                    WR1_PROG(REG_1400H, 0x00890005U);
                    WAIT_STS(REG_1404H, 30, 0);
                    WR1_PROG(REG_143CH, 0x00001800U);

                    WR1_PROG(REG_1408H, 0x00002012U);
                    WAIT_STS(REG_1408H, 30, 1);
                    RD4_ADDR(REG_1420H, &OutData_KeyIndex[1 + iLoop]);
                    iLoop = iLoop + 4;

                    r_rsip_func101(bswap_32big(0xb51a835dU),
                                   bswap_32big(0x70ac051fU),
                                   bswap_32big(0x0e356288U),
                                   bswap_32big(0x54221436U));
                }

                r_rsip_func100(bswap_32big(0xed394d43U),
                               bswap_32big(0x93b1089fU),
                               bswap_32big(0x596f1803U),
                               bswap_32big(0xbcf31b70U));
                WR1_PROG(REG_1444H, 0x000000a2U);
                WR1_PROG(REG_1A24H, 0x0c000104U);
                WAIT_STS(REG_1444H, 31, 1);
                WR1_PROG(REG_1420H, bswap_32big(0x00000000U));

                WR1_PROG(REG_1A2CH, 0x40000000U);
                WR1_PROG(REG_1A24H, 0x09108105U);
                WR1_PROG(REG_1400H, 0x00820011U);
                WAIT_STS(REG_1404H, 30, 0);
                WR1_PROG(REG_143CH, 0x00001800U);

                WR1_PROG(REG_1408H, 0x00002012U);
                WAIT_STS(REG_1408H, 30, 1);
                RD4_ADDR(REG_1420H, &OutData_KeyIndex[1 + iLoop]);

                r_rsip_func100(bswap_32big(0xe2ecd172U),
                               bswap_32big(0xd32b3232U),
                               bswap_32big(0x43f1ec7aU),
                               bswap_32big(0xfa8d9e7cU));
                WR1_PROG(REG_1608H, 0x81010000U);
                WR1_PROG(REG_1408H, 0x00005006U);
                WAIT_STS(REG_1408H, 30, 1);
                RD1_ADDR(REG_1420H, &OutData_KeyIndex[0]);

                r_rsip_func102(bswap_32big(0x6f1dabfeU),
                               bswap_32big(0xead6bdbcU),
                               bswap_32big(0x656e7494U),
                               bswap_32big(0x4a1d48b8U));
                WR1_PROG(REG_14BCH, 0x00000040U);
                WAIT_STS(REG_142CH, 12, 0);

                return RSIP_RET_PASS;
            }
        }
    }
}
