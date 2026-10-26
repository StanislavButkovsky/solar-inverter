// Профиль первого инвертора: донгл Eybond, профиль 09CB.
// Проверен сообществом на «Shenzhen Manke Hybrid 5.5K»; для нашей модели
// это рабочая гипотеза, которую надо сверить с показаниями SmartESS.
#if !defined(TARGET_HS35)

#include "profile_modbus.h"
#include "state.h"

const uint8_t  INV_SLAVE = 1;
const InvBlock INV_BLOCKS[] = { {201, 21}, {231, 3} };
const uint8_t  INV_NSTEPS = sizeof(INV_BLOCKS) / sizeof(INV_BLOCKS[0]);

// Ответ донгла всегда начинается с восьми ASCII-единиц.
const uint8_t INV_PREFIX[] = { '1','1','1','1','1','1','1','1' };
const uint8_t INV_PREFIX_LEN = sizeof(INV_PREFIX);
const char*   INV_PROFILE_NAME = "Eybond 09CB";

static inline uint16_t be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }

// Сырые регистры последнего ответа — для сверки карты с экраном инвертора:
// карта 09CB с чужой модели, и 26.10.2026 на SD-HYM-4862HWP она дала выход
// 3,7 В / 5,9 Гц. Показываются в /api/inv.
static const uint16_t RAW_FROM = 201, RAW_N = 33;      // 201…233
static uint16_t s_raw[RAW_N];
static bool     s_rawHave[RAW_N];
static uint32_t s_rawMs = 0;

String invProfileReport() {
    String o = "сырые регистры (адрес: число, hex, со знаком), ответ " +
               String(s_rawMs ? (millis() - s_rawMs) / 1000 : 0) + " с назад:\n";
    for (uint16_t i = 0; i < RAW_N; i++) {
        if (!s_rawHave[i]) continue;
        char b[48];
        snprintf(b, sizeof(b), "  %u: %u  0x%04X  %d\n", RAW_FROM + i, s_raw[i], s_raw[i], (int16_t)s_raw[i]);
        o += b;
    }
    return o;
}

void invApplyBlock(uint16_t start, const uint8_t* d, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        uint16_t r = start + i;
        if (r < RAW_FROM || r >= RAW_FROM + RAW_N) continue;
        s_raw[r - RAW_FROM] = be16(d + i * 2);
        s_rawHave[r - RAW_FROM] = true;
    }
    s_rawMs = millis();

    auto R  = [&](uint16_t r) -> uint16_t {
        uint16_t i = r - start; return (i < n) ? be16(d + i * 2) : 0;
    };
    auto RS = [&](uint16_t r) -> int16_t { return (int16_t)R(r); };

    bool  haveBatt = false;
    float bV = 0, bA = 0;
    invUpdate([&](InvState& v) {
        v.linked = true; v.fresh = true; v.lastFrameMs = millis();

        if (start == 201) {
            // Карта SD-HYM-4862HWP, сверена 26.10.2026 с экраном инвертора (выход
            // 230,2 В / 50 Гц / 510 Вт, батарея 55,7 В, панели 160 В / 480 Вт):
            // от карты 09CB выход сдвинут на регистр, у панелей и батареи —
            // свой порядок. 210–213 — второй набор В/А/Гц/Вт (сам инвертор?), не берём.
            uint16_t mode = R(201);
            v.gridV   = R(202) * 0.1f;
            v.gridHz  = R(203) * 0.01f;
            v.gridW   = R(204);
            v.outV    = R(205) * 0.1f;
            v.outHz   = R(207) * 0.01f;
            v.outW    = R(208);
            // Процента загрузки в регистрах нет — от номинала 6,2 кВт (…62…).
            float pct = v.outW * 100.0f / 6200.0f;
            v.loadPct = (uint8_t)(pct > 255 ? 255 : pct + 0.5f);
            v.pvW     = R(218) * 0.1f;
            v.pvV     = R(219) * 0.1f;

            float battW = RS(217);          // минус — разряд
            v.chgW = battW > 0 ? battW : 0;
            v.tempC = RS(220);

            v.mode = (mode == 3) ? INVM_BATTERY : (mode == 1 ? INVM_LINE : INVM_STANDBY);

            haveBatt = true;
            bV = R(215) * 0.1f;
            bA = RS(216) * 0.1f;
        } else if (start == 231) {
            uint16_t st = R(231);
            v.gridOn    = (st >> 2) & 1;
            // По карте 09CB 232 — код аварии, но у SD-HYM-4862HWP 26.10.2026
            // там −322 при работе без аварии: величина со знаком, не код.
            // Аварию отсюда не берём, пока не найдём настоящий регистр.
            v.faultCode = 0;
            v.prioOut = 0xFF;               // в этой карте приоритетов нет
            v.prioChg = 0xFF;
        }
    });

    // Батарея — по инвертору, пока DALY не отдаёт ячейки: её данные точнее.
    // Вне invUpdate: замок у снапшотов общий и не рекурсивный — изнутри
    // stateUpdate ждал его 200 мс и молча ничего не делал.
    if (haveBatt) stateUpdate([&](BmsState& b) {
        if (b.cellCount) return;
        b.linked = true; b.fresh = true; b.lastFrameMs = millis();
        b.voltage = bV;
        b.current = bA;
        b.chargeState = bA > 0.5f ? 1 : (bA < -0.5f ? 2 : 0);
    });
}

#endif
