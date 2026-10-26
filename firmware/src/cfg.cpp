#include "cfg.h"
#include <Preferences.h>

static Preferences prefs;
static Config      g_cfg;

void cfgInit() {
    prefs.begin("solar", false);
    g_cfg.ssid   = prefs.getString("ssid", "");
    g_cfg.pass   = prefs.getString("pass", "");
    g_cfg.sip    = prefs.getString("sip", "");
    g_cfg.sgw    = prefs.getString("sgw", "");
    g_cfg.smask  = prefs.getString("smask", "");
    g_cfg.sdns   = prefs.getString("sdns", "");
    g_cfg.wifiSleep = prefs.getBool("wsleep", false);
    g_cfg.bmsMac = prefs.getString("bmsmac", "");
    g_cfg.invMac = prefs.getString("invmac", "");
    g_cfg.webPass= prefs.getString("webpass", "");
    g_cfg.pollMs = prefs.getUShort("pollms", 300);
    g_cfg.apAfterSave = prefs.getBool("apafter", false);
    g_cfg.tgToken = prefs.getString("tgtoken", "");
    g_cfg.tgChat  = prefs.getString("tgchat", "");
    g_cfg.tgSocLow = prefs.getUChar("tgsoclow", 20);
    g_cfg.lat       = prefs.getFloat("lat", 0);
    g_cfg.lon       = prefs.getFloat("lon", 0);
    g_cfg.pvKwp     = prefs.getFloat("pvkwp", 0);
    g_cfg.price     = prefs.getFloat("price", 0);
    g_cfg.autoOn    = prefs.getBool("autoon", false);
    g_cfg.autoBelow = prefs.getFloat("autobelow", 5);
    g_cfg.autoFrom  = prefs.getUChar("autofrom", 23);
    g_cfg.autoTo    = prefs.getUChar("autoto", 7);
    g_cfg.autoSoc   = prefs.getUChar("autosoc", 95);
    if (g_cfg.pollMs < 100) g_cfg.pollMs = 100;
}

Config& cfg() { return g_cfg; }

void cfgSave() {
    prefs.putString("ssid",   g_cfg.ssid);
    prefs.putString("pass",   g_cfg.pass);
    prefs.putString("sip",    g_cfg.sip);
    prefs.putString("sgw",    g_cfg.sgw);
    prefs.putString("smask",  g_cfg.smask);
    prefs.putString("sdns",   g_cfg.sdns);
    prefs.putBool("wsleep",   g_cfg.wifiSleep);
    prefs.putString("bmsmac", g_cfg.bmsMac);
    prefs.putString("invmac", g_cfg.invMac);
    prefs.putString("webpass",g_cfg.webPass);
    prefs.putUShort("pollms", g_cfg.pollMs);
    prefs.putBool("apafter", g_cfg.apAfterSave);
    prefs.putString("tgtoken", g_cfg.tgToken);
    prefs.putString("tgchat", g_cfg.tgChat);
    prefs.putUChar("tgsoclow", g_cfg.tgSocLow);
    prefs.putFloat("lat", g_cfg.lat);
    prefs.putFloat("lon", g_cfg.lon);
    prefs.putFloat("pvkwp", g_cfg.pvKwp);
    prefs.putFloat("price", g_cfg.price);
    prefs.putBool("autoon", g_cfg.autoOn);
    prefs.putFloat("autobelow", g_cfg.autoBelow);
    prefs.putUChar("autofrom", g_cfg.autoFrom);
    prefs.putUChar("autoto", g_cfg.autoTo);
    prefs.putUChar("autosoc", g_cfg.autoSoc);
}

void cfgFactoryReset() {
    prefs.clear();
    g_cfg = Config();
    g_cfg.pollMs = 300;
    g_cfg.tgSocLow = 20;
    g_cfg.autoBelow = 5; g_cfg.autoFrom = 23; g_cfg.autoTo = 7; g_cfg.autoSoc = 95;
}
