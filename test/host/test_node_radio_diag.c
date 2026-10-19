/* SPDX-License-Identifier: GPL-3.0-or-later */
// Run the production node callback with the real driver and mocked registers.
#include "uwb_arduswarm_anchor.c"
#include <assert.h>
uint32_t test_tick;
bool cfgReadU32(ConfigField field, uint32_t *value) { (void)field; (void)value; return false; }
bool cfgReadU8(ConfigField field, uint8_t *value) { (void)field; (void)value; return false; }
static uint8_t regs[64][1024];
static unsigned offset(const uint8_t *h, size_t n)
{
    return n == 1 ? 0 : (h[1] & 0x7fU) + (n == 3 ? (unsigned)h[2] * 128U : 0);
}
static void read_spi(dwDevice_t *dev, const void *header, size_t hlen, void *data, size_t len)
{
    (void)dev; const uint8_t *h = header;
    assert(offset(h, hlen) + len <= 1024);
    memcpy(data, &regs[h[0] & 0x3f][offset(h, hlen)], len);
}
static void write_spi(dwDevice_t *dev, const void *header, size_t hlen, const void *data, size_t len)
{
    (void)dev; const uint8_t *h = header;
    assert(offset(h, hlen) + len <= 1024);
    memcpy(&regs[h[0] & 0x3f][offset(h, hlen)], data, len);
}
static void receive(dwDevice_t *dev, const uint8_t *frame, unsigned len, uint64_t stamp)
{
    dev->deviceMode = RX_MODE;
    memcpy(regs[RX_BUFFER], frame, len);
    twr_put_le(regs[RX_FINFO], (128U << 20) | (len + 2), 4);
    twr_put_le(regs[RX_TIME], stamp, 5);
    twr_put_le(regs[SYS_TIME], stamp + 100000, 5);
    uwbArduSwarmAnchorAlgorithm.onEvent(dev, eventPacketReceived);
}
static void exchange(dwDevice_t *dev, bool diagnostics, uint16_t sequence)
{
    twr_engine tag;
    twr_init(&tag, TWR_TAG, 1, 1, 30);
    const uint64_t t1 = 1000000 + (uint64_t)sequence * 1000000000;
    const uint64_t flight = 500;
    uint8_t frame[TWR_DIAG_MAX_FRAME_SIZE];
    twr_packet decoded;
    twr_diag_rx remote[2], combined[3] = {0};
    bool request, report;
    test_tick += 100;
    assert(twr_start(&tag, TWR_ANCHOR, 8, 0x12345678, sequence, test_tick));
    unsigned len = twr_prepare_tx(&tag, t1, frame);
    assert(twr_sent(&tag, t1));
    if (diagnostics) { len = twr_diag_request(frame, len); }
    receive(dev, frame, len, t1 + flight);
    assert(engine.state == TWR_TX_RESPONSE && radio_diag == diagnostics);
    uint64_t tx = engine.expected_tx;
    twr_put_le(regs[TX_TIME], tx, 5);
    on_event(dev, eventPacketSent);
    assert(engine.state == TWR_WAIT_FINAL);
    unsigned txlen = (unsigned)twr_get_le(regs[TX_FCTRL], 2) & 0x3ff;
    assert(txlen == 18); // standard RESPONSE, includes FCS
    assert(twr_decode(regs[TX_BUFFER], txlen - 2, &decoded));
    // Tag RESPONSE diagnostic uses a separate same-packet snapshot.
    twr_put_le(regs[RX_TIME], tx + flight, 5);
    dwTime_t tag_rx = twr_diag_read(dev, &combined[1]);
    assert(twr_receive(&tag, &decoded, tag_rx.full, test_tick + 4, false) == TWR_SEND);
    len = twr_prepare_tx(&tag, tx + 250000000, frame);
    assert(twr_sent(&tag, tx + 250000000));
    test_tick += 8;
    receive(dev, frame, len, tx + 250000000 + flight);
    assert(engine.state == TWR_TX_REPORT);
    txlen = (unsigned)twr_get_le(regs[TX_FCTRL], 2) & 0x3ff;
    assert(txlen == (diagnostics ? TWR_DIAG_MAX_FRAME_SIZE : 32) + 2);
    assert(twr_diag_decode(regs[TX_BUFFER], txlen - 2, &decoded, &request, &report, remote));
    assert(report == diagnostics && !request);
    assert(twr_receive(&tag, &decoded, engine.expected_tx + flight, test_tick + 4, false) == TWR_COMPLETE);
    assert(tag.distance == engine.distance);
    if (diagnostics) {
        combined[0] = remote[0]; combined[2] = remote[1];
        float raw;
        assert(twr_diag_uncorrected(&tag, combined, &raw));
        assert(fabs(raw - flight * TWR_METERS_PER_TICK) < 1e-6);
    }
    twr_put_le(regs[TX_TIME], engine.expected_tx, 5);
    on_event(dev, eventPacketSent);
    assert(engine.state == TWR_IDLE);
}
int main(void)
{
    dwOps_t ops = {.spiRead = read_spi, .spiWrite = write_spi};
    dwDevice_t dev = {0};
    dwInit(&dev, &ops);
    dev.channel = CHANNEL_2; dev.pulseFrequency = TX_PULSE_FREQ_64MHZ;
    dev.frameCheck = true;
    twr_put_le(regs[RX_FQUAL] + CIR_PWR_SUB, 1000, 2);
    twr_put_le(regs[RX_TIME] + FP_AMPL1_SUB, 50, 2);
    twr_put_le(regs[RX_FQUAL] + FP_AMPL2_SUB, 70, 2);
    twr_put_le(regs[RX_FQUAL] + FP_AMPL3_SUB, 60, 2);
    enabled = true; reply_us = 3000;
    twr_init(&engine, TWR_ANCHOR, 8, 1, 30);
    exchange(&dev, false, 1);
    exchange(&dev, true, 2);
    exchange(&dev, false, 3); // no leaked diagnostic state into next legacy poll
    exchange(&dev, true, 4);
    assert(stats.ranges == 4 && stats.failures == 0);
    puts("Production node callback: legacy/extended exchanges and same-packet raw reconstruction pass");
}
