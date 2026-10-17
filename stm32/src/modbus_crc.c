#include "modbus_crc.h"

uint16_t mb_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    uint8_t  bit;

    for (i = 0u; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (bit = 0u; bit < 8u; bit++) {
            if (crc & 0x0001u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}
