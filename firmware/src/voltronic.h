// Протокол Voltronic (Axpert, Asterion PLUS и родня): текстовые команды.
//
//   запрос   QPIGS<CRC16-XMODEM><0x0D>
//   ответ    (поле поле поле …<CRC16-XMODEM><0x0D>
//
// Ни регистров, ни адресов: строка полей в фиксированном порядке. Разбор —
// это разбиение по пробелам, и позиция поля и есть его смысл.
//
// Здесь только чистая логика, без Arduino: то же самое собирается и на
// компьютере, поэтому разбор проверяется тестами до всякого железа.
#pragma once
#include <stdint.h>
#include <stddef.h>

uint16_t vtCrc16(const uint8_t* d, size_t n);

// Собрать команду: имя + контрольная сумма + возврат каретки.
size_t vtBuildCommand(const char* cmd, uint8_t* out, size_t cap);

// Проверить обрамление ответа и вернуть тело без скобки и суммы.
// Возвращает длину тела либо 0, если ответ неполон или сумма не сошлась.
size_t vtUnwrap(const uint8_t* frame, size_t n, const char** body);

// Разобранный ответ QPIGS. Названия — по смыслу полей, а не по позициям:
// позиции живут в одном месте, в vtParseQpigs.
struct VtQpigs {
    float   gridV, gridHz;
    float   outV, outHz;
    int     outVA, outW;         // полная и активная мощность нагрузки
    int     loadPct;
    int     busV;
    float   battV;
    int     battChargeA;         // ток заряда батареи
    int     battSoc;             // заряд батареи, проценты
    int     tempC;               // температура радиатора инвертора
    float   pvA, pvV;
    float   sccBattV;
    int     battDischargeA;      // ток разряда батареи
    uint8_t status;              // биты b7…b0
    int     pvW;                 // мощность заряда от панелей
    bool    ok;
};

// Разобрать тело ответа QPIGS. false — полей меньше, чем нужно.
bool vtParseQpigs(const char* body, size_t len, VtQpigs* out);

// Режим из ответа QMOD: одна буква. P — включение, S — ожидание,
// L — от сети, B — от батареи, F — авария, H — энергосбережение.
bool vtParseQmod(const char* body, size_t len, char* mode);
