#include <Arduino.h>
#include "button.h"
#include "cfg.h"
#include "net.h"

static const uint32_t HOLD_RESET  = 8000;
static const uint32_t DEBOUNCE_MS = 30;

static bool     s_down      = false;
static uint32_t s_changedAt = 0;
static uint32_t s_downAt    = 0;

static void blink(uint8_t times, uint16_t ms) {
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(PIN_LED, HIGH); delay(ms);
        digitalWrite(PIN_LED, LOW);  delay(ms);
    }
}

void buttonInit() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    blink(2, 90);                       // «прошивка стартовала»
}

void buttonLoop() {
    bool raw = (digitalRead(PIN_BUTTON) == LOW);
    uint32_t now = millis();

    if (raw != s_down) {
        if (now - s_changedAt < DEBOUNCE_MS) return;
        s_changedAt = now;
        s_down = raw;
        if (s_down) { s_downAt = now; return; }

        uint32_t held = now - s_downAt;
        digitalWrite(PIN_LED, LOW);

        if (held >= HOLD_RESET) {
            Serial.println("[btn] сброс настроек и перезагрузка");
            blink(6, 60);
            cfgFactoryReset();
            delay(200);
            ESP.restart();
        } else {
            bool on = netToggleAp();
            Serial.printf("[btn] точка доступа %s\n", on ? "включена" : "выключена");
            blink(on ? 2 : 1, 150);
        }
        return;
    }

    if (s_down && now - s_downAt >= HOLD_RESET) {
        digitalWrite(PIN_LED, (now / 80) & 1);      // предупреждение о сбросе
        return;
    }

    // в покое светодиод показывает режим: горит — точка доступа поднята
    static uint32_t last = 0;
    if (now - last > 250) {
        last = now;
        if (!s_down) digitalWrite(PIN_LED, netApActive() ? HIGH : LOW);
    }
}
