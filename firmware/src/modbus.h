// Modbus RTU: то, что одинаково для обоих инверторов.
//
// Кадры одни и те же, отличаются только провода под ними: у первого инвертора
// это BLE через донгл Eybond, у второго — USB через встроенный мост CH340.
// Поэтому разбор и подсчёт контрольной суммы живут здесь, отдельно от
// транспорта, и переиспользуются целиком.
#pragma once
#include <Arduino.h>

uint16_t mbCrc16(const uint8_t* d, size_t n);

// Собрать запрос чтения регистров хранения. Буфер должен быть на 8 байт.
void mbBuildRead(uint8_t* out, uint8_t slave, uint16_t start, uint16_t count);

// Сборщик ответа из кусков: и BLE, и USB отдают данные произвольными порциями.
//
// Некоторые мосты добавляют перед кадром постоянный префикс — у Eybond это
// восемь ASCII-единиц. Он задаётся при создании и отрезается здесь же.
class MbAssembler {
public:
    void begin(uint8_t slave, const uint8_t* prefix, uint8_t prefixLen);
    void reset() { _len = 0; }

    // Скормить пришедшие байты. Вернёт true, когда собран корректный ответ:
    // тогда data/words указывают на полезную нагрузку.
    bool feed(const uint8_t* p, size_t n, const uint8_t** data, uint8_t* words);

    uint32_t bad = 0;      // ответов с несошедшейся контрольной суммой

private:
    uint8_t  _buf[300];
    size_t   _len = 0;
    uint8_t  _slave = 1;
    uint8_t  _prefix[8];
    uint8_t  _prefixLen = 0;
};
