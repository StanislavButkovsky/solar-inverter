#include "modbus.h"

uint16_t mbCrc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

void mbBuildRead(uint8_t* out, uint8_t slave, uint16_t start, uint16_t count) {
    out[0] = slave;
    out[1] = 0x03;                       // чтение регистров хранения
    out[2] = start >> 8;   out[3] = start & 0xFF;
    out[4] = count >> 8;   out[5] = count & 0xFF;
    uint16_t c = mbCrc16(out, 6);
    out[6] = c & 0xFF;                   // контрольная сумма младшим байтом вперёд
    out[7] = c >> 8;
}

void MbAssembler::begin(uint8_t slave, const uint8_t* prefix, uint8_t prefixLen) {
    _slave = slave;
    _prefixLen = prefixLen > sizeof(_prefix) ? sizeof(_prefix) : prefixLen;
    if (_prefixLen) memcpy(_prefix, prefix, _prefixLen);
    _len = 0;
}

bool MbAssembler::feed(const uint8_t* p, size_t n, const uint8_t** data, uint8_t* words) {
    if (_len + n > sizeof(_buf)) _len = 0;      // потеряли начало — начинаем заново
    memcpy(_buf + _len, p, n);
    _len += n;

    if (_len < (size_t)_prefixLen + 3) return false;

    for (uint8_t i = 0; i < _prefixLen; i++)
        if (_buf[i] != _prefix[i]) { _len = 0; return false; }

    const uint8_t* f = _buf + _prefixLen;
    size_t avail = _len - _prefixLen;
    if (f[0] != _slave || f[1] != 0x03) { _len = 0; return false; }

    uint8_t bytes = f[2];
    size_t  need  = 3 + (size_t)bytes + 2;
    if (avail < need) return false;             // хвост ещё не пришёл

    uint16_t got = f[need - 2] | (f[need - 1] << 8);
    _len = 0;
    if (got != mbCrc16(f, need - 2)) { bad++; return false; }

    *data  = f + 3;
    *words = bytes / 2;
    return true;
}
