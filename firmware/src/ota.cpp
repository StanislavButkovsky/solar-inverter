#include "ota.h"
#include "cfg.h"
#include <ArduinoOTA.h>
#include "bootlog.h"
#include <Update.h>
#include "esp_ota_ops.h"
#include "build_info.h"

static bool s_started = false;
static bool s_validated = false;

// Откат при неудачном обновлении. Загрузчик собран с откатом, но ядро
// Arduino помечает новую прошивку годной сразу при старте — откат случился
// бы, только упади она в первые доли секунды. Теперь годной её объявляет
// otaLoop(), когда она проработала VALID_AFTER_MS без сбоя; перезагрузка
// раньше — и загрузчик вернёт прежнюю. Wi-Fi в критерий не входит: роутер
// без питания не повод откатывать исправную прошивку.
static const uint32_t VALID_AFTER_MS = 90000;
extern "C" bool verifyRollbackLater() { return true; }

bool otaBusy() { return Update.isRunning(); }

const char* fwBuild() { return BUILD_INFO; }

bool otaPasswordSet() { return cfg().webPass.length() >= 6; }

void otaStart() {
    if (s_started) return;
    if (!otaPasswordSet()) {
        Serial.println("[ota] пароль не задан — обновление по сети выключено (webpass <пароль>)");
        return;
    }
    ArduinoOTA.setHostname("solar");
    ArduinoOTA.setPassword(cfg().webPass.c_str());
    ArduinoOTA.setMdnsEnabled(false);        // mDNS уже поднимает net.cpp под именем solar
    ArduinoOTA.onStart([]() { Serial.println("[ota] приём прошивки с компьютера…"); });
    ArduinoOTA.onEnd([]()   { Serial.println("[ota] прошивка принята, перезагрузка"); bootMark("обновление прошивки"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] ошибка %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    s_started = true;
    Serial.println("[ota] обновление по сети включено, порт 3232");
}

void otaLoop() {
    if (s_started) ArduinoOTA.handle();
    if (!s_validated && millis() > VALID_AFTER_MS) {
        s_validated = true;
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            Serial.println("[ota] новая прошивка проработала 90 с — помечена годной");
        }
    }
}
