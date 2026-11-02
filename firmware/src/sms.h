// SMS через модем SIM800L (плата «V2.0, 5V»): тревоги, когда интернета нет,
// и команды с телефона.
//
// Подключение к YD-ESP32-S3: SIM_RXD ← GPIO 5, SIM_TXD → GPIO 6, RST ← GPIO 7,
// VDD (уровень сигналов) → 3V3, 5V — от отдельного блока 5 В / 2 А (модем
// при передаче берёт до 2 А — от платы он будет сбрасываться), GND — общий.
// Сеть — только 2G (GSM 900/1800).
//
// Тревоги (smsAlert — отключение и возврат сети, заряд ниже порога, авария
// инвертора и BMS, нет связи с инвертором) уходят на номера из настроек:
//   режим 1 — только если бот не достучался: тревога ждёт N минут, и если
//             за это время Telegram/MAX ни разу не ответили, — SMS; когда
//             интернет вернётся, бот пришлёт список ушедших SMS;
//   режим 2 — всегда, сразу, вдобавок к боту.
// С тех же номеров модем принимает команды: «статус», «баланс», «помощь».
// SMS — в UCS2 (кириллица), до 70 знаков в одной, длинное — до трёх штук.
//
// Модема нет — модуль раз в 2 минуты пробует найти его снова и больше ничего
// не делает. На обычном ESP32 (первое устройство) — заглушки.
#pragma once
#include <Arduino.h>

void   smsStart();
void   smsAlert(const String& text);   // важное событие — по правилам режима
String smsJson();                      // для страницы: модем, сеть, настройки (номера — со звёздочками)
String smsSet(int mode, const String* nums, int delayMin, const String* ussd);   // nullptr / −1 — не менять
void   smsTest();                      // прислать проверочное SMS на все номера
void   smsBalance();                   // запросить баланс (USSD)
String smsLine();                      // для дисплея: «−73 дБм · MTS RUS»; "" — модема нет

// Разбор — отдельно, проверяется на компьютере.
String smsUcs2Enc(const String& utf8);       // UTF-8 → «04210442…»; вне BMP и вариационные — выкинуты
String smsUcs2Dec(const String& hex);        // обратно; "" — это не UCS2-строка
int    smsUtf8Len(const String& s);          // знаков (не байт)
String smsUtf8Cut(const String& s, int from, int n);
bool   smsNumMatch(const String& a, const String& b);   // по последним 10 цифрам
int    smsCsqDbm(const String& resp);        // «+CSQ: 17,0» → −79; 0 — неизвестно
int    smsCreg(const String& resp);          // «+CREG: 0,1» → 1; −1 — нет ответа
String smsCops(const String& resp);          // «+COPS: 0,0,"MTS RUS"» → «MTS RUS»
String smsCusd(const String& line);          // «+CUSD: 0,"…",72» → текст (UCS2 раскодирован)
String smsLower(const String& s);            // строчные, в т. ч. кириллица
