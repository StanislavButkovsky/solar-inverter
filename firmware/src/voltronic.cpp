#include "voltronic.h"
#include <string.h>
#include <stdlib.h>

uint16_t vtCrc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

size_t vtBuildCommand(const char* cmd, uint8_t* out, size_t cap) {
    size_t n = strlen(cmd);
    if (n + 3 > cap) return 0;
    memcpy(out, cmd, n);
    uint16_t c = vtCrc16(out, n);
    out[n]     = (uint8_t)(c >> 8);      // старшим байтом вперёд, в отличие от Modbus
    out[n + 1] = (uint8_t)(c & 0xFF);
    out[n + 2] = 0x0D;
    return n + 3;
}

size_t vtUnwrap(const uint8_t* frame, size_t n, const char** body) {
    if (n < 5 || frame[0] != '(' || frame[n - 1] != 0x0D) return 0;

    size_t payload = n - 3;                       // без двух байтов суммы и возврата каретки
    uint16_t got = ((uint16_t)frame[n - 3] << 8) | frame[n - 2];
    if (got != vtCrc16(frame, payload)) return 0;

    *body = (const char*)frame + 1;               // без открывающей скобки
    return payload - 1;
}

// Разбиение по пробелам без выделения памяти: возвращает указатели на поля.
static int split(const char* s, size_t len, const char* fld[], int max) {
    int n = 0;
    size_t i = 0;
    while (i < len && n < max) {
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        fld[n++] = s + i;
        while (i < len && s[i] != ' ') i++;
    }
    return n;
}

bool vtParseQpigs(const char* body, size_t len, VtQpigs* o) {
    const char* f[24];
    int n = split(body, len, f, 24);
    // Двадцать одно поле — стандартный ответ. Некоторые прошивки добавляют
    // свои в конец; лишние игнорируем, недостачу считаем ошибкой.
    if (n < 21) { o->ok = false; return false; }

    o->gridV          = (float)atof(f[0]);
    o->gridHz         = (float)atof(f[1]);
    o->outV           = (float)atof(f[2]);
    o->outHz          = (float)atof(f[3]);
    o->outVA          = atoi(f[4]);
    o->outW           = atoi(f[5]);
    o->loadPct        = atoi(f[6]);
    o->busV           = atoi(f[7]);
    o->battV          = (float)atof(f[8]);
    o->battChargeA    = atoi(f[9]);
    o->battSoc        = atoi(f[10]);
    o->tempC          = atoi(f[11]);
    o->pvA            = (float)atof(f[12]);
    o->pvV            = (float)atof(f[13]);
    o->sccBattV       = (float)atof(f[14]);
    o->battDischargeA = atoi(f[15]);

    // Поле 16 — строка битов состояния вида "00010110", старший бит слева.
    o->status = 0;
    for (int i = 0; i < 8; i++) {
        char c = f[16][i];
        if (c != '0' && c != '1') break;
        o->status = (uint8_t)((o->status << 1) | (c - '0'));
    }

    o->pvW = atoi(f[19]);
    o->ok  = true;
    return true;
}

bool vtParseQmod(const char* body, size_t len, char* mode) {
    if (len < 1) return false;
    *mode = body[0];
    return true;
}
