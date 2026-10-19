/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uwb.h"
#include "twr_protocol.h"
#include "dw1000_radio_diag.h"
#include "task.h"
#include "led.h"
#include <stdio.h>

static twr_engine engine;
static bool enabled;
static bool radio_diag;
static twr_diag_rx radio_rx[2];
static uint32_t reply_us, network, channel, antenna_delay, timeout_ms;
static struct {
    uint32_t ranges, failures, session;
    uint16_t sequence;
    uint8_t tag;
    float range;
} stats;

static uint32_t setting(ConfigField field, uint32_t fallback)
{
    cfgReadU32(field, &fallback);
    return fallback;
}

static void listen(dwDevice_t *dev)
{
    dwIdle(dev);
    dwNewReceive(dev);
    dwSetDefaults(dev);
    dwStartReceive(dev);
}

static void fail(dwDevice_t *dev)
{
    stats.failures++;
    twr_abort(&engine);
    listen(dev);
}

static void transmit(dwDevice_t *dev)
{
    dwNewTransmit(dev);
    dwTime_t delay = {.full = (uint64_t)((double)reply_us / TIME_RES)};
    dwTime_t scheduled = dwSetDelay(dev, &delay);
    // libdw1000 reads two additional bytes when FCS is enabled.
    uint8_t frame[TWR_DIAG_MAX_FRAME_SIZE + 2] = {0};
    unsigned len = twr_prepare_tx(&engine, scheduled.full, frame);
    if (radio_diag) {
        len = twr_diag_report(frame, len, radio_rx);
    }
    if (len == 0) {
        fail(dev);
        return;
    }
    dwSetData(dev, frame, len);
    dwWaitForResponse(dev, engine.state != TWR_TX_REPORT);
    dwStartTransmit(dev);
}

static void init(uwbConfig_t *config, dwDevice_t *dev)
{
    network = setting(cfgArduSwarmNetwork, 1);
    channel = setting(cfgArduSwarmChannel, 2);
    antenna_delay = setting(cfgArduSwarmAntennaDelay, 16384);
    reply_us = setting(cfgArduSwarmReplyUs, 3000);
    timeout_ms = setting(cfgArduSwarmTimeoutMs, 30);
    enabled = config->address[0] >= 1 && config->address[0] <= 8 &&
              network >= 1 && network <= 32767 && antenna_delay <= 32767 &&
              ((channel >= 1 && channel <= 5) || channel == 7) &&
              reply_us >= 1500 && reply_us <= 20000 && timeout_ms <= 200 &&
              timeout_ms * 1000 >= 4 * reply_us + 3000;
    if (!enabled) {
        printf("ArduSwarm: invalid ID (1..8) or timing/configuration; radio disabled\r\n");
        dwIdle(dev);
        return;
    }
    twr_init(&engine, TWR_ANCHOR, config->address[0], network, timeout_ms);
    // Override only this algorithm's RF profile and hardware antenna-delay convention.
    dwNewConfiguration(dev);
    dwSetDefaults(dev);
    dwEnableMode(dev, MODE_SHORTDATA_FAST_ACCURACY);
    dwSetChannel(dev, channel);
    dwSetPreambleCode(dev, PREAMBLE_CODE_64MHZ_9);
    dwUseSmartPower(dev, true);
    dwTime_t delay = {.full = antenna_delay};
    dwSetAntenaDelay(dev, delay);
    dwCommitConfiguration(dev);
    dwInterruptOnSent(dev, true);
    dwInterruptOnReceived(dev, true);
    dwInterruptOnReceiveFailed(dev, true);
    dwInterruptOnReceiveTimeout(dev, true);
    dwWriteSystemEventMaskRegister(dev);
    listen(dev);
    ledOn(ledMode);
}

