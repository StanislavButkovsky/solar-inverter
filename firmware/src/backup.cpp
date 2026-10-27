// Резервная копия настроек. Описание — в backup.h.
//
// Формат — JSON, все значения строками, чтобы разбирать его без библиотек:
//   {"v":1,"made":…,"fw":"…",
//    "cfg":{"ssid":"…","wpass":"…",…},
//    "nvs":[["пространство","ключ","тип","значение"],…]}
// Типы NVS: b — байты (hex), d — double, u — uint32, i — int32, c — uint8,
// q — uint64. Восстанавливаются только ключи из списка NVSK — чужого в NVS
// копия не запишет.
#include "backup.h"
#include "cfg.h"
#include "json.h"
#include "net.h"
#include "ota.h"
#include <Preferences.h>
#include <time.h>

struct NvsKey { const char* ns; const char* key; char type; };
static const NvsKey NVSK[] = {
    { "sched",   "r2",    'b' },   // расписание режимов
    { "sched",   "r",     'b' },   // оно же прежнего вида — из старых копий
    { "invprof", "ver",   'q' },   // настройки инвертора, сверенные с панелью
    { "wx",      "k",     'b' },   // коэффициент панелей по дням
    { "wx",      "kday",  'i' },
    { "wx",      "kv",    'c' },
    { "batt",    "out",   'd' },   // износ батареи
    { "batt",    "in",    'd' },
    { "batt",    "since", 'u' },
    { "batt",    "est",   'b' },
    { "pace",    "proto", 'c' },   // BMS по RS485: протокол и адреса
    { "pace",    "addr",  'b' },
};
static const uint8_t NNVSK = sizeof(NVSK) / sizeof(NVSK[0]);

static const char HX[] = "0123456789abcdef";

