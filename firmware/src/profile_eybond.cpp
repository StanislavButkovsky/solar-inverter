// Профиль первого инвертора: донгл Eybond, профиль 09CB.
// Проверен сообществом на «Shenzhen Manke Hybrid 5.5K»; для нашей модели
// это рабочая гипотеза, которую надо сверить с показаниями SmartESS.
#if !defined(TARGET_HS35)

#include "profile.h"
#include "state.h"

const uint8_t  INV_SLAVE = 1;
const InvBlock INV_BLOCKS[] = { {201, 21}, {231, 3} };
const uint8_t  INV_NBLOCKS = sizeof(INV_BLOCKS) / sizeof(INV_BLOCKS[0]);

// Ответ донгла всегда начинается с восьми ASCII-единиц.
const uint8_t INV_PREFIX[] = { '1','1','1','1','1','1','1','1' };
const uint8_t INV_PREFIX_LEN = sizeof(INV_PREFIX);
const char*   INV_PROFILE_NAME = "Eybond 09CB";

static inline uint16_t be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }

void invApplyBlock(uint16_t start, const uint8_t* d, uint8_t n) {
    auto R  = [&](uint16_t r) -> uint16_t {
        uint16_t i = r - start; return (i < n) ? be16(d + i * 2) : 0;
    };
    auto RS = [&](uint16_t r) -> int16_t { return (int16_t)R(r); };

    invUpdate([&](InvState& v) {
        v.linked = true; v.fresh = true; v.lastFrameMs = millis();

        if (start == 201) {
            uint16_t mode = R(201);
            v.gridV   = R(202) * 0.1f;
            v.gridHz  = R(203) * 0.01f;
            v.gridW   = R(204);
            v.outV    = R(206) * 0.1f;
            v.outHz   = R(208) * 0.01f;
            v.outW    = R(209);
            v.loadPct = (uint8_t)R(214);
            v.pvV     = R(211) * 0.1f;
            v.pvW     = R(213);

            float battW = RS(218);          // минус — разряд
            v.chgW = battW > 0 ? battW : 0;
            v.tempC = RS(220);

            v.mode = (mode == 3) ? INVM_BATTERY : (mode == 1 ? INVM_LINE : INVM_STANDBY);
        } else if (start == 231) {
            uint16_t st = R(231);
            v.gridOn    = (st >> 2) & 1;
            v.faultCode = (uint8_t)R(232);
            if (v.faultCode) v.mode = INVM_FAULT;
            v.prioOut = 0xFF;               // в этой карте приоритетов нет
            v.prioChg = 0xFF;
        }
    });
}

#endif
