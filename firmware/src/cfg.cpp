#include "cfg.h"
#include <Preferences.h>

static Preferences prefs;
static Config      g_cfg;

void cfgInit() {
    prefs.begin("solar", false);
    g_cfg.ssid   = prefs.getString("ssid", "");
    g_cfg.pass   = prefs.getString("pass", "");
    g_cfg.bmsMac = prefs.getString("bmsmac", "");
    g_cfg.webPass= prefs.getString("webpass", "");
    g_cfg.pollMs = prefs.getUShort("pollms", 300);
    g_cfg.apAfterSave = prefs.getBool("apafter", false);
    if (g_cfg.pollMs < 100) g_cfg.pollMs = 100;
}

Config& cfg() { return g_cfg; }

void cfgSave() {
    prefs.putString("ssid",   g_cfg.ssid);
    prefs.putString("pass",   g_cfg.pass);
    prefs.putString("bmsmac", g_cfg.bmsMac);
    prefs.putString("webpass",g_cfg.webPass);
    prefs.putUShort("pollms", g_cfg.pollMs);
    prefs.putBool("apafter", g_cfg.apAfterSave);
}

void cfgFactoryReset() {
    prefs.clear();
    g_cfg = Config();
    g_cfg.pollMs = 300;
}
