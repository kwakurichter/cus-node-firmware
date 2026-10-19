/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "cfg.h"
#include "eeprom.h"
static unsigned char memory[100];
static bool fail_write;
bool eepromRead(int address, void *data, size_t size)
{
    assert(address == 0 && size <= sizeof(memory));
    memcpy(data, memory, size);
    return true;
}
bool eepromWrite(int address, void *data, size_t size)
{
    assert(address == 0 && size <= sizeof(memory));
    if (fail_write) { return false; }
    memcpy(memory, data, size);
    return true;
}
void HAL_Delay(uint32_t delay) { (void)delay; }
int main(void)
{
    memset(memory, 0xff, sizeof(memory));
    cfgInit();
    uint8_t id;
    assert(cfgReadU8(cfgAddress, &id) && id == 0);
    assert(cfgWriteU8(cfgAddress, 8));
    const uint32_t values[] = {17, 2, 16384, 3000, 30};
    for (unsigned i = 0; i < 5; i++) {
        assert(cfgWriteU32((ConfigField)(cfgArduSwarmNetwork + i), values[i]));
    }
    cfgInit();  // reboot/persistence, including legacy fields
    assert(cfgReadU8(cfgAddress, &id) && id == 8);
    for (unsigned i = 0; i < 5; i++) {
        uint32_t value = 0;
        assert(cfgReadU32((ConfigField)(cfgArduSwarmNetwork + i), &value));
        assert(value == values[i]);
    }
    uint32_t value = 99;
    assert(!cfgReadU32(cfgAddress, &value) && value == 99);
    assert(!cfgWriteU32(cfgAddress, 42));
    uint8_t huge[100] = {0};
    float big[100] = {0};
    assert(!cfgWriteU8list((ConfigField)90, huge, sizeof(huge)));
    assert(!cfgWriteFP32list((ConfigField)91, big, 100));
    assert(!cfgReadU8list(cfgAnchorlist, huge, sizeof(huge)));
    fail_write = true;
    assert(!cfgWriteU32(cfgArduSwarmNetwork, 29));
    fail_write = false;
    assert(cfgReadU32(cfgArduSwarmNetwork, &value) && value == 17);
    // Corrupt checksum models interrupted storage: reject, then use safe legacy defaults.
    memory[10] ^= 0x40;
    cfgInit();
    assert(cfgReadU8(cfgAddress, &id) && id == 0);
    assert(!cfgReadU32(cfgArduSwarmNetwork, &value));
    puts("Configuration migration, persistence, bounds and failed-write tests passed");
    return 0;
}
