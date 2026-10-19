// Профиль второго инвертора: Hiden Control HS35-5648PRO по USB.
//
// Ждали Modbus, как у MUST PV18, а шесть разведок 19.10.2026 показали другое:
// за USB мост CH340, и порт говорит протоколом Megatec Q1 — тем, что у
// бытовых ИБП, — на 9600 8N1. Команду принимает только побайтно с паузой
// от 5 мс (целиком — NAK). Из 60 команд-вопросов отвечают три:
//
//   Q1, QS   (228.9 000.0 228.8 013 50.0 2.06 58.0 00100000
//            сеть, сеть при сбое, выход, загрузка %, частота,
//            батарея на элемент, температура, биты b7…b0
//   I        #220.0 026 048.0 50.0
//            номиналы: напряжение, ток, батарея, частота
//
// Панелей, тока батареи и процента заряда в этом протоколе нет вовсе.
#if defined(TARGET_HS35) && !defined(TARGET_ASTERION) && !defined(TARGET_MUST)

#include "profile.h"
#include "state.h"
#include <stdlib.h>
#include <string.h>

const char*    INV_PROFILE_NAME = "Megatec Q1 (HS35 по USB)";
const uint32_t INV_BAUD   = 9600;
const uint16_t INV_GAP_MS = 10;          // ответ с 5 мс, берём с запасом

// Номиналы нужны дважды — пересчитать батарею из вольт на элемент и
// загрузку из процентов в ватты, — а меняться им неоткуда. Поэтому «I»
// в круге одна на четыре. Состояние её не ждёт: первая версия спрашивала
// одну «I», пока та не ответит, и на живом HS35 так и не дошла до Q1.
static const char* STEPS[] = { "Q1", "Q1", "Q1", "I" };
const uint8_t INV_NSTEPS = sizeof(STEPS) / sizeof(STEPS[0]);

static float s_ratedV = 0, s_ratedA = 0, s_ratedBattV = 0;

static char   s_buf[128];
static size_t s_len = 0;

void invResetStream() { s_len = 0; }

size_t invBuildRequest(uint8_t step, uint8_t* out, size_t cap) {
    const char* cmd = STEPS[step % INV_NSTEPS];
    size_t n = strlen(cmd);
    if (n + 1 > cap) return 0;
    memcpy(out, cmd, n);
    out[n] = '\r';                       // без контрольной суммы: Megatec её не знает
    return n + 1;
}

// Разбить на числа после первого символа ('(' или '#').
static int fields(const char* s, size_t len, float* out, int max, char* bits, size_t bitsCap) {
    int n = 0;
    size_t i = 1;
    while (i < len && n < max) {
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && s[i] != ' ') i++;
        // Последнее поле Q1 — строка битов, её отдаём как есть.
        if (bits && n == max - 1) {
            size_t k = i - start < bitsCap - 1 ? i - start : bitsCap - 1;
            memcpy(bits, s + start, k);
            bits[k] = 0;
        }
        out[n++] = strtof(s + start, nullptr);
    }
    return n;
}

static void applyQ1(const char* s, size_t len) {
    float f[8];
    char  bits[12] = {0};
    if (fields(s, len, f, 8, bits, sizeof(bits)) < 8 || strlen(bits) < 8) return;

    // b7 сбой сети, b6 батарея разряжена, b5 обход, b4 авария инвертора,
    // b3 тип «standby», b2 тест, b1 выключение, b0 пищалка.
    bool gridFail = bits[0] == '1';
    bool battLow  = bits[1] == '1';
    bool bypass   = bits[2] == '1';
    bool failed   = bits[3] == '1';

    // Батарея приходит в вольтах на элемент свинцового счёта (2 В на
    // элемент): 2.06 у пакета на 48 В — это 24 элемента, около 49,4 В.
    float cellV = f[5];
    float cells = s_ratedBattV > 0 ? s_ratedBattV / 2.0f : 24.0f;
    float battV = cellV < 5.0f ? cellV * cells : cellV;

    // Мощности в протоколе нет, только процент загрузки. Пересчёт через
    // номинал даёт полную мощность (ВА), а не активную — это оценка.
    float ratedVA = s_ratedV * s_ratedA;

    invUpdate([&](InvState& v) {
        v.linked = true; v.fresh = true; v.lastFrameMs = millis();
        v.gridV   = f[0];
        v.outV    = f[2];
        v.loadPct = (uint8_t)f[3];
        v.gridHz  = f[4];
        v.outHz   = f[4];                // частота одна на оба
        v.tempC   = (int16_t)f[6];
        v.outW    = ratedVA > 0 ? ratedVA * f[3] / 100.0f : 0;
        v.gridOn  = !gridFail && f[0] > 50.0f;
        // В обходе нагрузку кормит сеть — значит, из сети уходит её мощность.
        v.gridW   = (v.gridOn && bypass) ? v.outW : 0;
        v.faultCode = failed ? 1 : (battLow ? 2 : 0);

        if (failed)        v.mode = INVM_FAULT;
        else if (gridFail) v.mode = INVM_BATTERY;
        else if (bypass)   v.mode = INVM_BYPASS;
        else if (v.gridOn) v.mode = INVM_LINE;
        else               v.mode = INVM_STANDBY;
    });

    // Батарея штатная и своего канала не имеет: известно только напряжение.
    stateUpdate([&](BmsState& b) {
        b.linked = true; b.fresh = true; b.lastFrameMs = millis();
        b.voltage  = battV;
        b.socKnown = false;
    });
}

static void applyRating(const char* s, size_t len) {
    float f[4];
    if (fields(s, len, f, 4, nullptr, 0) < 4) return;
    s_ratedV = f[0];
    s_ratedA = f[1];
    s_ratedBattV = f[2];
}

bool invFeed(uint8_t step, const uint8_t* data, size_t n) {
    if (s_len + n >= sizeof(s_buf)) s_len = 0;
    memcpy(s_buf + s_len, data, n);
    s_len += n;
    if (!s_len || s_buf[s_len - 1] != '\r') return false;   // строка ещё не кончилась

    size_t len = s_len - 1;
    s_len = 0;
    // NAK — порт не разобрал команду; шаг повторится на следующем опросе.
    if (len >= 3 && !memcmp(s_buf, "NAK", 3)) return false;

    if (s_buf[0] == '(') { applyQ1(s_buf, len); return true; }
    if (s_buf[0] == '#') { applyRating(s_buf, len); return true; }
    return false;
}

#endif
