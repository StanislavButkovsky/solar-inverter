// Конфигурация в NVS. Всё, что переживает перезагрузку.
#pragma once
#include <Arduino.h>

struct Config {
    String  ssid;
    String  pass;
    // Свой адрес в сети вместо выданного DHCP. Пустой sip — DHCP. Проверяет
    // и применяет net.cpp (netSetStatic), пустой sdns — DNS шлюза.
    String  sip, sgw, smask, sdns;
    bool    wifiSleep;   // сон Wi-Fi (modem sleep); /api/wifi/sleep, см. net.cpp
    String  bmsMac;     // "aa:bb:cc:dd:ee:ff", пусто = искать по имени/сервису
    String  invMac;      // донгл инвертора Eybond; пусто = не опрашивать
    String  webPass;     // пароль на запись; пусто = запись запрещена вовсе
    uint16_t pollMs;     // пауза между запросами к BMS
    bool     apAfterSave; // одноразово: после смены сети поднять точку доступа,
                          // чтобы человек смог вернуться и узнать новый адрес
    String   tgToken;     // токен бота Telegram от @BotFather; пусто — бот выключен
    String   tgChat;      // привязанный чат; пусто — ждём /start с кодом
    String   mxToken;     // токен бота MAX (business.max.ru); пусто — MAX выключен
    String   mxChat;      // привязанный чат MAX (chat_id диалога)
    uint8_t  tgSocLow;    // порог «батарея разряжена», %

    // Погода и автоматика (weather.cpp). Координаты 0/0 — погода выключена.
    float    lat, lon;
    float    pvKwp;       // мощность панелей, кВт; 0 — не задана, прогноз учится по факту
    float    price;       // цена кВт·ч из сети, ₽; 0 — экономию не считать
    bool     autoOn;      // ночная зарядка от сети, если завтра пасмурно
    float    autoBelow;   // «пасмурно» — прогноз выработки ниже, кВт·ч
    uint8_t  autoFrom, autoTo;  // ночное окно, часы местного времени
    uint8_t  autoSoc;     // зарядили до — возвращаем PV раньше конца окна, %

    // Облачность — переключение OPP днём по факту выработки панелей.
    // cloudOn=false — ничего не трогаем. Пороги — от ожидаемой выработки:
    // pvW < expected × cloudLow → GRD, pvW > expected × cloudHigh → PBG.
    bool     cloudOn;     // включена автоматика
    float    cloudLow;    // «облачно»: pvW < expected × cloudLow (по умолч. 0.3)
    float    cloudHigh;   // «ясно»: pvW > expected × cloudHigh (по умолч. 0.6)
    uint8_t  cloudSocMin; // минимальный SOC для переключения на PBG (по умолч. 30%)
    uint8_t  cloudSocMax; // максимальный SOC для переключения на GRD (по умолч. 90%)
    uint8_t  cloudFrom;   // начало окна, часы местного (по умолч. 8)
    uint8_t  cloudTo;     // конец окна, часы местного (по умолч. 18)

    // Свой счёт заряда (battsoc.cpp). Ёмкость 0 — выключен, процент берётся у BMS.
    float    bCapAh;      // ёмкость батареи, А·ч
    uint16_t bFullMv;     // 100 % — средняя ячейка, мВ
    uint16_t bEmptyMv;    // 0 % — средняя ячейка, мВ
};

void    cfgInit();
Config& cfg();
void    cfgSave();
void    cfgFactoryReset();
