// Транспорт до инвертора по USB: встроенный мост CH340 (VID 0x1A86, PID 0x7523).
//
// Плата в роли хоста, инвертор — устройство. Обязателен ESP32-S3: у обычного
// ESP32 контроллера USB-host нет ни аппаратно, ни программно.
//
// ⚠️ Драйвер пишется под приехавшее железо. Пока здесь заглушка, которая
// честно сообщает «канала нет»: так собирается и проверяется всё остальное —
// профиль регистров, показ, журнал, — не дожидаясь плат.
//
// Что предстоит: поднять usb_host, дождаться устройства с нужными VID/PID,
// выставить скорость 19200 8N1 управляющими посылками CH340, повесить
// приём на bulk-in и звать invOnBytes(). Готовый компонент Espressif —
// usb_host_ch34x_vcp.
#if defined(TARGET_HS35)

#include "inv.h"
#include "inv_transport.h"
#include "cfg.h"

static bool s_reported = false;

bool invLooksLikeDongle(const String&, bool) { return false; }   // тут не ищут в эфире

void invTrBegin() {
    Serial.println("[инв] транспорт USB: драйвер CH340 ещё не написан");
}

bool invTrConnected() { return false; }

bool invTrConnect() {
    if (!s_reported) {
        s_reported = true;
        Serial.println("[инв] жду плату ESP32-S3, чтобы писать драйвер по живому");
    }
    return false;
}

void invTrSend(const uint8_t*, size_t) { }

void invBind(const String&) {
    Serial.println("[инв] по USB привязывать нечего: устройство одно");
}

#endif
