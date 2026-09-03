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
