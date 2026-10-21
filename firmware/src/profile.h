// Профиль инвертора: что спрашивать и как понимать ответ.
//
// Первые два инвертора говорят Modbus RTU и различаются лишь скоростью,
// адресом и картой регистров. Третий говорит текстовым протоколом Voltronic,
// где регистров нет вовсе — есть команды вроде QPIGS и строка полей в ответе.
//
// Поэтому профиль описан не «блоками регистров», а шагами опроса: собрать
// запрос шага, скормить пришедшие байты, получить «ответ разобран». Что
// внутри — кадр Modbus или строка с пробелами — знает только сам профиль.
//
// Реализации выбираются на сборке:
//   profile_eybond.cpp    первый инвертор, Modbus через донгл Eybond
//   profile_hs35.cpp      второй, Hiden HS35 по USB: JsdSolar G-ASCII, запасной — Q1
//   profile_must.cpp      MUST PV18 по Modbus — для HS35 через RS485/RS232,
//                         если порт освободится (флаг TARGET_MUST)
//   profile_voltronic.cpp третий, Asterion PLUS = Voltronic Axpert, ASCII
#pragma once
#include <Arduino.h>

extern const uint8_t INV_NSTEPS;        // сколько шагов в круге опроса
extern const char*   INV_PROFILE_NAME;

// Собрать запрос очередного шага. Возвращает длину; 0 — шаг пропускается.
size_t invBuildRequest(uint8_t step, uint8_t* out, size_t cap);

// Скормить пришедшие байты. true — ответ собран, разобран и уложен в снапшот.
bool   invFeed(uint8_t step, const uint8_t* data, size_t n);

// Сбросить накопленное: перед новым запросом и при обрыве связи.
void   invResetStream();

// Параметры провода — только у профилей по USB (inv_usb.cpp). Пауза между
// байтами нужна HS35: целую строку подряд его порт не принимает, отвечает NAK.
extern const uint32_t INV_BAUD;
extern const uint16_t INV_GAP_MS;

// Что профиль узнал о своём инверторе — для status в терминале. Профилю,
// которому сказать нечего, определять не нужно: пустая версия в inv.cpp.
String invProfileReport();
String invTrace();                       // журнал обмена — последние строки, по порядку

// Переключение настроек — только у профилей, которые это умеют (HS35).
// what: 0 — приоритет работы (OPP), 1 — приоритет зарядки (CHP).
// Вернёт "" — принято в работу, иначе причину отказа.
String   invSetSetting(uint8_t what, uint8_t code);
String   invSetStatusJson();             // {"state":…,"msg":…,"seq":N}
uint32_t invSetSeq();                    // растёт на каждом завершённом переключении
String   invSetLastMsg();

// Остальные настройки инвертора (у HS35 — hs35_settings.h): все значения,
// пределы и почему нельзя писать; запись; отметка «сверено с экраном».
String   invSettingsJson();
String   invSettingsText();              // для бота
String   invSettingWrite(const String& id, const String& value);   // "" — принято в работу
String   invSettingVerify(const String& id, bool on);
