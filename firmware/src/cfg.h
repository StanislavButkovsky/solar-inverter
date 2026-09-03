// Конфигурация в NVS. Всё, что переживает перезагрузку.
#pragma once
#include <Arduino.h>

struct Config {
    String  ssid;
    String  pass;
    String  bmsMac;      // "aa:bb:cc:dd:ee:ff", пусто = искать по имени/сервису
    String  webPass;     // пароль на запись; пусто = запись запрещена вовсе
    uint16_t pollMs;     // пауза между запросами к BMS
    bool     apAfterSave; // одноразово: после смены сети поднять точку доступа,
                          // чтобы человек смог вернуться и узнать новый адрес
};

void    cfgInit();
Config& cfg();
void    cfgSave();
void    cfgFactoryReset();
