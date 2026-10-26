// Профиль первого инвертора: донгл Eybond, профиль 09CB.
// Проверен сообществом на «Shenzhen Manke Hybrid 5.5K»; для нашей модели
// это рабочая гипотеза, которую надо сверить с показаниями SmartESS.
#if !defined(TARGET_HS35)

#include "profile_modbus.h"
#include "state.h"

const uint8_t  INV_SLAVE = 1;
// 222–230 — панели (мощность), температуры, оценка заряда; 234–249 у этого
// инвертора нули, не читаем.
const InvBlock INV_BLOCKS[] = { {201, 21}, {231, 3}, {222, 9} };
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
static float    s_pvW = -1;             // регистр 223, из блока 222

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
            // Карта SD-HYM-4862HWP, сверена 26.10.2026 с экраном инвертора и
            // балансом мощностей (панели = нагрузка + заряд + потери) на шести
            // замерах. От карты 09CB выход сдвинут на регистр, остальное — своё:
            //   205/206/207 выход В ×0,1, А ×0,1, Гц ×0,01 · 208 — больше ВА,
            //   видимо мощность самого инвертора с потерями, не берём
            //   213 нагрузка Вт · 214 нагрузка ВА (процент на экране — от ВА)
            //   215/216/217 батарея В ×0,1, А ×0,1 и Вт со знаком (+ заряд)
            //   218 шина DC ×0,1 В (390–480 при любом солнце — не панели)
            //   219/220 панели В ×0,1, А ×0,1 · 223 панели Вт
            //   226–228 температуры °C · 229 заряд по оценке инвертора, %
            uint16_t mode = R(201);
            v.gridV   = R(202) * 0.1f;
            v.gridHz  = R(203) * 0.01f;
            v.gridW   = R(204);
            v.outV    = R(205) * 0.1f;
            v.outHz   = R(207) * 0.01f;
            v.outW    = R(213);
            float pct = R(214) * 100.0f / 6200.0f;           // от ВА, номинал 6,2 кВт
            v.loadPct = (uint8_t)(pct > 255 ? 255 : pct + 0.5f);
            v.pvV     = R(219) * 0.1f;
            // мощность панелей — в блоке 222 (s_pvW), здесь — пока он не пришёл
            v.pvW     = s_pvW >= 0 ? s_pvW : R(219) * 0.1f * R(220) * 0.1f;

            float battW = RS(217);          // минус — разряд
            v.chgW = battW > 0 ? battW : 0;

            v.mode = (mode == 3) ? INVM_BATTERY : (mode == 1 ? INVM_LINE : INVM_STANDBY);

            haveBatt = true;
            bV = R(215) * 0.1f;
            bA = RS(216) * 0.1f;
        } else if (start == 222) {
            s_pvW = R(223);
            v.pvW = s_pvW;
            int16_t t = RS(226);
            if (RS(227) > t) t = RS(227);
            if (RS(228) > t) t = RS(228);
            v.tempC = t;                    // самая горячая из трёх
        } else if (start == 231) {
            uint16_t st = R(231);
            v.gridOn    = (st >> 2) & 1;
            // По карте 09CB 232 — код аварии, но у SD-HYM-4862HWP 26.10.2026
            // там −322, −7, 14 без аварии — повторяет ток батареи (216), не код.
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
