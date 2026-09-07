// Транспорт до инвертора. Реализация выбирается на сборке:
//   inv_ble.cpp — донгл Eybond по Bluetooth (первый инвертор)
//   inv_usb.cpp — встроенный мост CH340 по USB (второй, Hiden HS35)
//
// Выше этого уровня код одинаков: тот же Modbus, тот же опрос по кругу,
// тот же снапшот. Меняются только провода.
#pragma once
#include <Arduino.h>

void invTrBegin();                              // однократная подготовка
bool invTrConnected();                          // готов ли канал
bool invTrConnect();                            // попытка поднять канал
void invTrSend(const uint8_t* frame, size_t n); // отправить кадр

// Транспорт зовёт это на каждый пришедший кусок данных.
void invOnBytes(const uint8_t* data, size_t n);