String backupName() {
    time_t loc = time(nullptr) + 3 * 3600;
    struct tm lt;
    gmtime_r(&loc, &lt);
    char b[40];
    snprintf(b, sizeof(b), "solar-backup-%04d-%02d-%02d.json", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return b;
}

static void kv(String& j, bool& first, const char* k, const String& v) {
    if (!first) j += ",";
    first = false;
    j += "\"" + String(k) + "\":\"" + jsonEsc(v) + "\"";
}

String backupJson() {
    const Config& c = cfg();
    String j;
    j.reserve(2048);
    j = "{\"v\":\"1\",\"made\":\"" + String((uint32_t)time(nullptr)) + "\",\"fw\":\"" + jsonEsc(fwBuild()) + "\",\"cfg\":{";
    bool f = true;
    kv(j, f, "ssid", c.ssid);         kv(j, f, "wpass", c.pass);
    kv(j, f, "sip", c.sip);           kv(j, f, "sgw", c.sgw);
    kv(j, f, "smask", c.smask);       kv(j, f, "sdns", c.sdns);
    kv(j, f, "bmsMac", c.bmsMac);     kv(j, f, "invMac", c.invMac);
    kv(j, f, "tgToken", c.tgToken);   kv(j, f, "tgChat", c.tgChat);    kv(j, f, "tgLow", String(c.tgSocLow));
    kv(j, f, "mxToken", c.mxToken);   kv(j, f, "mxChat", c.mxChat);
    kv(j, f, "lat", String(c.lat, 5)); kv(j, f, "lon", String(c.lon, 5));
    kv(j, f, "kwp", String(c.pvKwp, 2)); kv(j, f, "price", String(c.price, 2));
    kv(j, f, "autoOn", c.autoOn ? "1" : "0"); kv(j, f, "autoBelow", String(c.autoBelow, 1));
    kv(j, f, "autoFrom", String(c.autoFrom)); kv(j, f, "autoTo", String(c.autoTo)); kv(j, f, "autoSoc", String(c.autoSoc));
    j += "},\"nvs\":[";
    bool first = true;
    for (uint8_t i = 0; i < NNVSK; i++) {
        const NvsKey& k = NVSK[i];
        Preferences p;
        if (!p.begin(k.ns, true)) continue;
        if (!p.isKey(k.key)) { p.end(); continue; }
        String v;
        switch (k.type) {
        case 'b': {
            size_t n = p.getBytesLength(k.key);
            if (n > 512) n = 512;
            uint8_t buf[512];
            p.getBytes(k.key, buf, n);
            for (size_t q = 0; q < n; q++) { v += HX[buf[q] >> 4]; v += HX[buf[q] & 15]; }
            break;
        }
        case 'd': v = String(p.getDouble(k.key, 0), 4); break;
        case 'u': v = String((uint32_t)p.getULong(k.key, 0)); break;
        case 'i': v = String((long)p.getInt(k.key, 0)); break;
        case 'c': v = String(p.getUChar(k.key, 0)); break;
        case 'q': { char b[24]; snprintf(b, sizeof(b), "%llu", (unsigned long long)p.getULong64(k.key, 0)); v = b; break; }
        }
        p.end();
        if (!first) j += ",";
        first = false;
        j += "[\"" + String(k.ns) + "\",\"" + String(k.key) + "\",\"" + String(k.type) + "\",\"" + v + "\"]";
    }
    return j + "]}";
}

// ------------------------------------------------------------------ разбор

// Строка JSON с позиции pos (на открывающей кавычке); pos — за закрывающей.
static bool jstr(const String& s, int& pos, String& out) {
    if (pos >= (int)s.length() || s[pos] != '"') return false;
    out = "";
    for (pos++; pos < (int)s.length(); pos++) {
        char c = s[pos];
        if (c == '"') { pos++; return true; }
        if (c == '\\' && pos + 1 < (int)s.length()) {
            char e = s[++pos];
            if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else if (e == 'u' && pos + 4 < (int)s.length()) {
                unsigned cp = strtoul(s.substring(pos + 1, pos + 5).c_str(), nullptr, 16);
                pos += 4;
                if (cp < 0x80) out += (char)cp;              // кириллица в копии — в UTF-8 как есть, сюда не попадает
            } else out += e;
        } else out += c;
    }
    return false;
}
static void skipWs(const String& s, int& pos) { while (pos < (int)s.length() && isspace((uint8_t)s[pos])) pos++; }

// Значение ключа в объекте «"cfg":{…}» — только строки.
static bool cfgVal(const String& s, int from, int to, const char* key, String& out) {
    String k = String("\"") + key + "\"";
    int p = s.indexOf(k, from);
    if (p < 0 || p > to) return false;
    p += k.length();
    skipWs(s, p);
    if (p >= (int)s.length() || s[p] != ':') return false;
    p++;
    skipWs(s, p);
    return jstr(s, p, out);
}

String backupRestore(const String& s) {
    if (s.indexOf("\"cfg\"") < 0 || s.indexOf("\"nvs\"") < 0) return "это не резервная копия модуля";
    // Объект cfg кончается там, где начинается nvs: искать «}» нельзя — она
    // может быть в пароле Wi-Fi.
    int cf = s.indexOf("\"cfg\"");
    int cfEnd = s.indexOf("\"nvs\"", cf);
    if (cfEnd < 0) return "копия повреждена";

    Config& c = cfg();
    String v;
    if (cfgVal(s, cf, cfEnd, "ssid", v))    c.ssid = v;
    if (cfgVal(s, cf, cfEnd, "wpass", v))   c.pass = v;
    // Адрес — только целиком и только проверенный: кривой сделал бы модуль
    // недоступным сразу после восстановления. Копии без него адрес не трогают.
    if (cfgVal(s, cf, cfEnd, "sip", v)) {
        String gw, mask, dns;
        cfgVal(s, cf, cfEnd, "sgw", gw);
        cfgVal(s, cf, cfEnd, "smask", mask);
        cfgVal(s, cf, cfEnd, "sdns", dns);
        String err = netSetStatic(v, gw, mask, dns);
        if (err.length()) Serial.printf("[копия] адрес из копии не принят: %s\n", err.c_str());
    }
    if (cfgVal(s, cf, cfEnd, "bmsMac", v))  c.bmsMac = v;
    if (cfgVal(s, cf, cfEnd, "invMac", v))  c.invMac = v;
    if (cfgVal(s, cf, cfEnd, "tgToken", v)) c.tgToken = v;
    if (cfgVal(s, cf, cfEnd, "tgChat", v))  c.tgChat = v;
    if (cfgVal(s, cf, cfEnd, "mxToken", v)) c.mxToken = v;
    if (cfgVal(s, cf, cfEnd, "mxChat", v))  c.mxChat = v;
    auto num = [&](const char* k, float lo, float hi, float& dst) {
        String t;
        if (!cfgVal(s, cf, cfEnd, k, t) || !t.length()) return;
        float x = t.toFloat();
        if (x >= lo && x <= hi) dst = x;
    };
    float f;
    f = c.tgSocLow;  num("tgLow", 5, 90, f);    c.tgSocLow = (uint8_t)f;
    num("lat", -90, 90, c.lat);
    num("lon", -180, 180, c.lon);
    num("kwp", 0, 100, c.pvKwp);
    num("price", 0, 100, c.price);
    num("autoBelow", 0, 200, c.autoBelow);
    f = c.autoFrom;  num("autoFrom", 0, 23, f);  c.autoFrom = (uint8_t)f;
    f = c.autoTo;    num("autoTo", 0, 23, f);    c.autoTo = (uint8_t)f;
    f = c.autoSoc;   num("autoSoc", 30, 100, f); c.autoSoc = (uint8_t)f;
    if (cfgVal(s, cf, cfEnd, "autoOn", v)) c.autoOn = v == "1";
    cfgSave();

    // NVS: четвёрки строк, только ключи из белого списка.
    int p = s.indexOf("\"nvs\"");
    p = s.indexOf('[', p);
    uint8_t restored = 0;
    while (p >= 0 && p < (int)s.length()) {
        int a = s.indexOf('[', p + 1);
        if (a < 0) break;
        int q = a + 1;
        String e[4];
        bool ok = true;
        for (int i = 0; i < 4 && ok; i++) {
            skipWs(s, q);
            ok = jstr(s, q, e[i]);
            skipWs(s, q);
            if (ok && i < 3) { if (q < (int)s.length() && s[q] == ',') q++; else ok = false; }
        }
        p = s.indexOf(']', q);
        if (!ok || p < 0) break;
        const NvsKey* k = nullptr;
        for (uint8_t i = 0; i < NNVSK; i++)
            if (e[0] == NVSK[i].ns && e[1] == NVSK[i].key && e[2].length() == 1 && e[2][0] == NVSK[i].type) k = &NVSK[i];
        if (!k) continue;
        Preferences pr;
        if (!pr.begin(k->ns, false)) continue;
        const String& val = e[3];
        switch (k->type) {
        case 'b': {
            uint8_t buf[512];
            size_t n = val.length() / 2;
            if (n > sizeof(buf)) n = sizeof(buf);
            for (size_t i = 0; i < n; i++) buf[i] = strtoul(val.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
            pr.putBytes(k->key, buf, n);
            break;
        }
        case 'd': pr.putDouble(k->key, val.toDouble()); break;
        case 'u': pr.putULong(k->key, strtoul(val.c_str(), nullptr, 10)); break;
        case 'i': pr.putInt(k->key, val.toInt()); break;
        case 'c': pr.putUChar(k->key, (uint8_t)val.toInt()); break;
        case 'q': pr.putULong64(k->key, strtoull(val.c_str(), nullptr, 10)); break;
        }
        pr.end();
        restored++;
    }
    Serial.printf("[копия] восстановлено: настройки и %u ключей NVS\n", restored);
    return "";
}
