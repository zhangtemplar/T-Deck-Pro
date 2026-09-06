/**
 * @file      app_config.cpp
 * @brief     SD-card settings with compile-time fallbacks. See app_config.h.
 */
#include "app_config.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <ctype.h>

#include "utilities.h"
#include "config_keys.h"

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
extern void shared_spi_prepare_device(int cs_pin);
extern bool sd_ensure_mounted(void);

/* Small and fixed: this holds a handful of short strings, and a failure to
 * allocate at boot would be a worse outcome than a cap nobody reaches. */
#define CFG_MAX_ENTRIES 24
#define CFG_ARENA       2048
#define CFG_MAX_LINE    256

typedef struct { const char *key; const char *val; } entry_t;

static entry_t s_ent[CFG_MAX_ENTRIES];
static int     s_n;
static char    s_arena[CFG_ARENA];
static size_t  s_used;
static bool    s_loaded;

/* Compile-time defaults, so an existing build keeps working untouched. The
 * table is the single place that knows both spellings of each setting. */
static const entry_t k_builtin[] = {
#ifdef WIFI_SSID
    { CFG_WIFI_SSID,        WIFI_SSID },
#endif
#ifdef WIFI_PASSWORD
    { CFG_WIFI_PASSWORD,    WIFI_PASSWORD },
#endif
#ifdef WIFI_SSID2
    { CFG_WIFI_SSID2,       WIFI_SSID2 },
#endif
#ifdef WIFI_PASSWORD2
    { CFG_WIFI_PASSWORD2,   WIFI_PASSWORD2 },
#endif
#ifdef GEMINI_API_KEY
    { CFG_GEMINI_KEY,       GEMINI_API_KEY },
#endif
#ifdef OWM_API_KEY
    { CFG_OWM_KEY,          OWM_API_KEY },
#endif
#ifdef CALENDARIFIC_API_KEY
    { CFG_CALENDARIFIC_KEY, CALENDARIFIC_API_KEY },
#endif
#ifdef UBLOX_ASSISTNOW_TOKEN
    { CFG_ASSISTNOW_TOKEN,  UBLOX_ASSISTNOW_TOKEN },
#endif
    { NULL, NULL }
};

static const char *builtin_get(const char *key)
{
    for (int i = 0; k_builtin[i].key; i++)
        if (strcmp(k_builtin[i].key, key) == 0) return k_builtin[i].val;
    return NULL;
}

static const char *sd_get(const char *key)
{
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_ent[i].key, key) == 0) return s_ent[i].val;
    return NULL;
}

static char *arena_dup(const char *s, size_t len)
{
    if (s_used + len + 1 > CFG_ARENA) return NULL;
    char *p = s_arena + s_used;
    memcpy(p, s, len);
    p[len] = '\0';
    s_used += len + 1;
    return p;
}

static char *trim(char *s, size_t *len_out)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    /* Quotes are stripped last, so "  spaced  " keeps its spaces but an
     * unquoted value does not. */
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s++;
        n -= 2;
    }
    *len_out = n;
    return s;
}

static void parse_line(char *line)
{
    while (*line && isspace((unsigned char)*line)) line++;
    if (!*line || *line == '#' || *line == ';' || *line == '[') return;

    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';

    size_t klen = 0, vlen = 0;
    char *k = trim(line, &klen);
    char *v = trim(eq + 1, &vlen);
    if (!klen) return;

    /* Keys are matched case-insensitively, so WIFI_SSID and wifi_ssid both
     * work — the .h spelling is the one people will copy. */
    for (size_t i = 0; i < klen; i++) k[i] = (char)tolower((unsigned char)k[i]);

    if (s_n >= CFG_MAX_ENTRIES) return;
    char *ks = arena_dup(k, klen);
    char *vs = arena_dup(v, vlen);
    if (!ks || !vs) return;

    /* A repeated key replaces the earlier one rather than being ignored. */
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_ent[i].key, ks) == 0) { s_ent[i].val = vs; return; }
    }
    s_ent[s_n].key = ks;
    s_ent[s_n].val = vs;
    s_n++;
}

void cfg_load(void)
{
    s_n = 0;
    s_used = 0;
    s_loaded = false;

    if (!sd_ensure_mounted()) {
        Serial.println("[CFG] no card; using values compiled into the firmware");
        return;
    }

    shared_spi_lock();
    shared_spi_prepare_device(BOARD_SD_CS);
    bool present = SD.exists(CFG_PATH);
    File f = present ? SD.open(CFG_PATH, FILE_READ) : File();
    if (f) {
        char line[CFG_MAX_LINE];
        size_t n = 0;
        while (f.available()) {
            int c = f.read();
            if (c < 0) break;
            if (c == '\n' || c == '\r') {
                line[n] = '\0';
                if (n) parse_line(line);
                n = 0;
            } else if (n < sizeof(line) - 1) {
                line[n++] = (char)c;
            }
            /* A line longer than the buffer is truncated rather than split,
             * so a runaway file cannot inject a bogus key. */
        }
        line[n] = '\0';
        if (n) parse_line(line);
        f.close();
        s_loaded = true;
    }
    shared_spi_unlock();

    if (!present) {
        Serial.println("[CFG] no " CFG_PATH "; using values compiled into the firmware");
        return;
    }
    if (!s_loaded) {
        Serial.println("[CFG] " CFG_PATH " could not be read");
        return;
    }

    /* Names and sources only. Printing a value here would put a WiFi password
     * and every API key into the serial log. */
    Serial.printf("[CFG] %s: %d setting%s\n", CFG_PATH, s_n, s_n == 1 ? "" : "s");
    for (int i = 0; i < s_n; i++)
        Serial.printf("[CFG]   %s = <%u chars>\n",
                      s_ent[i].key, (unsigned)strlen(s_ent[i].val));
}

const char *cfg_get(const char *key)
{
    const char *v = sd_get(key);
    if (v && *v) return v;
    v = builtin_get(key);
    return (v && *v) ? v : "";
}

bool cfg_has(const char *key) { return cfg_get(key)[0] != '\0'; }

const char *cfg_source(const char *key)
{
    const char *v = sd_get(key);
    if (v && *v) return "sd";
    v = builtin_get(key);
    return (v && *v) ? "firmware" : "unset";
}
