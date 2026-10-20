#include "ota.h"
#include "cfg.h"
#include <ArduinoOTA.h>

static bool s_started = false;

const char* fwBuild() { return __DATE__ " " __TIME__; }

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
    ArduinoOTA.onEnd([]()   { Serial.println("[ota] прошивка принята, перезагрузка"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] ошибка %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    s_started = true;
    Serial.println("[ota] обновление по сети включено, порт 3232");
}

void otaLoop() {
    if (s_started) ArduinoOTA.handle();
}
