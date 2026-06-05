/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/misc/bh_fwtable.h>

#define FWTABLE_DEV DEVICE_DT_GET(DT_NODELABEL(fwtable))

ZTEST(bh_fwtable_sim, test_device_ready)
{
    const struct device *dev = FWTABLE_DEV;

    zassert_not_null(dev, "fwtable device pointer is NULL");
    zassert_true(device_is_ready(dev),
                 "fwtable device not ready — init likely failed "
                 "(check that flash.bin contains boardcfg, cmfwcfg, flshinfo entries)");
}

ZTEST(bh_fwtable_sim, test_read_only_loaded)
{
    const struct device *dev = FWTABLE_DEV;
    const ReadOnly *ro = tt_bh_fwtable_get_read_only_table(dev);

    zassert_not_null(ro, "read_only table pointer is NULL");
    zassert_equal(ro->vendor_id, 0x1e52,
                  "expected vendor_id=0x1e52 (Tenstorrent), got 0x%x", ro->vendor_id);
    /* board_id non-zero indicates the protobuf successfully deserialized */
    zassert_not_equal(ro->board_id, 0, "board_id is zero — did boardcfg actually load?");

    TC_PRINT("board_id  = 0x%llx\n", (unsigned long long)ro->board_id);
    TC_PRINT("vendor_id = 0x%x\n", ro->vendor_id);
}

ZTEST(bh_fwtable_sim, test_fw_table_loaded)
{
    const struct device *dev = FWTABLE_DEV;
    const FwTable *fw = tt_bh_fwtable_get_fw_table(dev);
// read f_max and assert
    zassert_not_null(fw, "fw_table pointer is NULL");
    TC_PRINT("fw_bundle_version = 0x%x\n", fw->fw_bundle_version);
}

ZTEST(bh_fwtable_sim, test_flash_info_loaded)
{
    const struct device *dev = FWTABLE_DEV;
    const FlashInfoTable *fi = tt_bh_fwtable_get_flash_info_table(dev);

    zassert_not_null(fi, "flash_info pointer is NULL");
    TC_PRINT("reprogrammed_count = %u\n", fi->reprogrammed_count);
    TC_PRINT("tt_flash_version   = 0x%x\n", fi->tt_flash_version);
}

ZTEST_SUITE(bh_fwtable_sim, NULL, NULL, NULL, NULL, NULL);