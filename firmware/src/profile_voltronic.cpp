// Профиль третьего инвертора: Asterion PLUS 5.6K, семейство Voltronic Axpert.
// Текстовый протокол: команды QPIGS/QMOD/QPIWS, ответ — строка полей.
#if defined(TARGET_ASTERION)

#include "profile.h"
#include "voltronic.h"
#include "state.h"

const char* INV_PROFILE_NAME = "Voltronic (Asterion PLUS)";

// Круг опроса: показания, режим, предупреждения. Показания спрашиваются
// вдвое чаще прочего — они и есть то, что видно на экране.
static const char* STEPS[] = { "QPIGS", "QMOD", "QPIGS", "QPIWS" };
const uint8_t INV_NSTEPS = sizeof(STEPS) / sizeof(STEPS[0]);

static uint8_t s_buf[300];
static size_t  s_len = 0;

void invResetStream() { s_len = 0; }

size_t invBuildRequest(uint8_t step, uint8_t* out, size_t cap) {
    return vtBuildCommand(STEPS[step % INV_NSTEPS], out, cap);
}

static void applyQpigs(const VtQpigs& q) {
    invUpdate([&](InvState& v) {
        v.linked = true; v.fresh = true; v.lastFrameMs = millis();

        v.gridV   = q.gridV;
        v.gridHz  = q.gridHz;
        v.outV    = q.outV;
        v.outHz   = q.outHz;
        v.outW    = q.outW;
        v.loadPct = (uint8_t)q.loadPct;
        v.pvV     = q.pvV;
        v.pvW     = q.pvW;
        v.tempC   = q.tempC;
        v.gridOn  = q.gridV > 50.0f;

        float chgW = q.battV * q.battChargeA;
        v.chgW = chgW;

        // Мощности из сети в ответе нет — считаем как недостачу: что нужно
        // нагрузке и заряду сверх того, что дало солнце. Это оценка,
        // и она честнее прочерка, но точным измерением не является.
        float deficit = q.outW + chgW - q.pvW;
        v.gridW = (v.gridOn && deficit > 0) ? deficit : 0;
    });

    // У этого инвертора батарея штатная и своего канала не имеет:
    // всё, что о ней известно, приходит отсюда же.
    stateUpdate([&](BmsState& b) {
        b.linked = true; b.fresh = true; b.lastFrameMs = millis();
        b.voltage = q.battV;
        b.soc     = q.battSoc;
        b.current = q.battChargeA ? (float)q.battChargeA : -(float)q.battDischargeA;
    });
}

bool invFeed(uint8_t step, const uint8_t* data, size_t n) {
    if (s_len + n > sizeof(s_buf)) s_len = 0;
    memcpy(s_buf + s_len, data, n);
    s_len += n;

    // Ответ кончается возвратом каретки; до него ждём.
    if (s_len == 0 || s_buf[s_len - 1] != 0x0D) return false;

    const char* body = nullptr;
    size_t len = vtUnwrap(s_buf, s_len, &body);
    s_len = 0;
    if (!len) return false;                      // не сошлась сумма — ждём следующего

    const char* cmd = STEPS[step % INV_NSTEPS];

    if (strcmp(cmd, "QPIGS") == 0) {
        VtQpigs q;
        if (!vtParseQpigs(body, len, &q)) return false;
        applyQpigs(q);
        return true;
    }

    if (strcmp(cmd, "QMOD") == 0) {
        char m = 0;
        if (!vtParseQmod(body, len, &m)) return false;
        invUpdate([&](InvState& v) {
            switch (m) {
            case 'L': v.mode = INVM_LINE;    break;   // от сети
            case 'B': v.mode = INVM_BATTERY; break;   // от батареи
            case 'S': v.mode = INVM_STANDBY; break;   // ожидание
            case 'F': v.mode = INVM_FAULT;   break;   // авария
            case 'Y': v.mode = INVM_BYPASS;  break;   // обход
            default:  v.mode = INVM_STANDBY; break;
            }
        });
        return true;
    }

    if (strcmp(cmd, "QPIWS") == 0) {
        // Ответ — строка нулей и единиц, по биту на предупреждение.
        // Подробную расшифровку сделаем после сверки с живым инвертором,
        // пока храним сам факт и номер первого поднятого бита.
        uint8_t first = 0;
        for (size_t i = 0; i < len && i < 40; i++)
            if (body[i] == '1') { first = (uint8_t)(i + 1); break; }
        invUpdate([&](InvState& v) {
            v.faultCode = first;
            if (first) v.mode = INVM_FAULT;
        });
        return true;
    }

    return false;
}

#endif