static uint32_t on_event(dwDevice_t *dev, uwbEvent_t event)
{
    if (!enabled) {
        return 100;
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (twr_expired(&engine, now)) {
        fail(dev);
        return 1;
    }
    if (event == eventPacketSent) {
        dwTime_t tx = {.full = 0};
        dwGetTransmitTimestamp(dev, &tx);
        if (!twr_sent(&engine, tx.full)) {
            fail(dev);
        } else if (engine.state == TWR_IDLE) {
            listen(dev);
        }
    } else if (event == eventPacketReceived) {
        uint8_t frame[TWR_DIAG_MAX_FRAME_SIZE];
        twr_packet packet;
        dwTime_t rx = {.full = 0};

        unsigned len = dwGetDataLength(dev);
        enum twr_result result = TWR_IGNORED;
        if (len <= sizeof(frame)) {
            dwGetData(dev, frame, len);
            bool requested = false, report = false;
            twr_diag_rx unused[2];
            if (twr_diag_decode(frame, len, &packet, &requested, &report, unused) && !report) {
                const bool addressed = packet.network == engine.network && packet.dst_role == engine.role &&
                    packet.dst_id == engine.id && packet.src_role == TWR_TAG;
                const bool poll = addressed && requested && engine.state == TWR_IDLE && packet.type == TWR_POLL;
                const bool final = addressed && radio_diag && engine.state == TWR_WAIT_FINAL &&
                    packet.type == TWR_FINAL && packet.src_id == engine.remote_id &&
                    packet.session == engine.session && packet.sequence == engine.sequence;
                twr_diag_rx captured = {0};
                if (poll || final) {
                    rx = twr_diag_read(dev, &captured);
                } else {
                    dwGetReceiveTimestamp(dev, &rx);
                }
                result = twr_receive(&engine, &packet, rx.full, now, true);
                if (result == TWR_SEND && packet.type == TWR_POLL) {
                    radio_diag = requested;
                    memset(radio_rx, 0, sizeof(radio_rx));
                    radio_rx[0] = captured;
                } else if (result == TWR_SEND && packet.type == TWR_FINAL && radio_diag) {
                    radio_rx[1] = captured;
                }
                if (engine.new_range) {
                    ledBlink(ledRanging, true);
                    stats.ranges++;
                    stats.range = engine.distance;
                    stats.tag = engine.remote_id;
                    stats.session = engine.session;
                    stats.sequence = engine.sequence;
                }
            }
        }
        if (result == TWR_SEND) {
            transmit(dev);
        } else if (result == TWR_FAILED) {
            fail(dev);
        } else if (!twr_tx_pending(&engine)) {
            listen(dev);
        }
    } else if (event == eventReceiveTimeout || event == eventReceiveFailed) {
        if (!twr_tx_pending(&engine)) {
            listen(dev);
        }
    }
    // Periodic wakeups enforce the original deadline even without radio events.
    return 1;
}

void uwbArduSwarmPrintStatus(void)
{
    uint8_t saved_id = 0;
    uint8_t saved_mode = MODE_ANCHOR;
    cfgReadU8(cfgAddress, &saved_id);
    cfgReadU8(cfgMode, &saved_mode);
    printf("ArduSwarm radio diagnostics supported=1\r\n");
    printf("Stored settings (defaults for unset fields):\r\n");
    printf("id=%u mode=%u net=%lu channel=%lu ant=%lu reply=%lu timeout=%lu\r\n",
           saved_id, saved_mode,
           (unsigned long)setting(cfgArduSwarmNetwork, 1),
           (unsigned long)setting(cfgArduSwarmChannel, 2),
           (unsigned long)setting(cfgArduSwarmAntennaDelay, 16384),
           (unsigned long)setting(cfgArduSwarmReplyUs, 3000),
           (unsigned long)setting(cfgArduSwarmTimeoutMs, 30));
    if (!enabled) {
        printf("ArduSwarm inactive: runtime values below do not describe stored settings\r\n");
    }
    uint32_t ranges, failures, session;
    uint16_t seq;
    uint8_t tag;
    float range;
    taskENTER_CRITICAL();
    ranges = stats.ranges; failures = stats.failures; session = stats.session;
    seq = stats.sequence; tag = stats.tag; range = stats.range;
    taskEXIT_CRITICAL();
    printf("ArduSwarm enabled=%u net=%lu channel=%lu ant=%lu reply=%lu timeout=%lu\r\n",
           enabled, (unsigned long)network, (unsigned long)channel, (unsigned long)antenna_delay,
           (unsigned long)reply_us, (unsigned long)timeout_ms);
    printf("ranges=%lu failures=%lu tag=%u range=%.3f session=%lu seq=%u\r\n",
           (unsigned long)ranges, (unsigned long)failures, tag, (double)range,
           (unsigned long)session, seq);
}

uwbAlgorithm_t uwbArduSwarmAnchorAlgorithm = {.init = init, .onEvent = on_event};
