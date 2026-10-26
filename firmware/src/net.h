#pragma once
#include <Arduino.h>

void   netStart();          // поднять Wi-Fi (STA, при неудаче — SoftAP)
void   netLoop();           // вызывать из loop(): переподключение
bool   netIsAp();
String netIp();
String netStatus();

// Поиск сетей для формы настройки. Сканирование асинхронное: блокирующее
// на 3-4 секунды подвесило бы веб-сервер и оборвало бы страницу настройщика.
bool   netToggleAp();      // кнопка: включить или выключить точку доступа
bool   netApActive();      // точка доступа сейчас поднята
void   netScanStart();
String netScanJson();

// Свой адрес вместо DHCP. Пустой ip — вернуть DHCP. Проверяет, что адрес и
// шлюз из одной подсети, и пишет в cfg(); сохранить и перезагрузить — вызывающему.
// Пустая строка — принято, иначе — что не так.
String netSetStatic(const String& ip, const String& gw, const String& mask, const String& dns);
String netAddrJson();      // {"static":…,"ip","gw","mask","dns"} — для страницы

// Сон Wi-Fi: включить/выключить сразу и запомнить. Возвращает режим, который
// драйвер реально поставил: none, min_modem, max_modem.
String netSetSleep(bool on);
String netSleepState();
