/*
 * QTest for the SMN SPI controller in front of the flash
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "amd-psp-test.h"

#define SMN_CTRL 0x03220000
#define SMN_WINDOW0 0x01000000          /* PSP address of slot 0 */
#define SPI_SMN_SLOT 0x2d               /* SMN 0x2d00000 */
#define SPI (SMN_WINDOW0 + 0xc4000)     /* SMN 0x2dc4000 */
#define SPI_OPCODE (SPI + 0x45)
#define SPI_TRIGGER (SPI + 0x47)
#define SPI_TXCOUNT (SPI + 0x48)
#define SPI_RXCOUNT (SPI + 0x4b)
#define SPI_STATUS (SPI + 0x4c)
#define SPI_FIFO (SPI + 0x80)

static void spi_cmd(QTestState *qts, uint8_t op, const uint8_t *tx,
                    unsigned ntx, unsigned nrx)
{
    for (unsigned i = 0; i < ntx; i++) {
        qtest_writeb(qts, SPI_FIFO + i, tx[i]);
    }
    qtest_writeb(qts, SPI_OPCODE, op);
    qtest_writeb(qts, SPI_TXCOUNT, ntx);
    qtest_writeb(qts, SPI_RXCOUNT, nrx);
    qtest_writeb(qts, SPI_TRIGGER, 0x80);
    g_assert_cmphex(qtest_readb(qts, SPI_TRIGGER), ==, 0);
}

static uint8_t spi_status(QTestState *qts)
{
    spi_cmd(qts, 0x05, NULL, 0, 1);
    return qtest_readb(qts, SPI_FIFO);
}

/* Write enable latch, erase, program and read back through the FIFO */
static void test_spi_flash_ops(void)
{
    static const uint8_t addr[] = { 0x00, 0x01, 0x00 };
    uint8_t prog[] = { 0x00, 0x01, 0x00, 0xa5, 0x5a };
    QTestState *qts = psp_start(PSP_ZEN2);

    if (!qts) {
        return;
    }
    qtest_writew(qts, SMN_CTRL, SPI_SMN_SLOT);
    g_assert_cmphex(qtest_readl(qts, SPI_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(spi_status(qts), ==, 0);
    spi_cmd(qts, 0x06, NULL, 0, 0);
    g_assert_cmphex(spi_status(qts), ==, 0x02);

    /* Erase 4 KiB at 0x100: the sector reads back as 0xff */
    spi_cmd(qts, 0x20, addr, 3, 0);
    g_assert_cmphex(spi_status(qts), ==, 0);
    spi_cmd(qts, 0x03, addr, 3, 2);
    g_assert_cmphex(qtest_readw(qts, SPI_FIFO + 3), ==, 0xffff);

    /* Program without write enable changes nothing */
    spi_cmd(qts, 0x02, prog, 5, 0);
    spi_cmd(qts, 0x03, addr, 3, 2);
    g_assert_cmphex(qtest_readw(qts, SPI_FIFO + 3), ==, 0xffff);

    spi_cmd(qts, 0x06, NULL, 0, 0);
    spi_cmd(qts, 0x02, prog, 5, 0);
    g_assert_cmphex(spi_status(qts), ==, 0);
    spi_cmd(qts, 0x03, addr, 3, 2);
    g_assert_cmphex(qtest_readb(qts, SPI_FIFO + 3), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SPI_FIFO + 4), ==, 0x5a);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    psp_test_init(&argc, &argv);
    qtest_add_func("/amd-psp/spi/flash-ops", test_spi_flash_ops);
    return psp_test_run();
}
