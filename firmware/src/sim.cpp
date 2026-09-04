#include "sim.h"
#include "state.h"
#include <Preferences.h>

static volatile bool s_on = false;

// Профиль суток в миниатюре: заряд от солнца днём, разряд вечером.
static void simTick(uint32_t t) {
    const uint8_t CELLS = 14;

    // период 240 с: заряд → пауза → разряд → пауза
    float phase = fmodf(t / 1000.0f, 240.0f);
    float cur;
    if      (phase <  90) cur =  38.0f + 6.0f * sinf(phase / 7.0f);   // заряд
    else if (phase < 110) cur =   0.2f;                               // покой
    else if (phase < 210) cur = -24.0f - 9.0f * sinf(phase / 5.0f);   // разряд
    else                  cur =  -0.3f;

    static float soc = 68.0f;
    soc += cur * 0.0009f;                       // 150 А·ч, шаг раз в секунду
    if (soc > 99.5f) soc = 99.5f;
    if (soc <  8.0f) soc =  8.0f;

    // напряжение ячейки по SOC: грубая кривая NMC 3,45…4,15 В плюс просадка на токе
    float base = 3.45f + (soc / 100.0f) * 0.70f + cur * 0.0007f;

    stateUpdate([&](BmsState& s) {
        s.linked      = true;
        s.fresh       = true;
        s.lastFrameMs = millis();
        s.framesOk++;
        s.rssi        = -62;
        strncpy(s.mac, "DE:MO:00:00:00:01", sizeof(s.mac) - 1);

        s.cellCount = CELLS;
        s.tempCount = 2;

        uint16_t mn = 65535, mx = 0;
        uint8_t  mnNo = 1, mxNo = 1;
        for (uint8_t i = 0; i < CELLS; i++) {
            // небольшой индивидуальный разброс: одна ячейка заметно слабее
            float off = (i == 6 ? -0.022f : 0.0f) + 0.004f * sinf(i * 1.7f);
            uint16_t mv = (uint16_t)((base + off) * 1000.0f);
            s.cellMv[i] = mv;
            if (mv < mn) { mn = mv; mnNo = i + 1; }
            if (mv > mx) { mx = mv; mxNo = i + 1; }
        }
        s.cellMinMv = mn; s.cellMinNo = mnNo;
        s.cellMaxMv = mx; s.cellMaxNo = mxNo;

        s.voltage = 0;
        for (uint8_t i = 0; i < CELLS; i++) s.voltage += s.cellMv[i] / 1000.0f;
        s.current = cur;
        s.soc     = soc;

        s.tempMax = 27 + (int)(3 * sinf(t / 30000.0f));
        s.tempMin = s.tempMax - 2;
        s.tempMaxNo = 1; s.tempMinNo = 2;
        s.cellTemp[0] = s.tempMax;
        s.cellTemp[1] = s.tempMin;

        s.chargeState  = (cur > 1.0f) ? 1 : (cur < -1.0f ? 2 : 0);
        s.chargeMos    = true;
        s.dischargeMos = true;
        s.bmsLife      = (uint8_t)((t / 1000) & 0xFF);
        s.remainingMah = (uint32_t)(soc * 1500.0f);   // 150 А·ч

        // балансировка включается на верхних ячейках в конце заряда
        memset(s.balance, 0, sizeof(s.balance));
        if (soc > 90.0f) {
            for (uint8_t i = 0; i < CELLS; i++)
                if (s.cellMv[i] > mn + 12) s.balance[i / 8] |= (1 << (i % 8));
        }

        memset(s.faults, 0, sizeof(s.faults));
        s.haveFaults = true;
    });
}

// Инвертор в демо-режиме. Отдельно моделируется пропадание питающей сети:
// каждые четыре минуты на сорок секунд — иначе индикацию «сеть пропала и
// вернулась» и журнал событий нечем показать до появления RS485.
static void simInv(uint32_t t, float battCur, float soc) {
    float phase   = fmodf(t / 1000.0f, 240.0f);
    bool  gridOn  = !(phase > 150.0f && phase < 190.0f);

    // солнце: горб в середине цикла, ноль по краям
    float sun = sinf((phase / 240.0f) * 3.14159f);
    if (sun < 0) sun = 0;
    float pvW = sun * 4200.0f;

    float loadW = 900.0f + 350.0f * sinf(phase / 11.0f);
    if (loadW < 120) loadW = 120;

    invUpdate([&](InvState& v) {
        v.linked      = true;
        v.fresh       = true;
        v.lastFrameMs = millis();

        v.gridOn = gridOn;
        v.gridV  = gridOn ? 231.0f + 3.0f * sinf(phase / 7.0f) : 0.0f;
        v.gridHz = gridOn ? 50.0f : 0.0f;
        // Из сети берётся то, чего не покрыло солнце: нагрузка плюс заряд минус выработка.
        float deficit = loadW + (battCur > 0 ? battCur * 55.0f : 0.0f) - pvW;
        v.gridW = (gridOn && deficit > 0) ? deficit : 0.0f;

        v.pvV = pvW > 50 ? 320.0f + 40.0f * sun : 0.0f;
        v.pvW = pvW;

        v.outV    = 230.0f;
        v.outHz   = 50.0f;
        v.outW    = loadW;
        v.loadPct = (uint8_t)(loadW / 6200.0f * 100.0f);

        v.chgW = battCur > 0 ? battCur * 55.0f : 0.0f;

        // Режим выводится из того, откуда сейчас берётся мощность.
        if (!gridOn)              v.mode = INVM_BATTERY;
        else if (pvW > loadW)     v.mode = INVM_BATTERY;   // солнце кормит и грузит батарею
        else                      v.mode = INVM_LINE;
        if (soc < 12.0f && !gridOn) v.mode = INVM_STANDBY;

        v.tempC     = 38 + (int)(6 * sun);
        v.faultCode = 0;
        v.prioOut   = 2;    // SBU
        v.prioChg   = 0;    // сеть и солнце
    });
}

static void simTask(void*) {
    Preferences p;
    p.begin("solar", true);
    s_on = p.getBool("sim", false);
    p.end();
    if (s_on) Serial.println("[sim] демо-данные включены");

    for (;;) {
        if (s_on) {
            simTick(millis());
            BmsState b = stateSnapshot();
            simInv(millis(), b.current, b.soc);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void simStart() {
    xTaskCreatePinnedToCore(simTask, "sim", 4096, nullptr, 3, nullptr, 1);
}

void simSetEnabled(bool on) {
    s_on = on;
    Preferences p;
    p.begin("solar", false);
    p.putBool("sim", on);
    p.end();
    if (!on) {
        invUpdate([](InvState& v) { v = InvState(); });
        stateUpdate([](BmsState& s) {
            s.linked = false; s.fresh = false; s.cellCount = 0;
            memset(s.cellMv, 0, sizeof(s.cellMv));
            s.mac[0] = 0;
        });
    }
}

bool simEnabled() { return s_on; }
