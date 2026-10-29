/*
 * QTest for the Zen3 SMBus controller and its DDR4 SPD EEPROMs. Register
 * layout and SPD content come from the emulator source and the public JEDEC
 * DDR4 SPD layout, not from firmware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

/* SMN control (include/psp/smn/psp-smn.h): 16 bit slot n at +2n */
#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */
#define SMBUS_SMN_SLOT 0x2d             /* SMN 0x2d00000 */
#define SB (SMN_WINDOW0 + 0x1a00)       /* SMN 0x2d01a00 */
#define SB_HSTS (SB + 0)
#define SB_SLVSTS (SB + 1)
#define SB_CNT (SB + 2)
#define SB_CMD (SB + 3)
#define SB_ADDR (SB + 4)
#define SB_DATA0 (SB + 5)
#define SB_DATA1 (SB + 6)
#define SB_MISC (SB + 8)

#define HSTS_DONE 0x02
#define HSTS_FAILED 0x04
#define CNT_START 0x40
#define CNT_BYTE_DATA 0x08
#define CNT_WORD_DATA 0x0c

#define ADDR_DIMM1 0xa2
#define ADDR_MUX 0xe0
#define ADDR_PAGE0 0x6c
#define ADDR_PAGE1 0x6e

/* JEDEC SPD CRC16: polynomial 0x1021, initial value 0 */
static uint16_t crc16(const uint8_t *p, size_t len)
{
    uint16_t crc = 0;

    while (len--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static QTestState *start(const char *extra)
{
    QTestState *qts = psp_startf(PSP_ZEN3, "%s", extra);

    if (qts) {
        qtest_writew(qts, SMN_CTRL, SMBUS_SMN_SLOT);
    }
    return qts;
}

/* Run a transaction and return HSTS, clearing it again like the ABL */
static uint8_t xfer(QTestState *qts, uint8_t addr, uint8_t cmd, uint8_t cnt)
{
    uint8_t hsts;

    qtest_writeb(qts, SB_HSTS, 0x1f);
    qtest_writeb(qts, SB_ADDR, addr);
    qtest_writeb(qts, SB_CMD, cmd);
    qtest_writeb(qts, SB_CNT, cnt | CNT_START);
    hsts = qtest_readb(qts, SB_HSTS);
    qtest_writeb(qts, SB_HSTS, 0x1f);
    return hsts;
}

static uint8_t spd_byte(QTestState *qts, uint8_t addr, unsigned page,
                        uint8_t off)
{
    g_assert_cmphex(xfer(qts, page ? ADDR_PAGE1 : ADDR_PAGE0, 0,
                         CNT_BYTE_DATA), ==, HSTS_DONE);
    g_assert_cmphex(xfer(qts, addr | 1, off, CNT_BYTE_DATA), ==, HSTS_DONE);
    return qtest_readb(qts, SB_DATA0);
}

static void test_spd_read(void)
{
    QTestState *qts = start("");
    uint8_t spd[128];

    if (!qts) {
        return;
    }
    g_assert_cmphex(spd_byte(qts, ADDR_DIMM1, 0, 2), ==, 0x0c);
    g_assert_cmphex(spd_byte(qts, ADDR_DIMM1, 0, 3), ==, 0x01);
    for (int i = 0; i < sizeof(spd); i++) {
        spd[i] = spd_byte(qts, ADDR_DIMM1, 0, i);
    }
    g_assert_cmphex(spd[126] | (spd[127] << 8), ==, crc16(spd, 126));
    /* 4 Gbit x8 dies, 15 row and 10 column bits: 4 GiB per DIMM */
    g_assert_cmphex(spd[4], ==, 0x84);
    g_assert_cmphex(spd[5], ==, 0x19);
    /* Single rank x8 RDIMM */
    g_assert_cmphex(spd[12], ==, 0x01);
    g_assert_cmphex(spd_byte(qts, ADDR_DIMM1, 0, 131), ==, 0x01);
    for (int i = 0; i < sizeof(spd); i++) {
        spd[i] = spd_byte(qts, ADDR_DIMM1, 0, 128 + i);
    }
    g_assert_cmphex(spd[126] | (spd[127] << 8), ==, crc16(spd, 126));
    /* Page 1: the part number starts at SPD byte 329 */
    g_assert_cmphex(spd_byte(qts, ADDR_DIMM1, 1, 329 - 256), ==, 'Q');
    /* Page 0 again */
    g_assert_cmphex(spd_byte(qts, ADDR_DIMM1, 0, 2), ==, 0x0c);

    /* Word data: bytes 2 and 3 */
    g_assert_cmphex(xfer(qts, ADDR_DIMM1 | 1, 2, CNT_WORD_DATA), ==,
                    HSTS_DONE);
    g_assert_cmphex(qtest_readb(qts, SB_DATA0), ==, 0x0c);
    g_assert_cmphex(qtest_readb(qts, SB_DATA1), ==, 0x01);
    qtest_quit(qts);
}

static void test_nack_and_mux(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    /* Slot 0 is not populated by default */
    g_assert_cmphex(xfer(qts, 0xa0 | 1, 0, CNT_BYTE_DATA), ==, HSTS_FAILED);
    g_assert_cmphex(xfer(qts, 0x10, 0, CNT_BYTE_DATA), ==, HSTS_FAILED);
    g_assert_cmphex(xfer(qts, ADDR_MUX, 0x01, CNT_BYTE_DATA), ==, HSTS_DONE);
    qtest_quit(qts);
}

static void test_status_clear(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    qtest_writeb(qts, SB_ADDR, ADDR_MUX);
    qtest_writeb(qts, SB_CNT, CNT_BYTE_DATA | CNT_START);
    g_assert_cmphex(qtest_readb(qts, SB_HSTS), ==, HSTS_DONE);
    qtest_writeb(qts, SB_HSTS, 0x1f);
    g_assert_cmphex(qtest_readb(qts, SB_HSTS), ==, 0);
    qtest_writeb(qts, SB_ADDR, 0x10);
    qtest_writeb(qts, SB_CNT, CNT_BYTE_DATA | CNT_START);
    g_assert_cmphex(qtest_readb(qts, SB_HSTS), ==, HSTS_FAILED);
    /* Only the written bits clear */
    qtest_writeb(qts, SB_HSTS, HSTS_DONE);
    g_assert_cmphex(qtest_readb(qts, SB_HSTS), ==, HSTS_FAILED);
    qtest_writeb(qts, SB_HSTS, 0x1f);
    g_assert_cmphex(qtest_readb(qts, SB_HSTS), ==, 0);
    qtest_writeb(qts, SB_SLVSTS, 0x3f);
    g_assert_cmphex(qtest_readb(qts, SB_SLVSTS), ==, 0);
    qtest_quit(qts);
}

static void test_misc_bit4(void)
{
    QTestState *qts = start("");

    if (!qts) {
        return;
    }
    qtest_writeb(qts, SB_MISC, 0x10);
    g_assert_cmphex(qtest_readb(qts, SB_MISC) & 0x10, ==, 0x10);
    qtest_quit(qts);
}

static void test_dimm_slots(void)
{
    QTestState *qts = start("-global driver=amd_psp.smn.smbus,"
                            "property=dimm-slots,value=3");

    if (!qts) {
        return;
    }
    g_assert_cmphex(spd_byte(qts, 0xa0, 0, 2), ==, 0x0c);
    g_assert_cmphex(spd_byte(qts, 0xa2, 0, 2), ==, 0x0c);
    qtest_quit(qts);
}

static void test_spd_file(void)
{
    g_autofree char *path = NULL;
    uint8_t img[512] = { 0 };
    QTestState *qts;
    int fd = g_file_open_tmp("psp-spd-XXXXXX", &path, NULL);
    g_autofree char *extra = NULL;

    g_assert_cmpint(fd, >=, 0);
    img[2] = 0x0c;
    img[100] = 0x5a;
    g_assert_cmpint(write(fd, img, sizeof(img)), ==, sizeof(img));
    close(fd);
    extra = g_strdup_printf("-global driver=amd_psp.smn.smbus,"
                            "property=spd-file,value=%s", path);
    qts = start(extra);
    if (qts) {
        g_assert_cmphex(spd_byte(qts, ADDR_DIMM1, 0, 100), ==, 0x5a);
        qtest_quit(qts);
    }
    unlink(path);
}

/* The controller does not exist on Zen2 */
static void test_zen2_absent(void)
{
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    qtest_writew(qts, SMN_CTRL, SMBUS_SMN_SLOT);
    qtest_writeb(qts, SB_ADDR, ADDR_MUX);
    qtest_writeb(qts, SB_CNT, CNT_BYTE_DATA | CNT_START);
    g_assert_cmphex(qtest_readb(qts, SB_HSTS), ==, 0);
    g_assert_cmphex(qtest_readb(qts, SB_MISC), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);

    qtest_add_func("/amd-psp/smbus/spd-read", test_spd_read);
    qtest_add_func("/amd-psp/smbus/nack-and-mux", test_nack_and_mux);
    qtest_add_func("/amd-psp/smbus/status-clear", test_status_clear);
    qtest_add_func("/amd-psp/smbus/misc-bit4", test_misc_bit4);
    qtest_add_func("/amd-psp/smbus/dimm-slots", test_dimm_slots);
    qtest_add_func("/amd-psp/smbus/spd-file", test_spd_file);
    qtest_add_func("/amd-psp/smbus/zen2-absent", test_zen2_absent);

    return psp_test_run();
}
