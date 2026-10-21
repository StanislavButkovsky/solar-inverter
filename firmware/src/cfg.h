// Конфигурация в NVS. Всё, что переживает перезагрузку.
#pragma once
#include <Arduino.h>

struct Config {
    String  ssid;
    String  pass;
    String  bmsMac;      // "aa:bb:cc:dd:ee:ff", пусто = искать по имени/сервису
    String  invMac;      // донгл инвертора Eybond; пусто = не опрашивать
    String  webPass;     // пароль на запись; пусто = запись запрещена вовсе
    uint16_t pollMs;     // пауза между запросами к BMS
    bool     apAfterSave; // одноразово: после смены сети поднять точку доступа,
                          // чтобы человек смог вернуться и узнать новый адрес
    String   tgToken;     // токен бота Telegram от @BotFather; пусто — бот выключен
    String   tgChat;      // привязанный чат; пусто — ждём /start с кодом
    uint8_t  tgSocLow;    // порог «батарея разряжена», %
};

void    cfgInit();
Config& cfg();
void    cfgSave();
void    cfgFactoryReset();
