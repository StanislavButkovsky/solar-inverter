#include <Arduino.h>
#include "button.h"
#include "cfg.h"
#include "net.h"

static const uint32_t HOLD_RESET  = 8000;

// Дольше этого кнопку человек не держит. А вот линия может залипнуть: у платы
// GPIO0 сидит на схеме автосброса, и любая программа, открывшая последовательный
// порт, прижимает его к земле на всё время работы. Для прошивки это неотличимо
// от удержания кнопки — и при закрытии порта срабатывал сброс настроек.
// Отсюда потерянный пароль Wi-Fi, который трижды списывали на порчу NVS.
static const uint32_t HOLD_MAX    = 20000;
static const uint32_t DEBOUNCE_MS = 30;

static bool     s_down      = false;
static uint32_t s_changedAt = 0;
static uint32_t s_downAt    = 0;

static void led(bool on) {
#if defined(LED_ADDRESSABLE)
    // Адресный светодиод: свой протокол, зато можно цветом. Белый — «горит».
    neopixelWrite(PIN_LED, on ? 40 : 0, on ? 40 : 0, on ? 40 : 0);
#else
    digitalWrite(PIN_LED, on ? HIGH : LOW);
#endif
}

static void blink(uint8_t times, uint16_t ms) {
    for (uint8_t i = 0; i < times; i++) {
        led(true);  delay(ms);
        led(false); delay(ms);
    }
}

void buttonInit() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    // Если линия уже прижата на старте (открыт последовательный порт), считаем
    // её нажатой изначально — иначе первое же отпускание сойдёт за длинное
    // нажатие со всеми последствиями.
    delay(5);
    s_down = (digitalRead(PIN_BUTTON) == LOW);
    s_downAt = s_changedAt = millis();
#if !defined(LED_ADDRESSABLE)
    pinMode(PIN_LED, OUTPUT);
#endif
    led(false);
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
        led(false);

        if (held >= HOLD_MAX) {
            Serial.printf("[btn] линия была прижата %u с — это не нажатие, игнорирую\n",
                          held / 1000);
        } else if (held >= HOLD_RESET) {
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

    // Предупреждение о сбросе — только в окне, когда сброс ещё возможен.
    if (s_down && now - s_downAt >= HOLD_RESET && now - s_downAt < HOLD_MAX) {
        led((now / 80) & 1);
        return;
    }

    // в покое светодиод показывает режим: горит — точка доступа поднята
    static uint32_t last = 0;
    if (now - last > 250) {
        last = now;
        if (!s_down) led(netApActive());
    }
}
