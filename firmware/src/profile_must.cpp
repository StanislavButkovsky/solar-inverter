// Профиль второго инвертора: Hiden Control HS35-5648PRO, он же MUST PV18 Pro.
// Modbus 19200, адрес 4, два банка регистров плюс отдельные адреса батареи.
// Источник карты: vladyspavlov/esphome-must-inverter, разбор в HIDEN-HS35.md.
#if defined(TARGET_HS35)

#include "profile.h"
#include "state.h"

const uint8_t  INV_SLAVE = 4;

// 15205…15209 — контроллер заряда от панелей,
// 25205…25216 — сам инвертор,
// 25273…25274 — мощность и ток батареи,
// 113…114     — заряд и здоровье батареи, если прошивка их отдаёт.
const InvBlock INV_BLOCKS[] = {
    {15205, 5},
    {25205, 12},
    {25273, 2},
    {113,   2},
};
const uint8_t  INV_NBLOCKS = sizeof(INV_BLOCKS) / sizeof(INV_BLOCKS[0]);

// Мост CH340 прозрачный, никакого префикса перед кадром нет.
const uint8_t INV_PREFIX[] = { 0 };
const uint8_t INV_PREFIX_LEN = 0;
const char*   INV_PROFILE_NAME = "MUST PV18 (HS35)";

static inline uint16_t be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }

void invApplyBlock(uint16_t start, const uint8_t* d, uint8_t n) {
    auto R  = [&](uint16_t r) -> uint16_t {
        uint16_t i = r - start; return (i < n) ? be16(d + i * 2) : 0;
    };
    auto RS = [&](uint16_t r) -> int16_t { return (int16_t)R(r); };

    invUpdate([&](InvState& v) {
        v.linked = true; v.fresh = true; v.lastFrameMs = millis();

        switch (start) {
        case 15205:                                    // панели
            v.pvV = R(15205) * 0.1f;
            v.pvW = R(15208);
            break;

        case 25205: {                                  // инвертор
            uint16_t mode = 0;                         // режим лежит в 25201, читаем отдельно
            (void)mode;
            v.gridV   = R(25207) * 0.1f;
            v.outV    = R(25206) * 0.1f;
            v.outW    = R(25213);
            v.gridW   = R(25214);
            v.loadPct = (uint8_t)R(25216);
            // Сеть считаем присутствующей по напряжению: отдельного бита
            // в этой карте нет, а реле 25238 читается другим блоком.
            v.gridOn  = v.gridV > 50.0f;
            v.mode    = v.gridOn ? INVM_LINE : INVM_BATTERY;
            break;
        }

        case 25273: {                                  // батарея со стороны инвертора
            float battW = RS(25273);                   // минус — разряд
            v.chgW = battW > 0 ? battW : 0;
            // Ток и напряжение батареи кладём в снапшот батареи: у этого
            // инвертора она штатная и своего канала связи не имеет.
            float amps = RS(25274) * 0.1f;
            stateUpdate([&](BmsState& b) {
                b.linked = true; b.fresh = true; b.lastFrameMs = millis();
                b.current = amps;
                b.voltage = 0;                         // напряжение придёт блоком 25205
            });
            break;
        }

        case 113:                                      // заряд и здоровье батареи
            stateUpdate([&](BmsState& b) {
                b.linked = true; b.fresh = true; b.lastFrameMs = millis();
                b.soc = R(113);
            });
            break;
        }
    });
}

#endif
