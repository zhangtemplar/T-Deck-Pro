/**
 * @file      dict_lookup.cpp
 * @author    LilyGo
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-04-01
 * @brief     Dictionary lookup backend implementation.
 *
 * Supports:
 *   - StarDict format (.ifo/.idx/.dict on SD card at /stardict/)
 *     Multiple dictionaries are supported - scans all .ifo files.
 *   - Custom binary format (dict_en.idx + dict_en.dat on SD root)
 *   - Online lookup via Free Dictionary API
 */
#include "dict_lookup.h"
#include "http_utils.h"

#include <string.h>
#include <time.h>

/* Pure row formatting, deliberately outside the ARDUINO guard so it builds
 * and can be tested on the host alongside the rest of the parsing code. */
int dict_history_format_row(char *out, size_t cap, const char *word, const struct tm *tm)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    /* Shortest possible row is "", so anything smaller cannot be represented.
     * Checking up front also keeps the cap arithmetic below from underflowing,
     * since cap is unsigned. */
    if (cap < 4) return 0;

    size_t o = 0;
    out[o++] = '"';

    /* RFC 4180: wrap in quotes and double any quote inside, so a word or
     * phrase containing a comma or quote doesn't split the row. */
    for (const char *p = word ? word : ""; *p; p++) {
        size_t need = (*p == '"') ? 2u : 1u;
        if (o + need > cap - 3) break;       /* room for closing quote, comma, NUL */
        if (*p == '"') out[o++] = '"';
        out[o++] = *p;
    }

    out[o++] = '"';
    out[o++] = ',';
    out[o] = '\0';

    /* strftime leaves the buffer undefined if the result does not fit, so only
     * call it when the full timestamp definitely does. */
    if (tm && (cap - o) >= sizeof("YYYY-MM-DD HH:MM:SS")) {
        o += strftime(out + o, cap - o, "%Y-%m-%d %H:%M:%S", tm);
    }
    out[o] = '\0';
    return (int)o;
}

#ifdef ARDUINO
#include <SD.h>
#include <WiFi.h>
#include <Preferences.h>
#include <cJSON.h>

extern void shared_spi_lock(void);
extern void shared_spi_unlock(void);
#include "utilities.h"
static bool sd_care_init(void) {
    shared_spi_lock();
    bool ok = SD.begin(BOARD_SD_CS, SPI, 1000000);
    shared_spi_unlock();
    return ok;
}


// ---- Multi-StarDict support ----

static dict_info_t stardict_dicts[MAX_STARDICT_DICTS];
static int stardict_count = 0;
static bool stardict_scanned = false;
/* Which dictionaries take part in lookups; all on until a selection is read. */
static bool stardict_enabled[MAX_STARDICT_DICTS] = {
    true, true, true, true, true, true, true, true
};

// PSRAM-cached index for fast binary search
struct idx_entry_t {
    const char *word;     // pointer into psram_idx_data buffer
    uint32_t offset;      // offset in .dict file
    uint32_t size;        // size in .dict file
};

static uint8_t *psram_idx_data[MAX_STARDICT_DICTS] = {};
static idx_entry_t *psram_idx_entries[MAX_STARDICT_DICTS] = {};
static uint32_t psram_idx_count[MAX_STARDICT_DICTS] = {};

#define MAX_IDX_FILE_SIZE  (4 * 1024 * 1024)  // 4MB cap per index

static void parse_ifo_file(const char *ifo_path, String &bookname, char &sametypesequence)
{
    File f = SD.open(ifo_path, FILE_READ);
    if (!f) {
        bookname = "Unknown";
        sametypesequence = 'm';
        return;
    }
    bookname = "Unknown";
    sametypesequence = 'm';
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith("bookname=")) {
            bookname = line.substring(9);
        } else if (line.startsWith("sametypesequence=")) {
            String val = line.substring(17);
            if (val.length() > 0) {
                sametypesequence = val.charAt(0);
            }
        }
    }
    f.close();
}

int dict_scan_stardict()
{
    if (stardict_scanned) return stardict_count;
    stardict_scanned = true;
    stardict_count = 0;

    shared_spi_lock();
    bool sd_ok = sd_care_init();
    Serial.printf("[Dictionary] installSD() returned: %d\n", sd_ok);
    if (!sd_ok) {
        Serial.println("[Dictionary] SD card mount failed");
        shared_spi_unlock();
        return 0;
    }
    Serial.println("[Dictionary] SD card mount success");

    if (!SD.exists("/stardict")) {
        Serial.println("[Dictionary] Creating /stardict/ directory");
        SD.mkdir("/stardict");
    }

    File dir = SD.open("/stardict");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[Dictionary] Failed to open /stardict/ directory");
        shared_spi_unlock();
        return 0;
    }

    File entry;
    while ((entry = dir.openNextFile()) && stardict_count < MAX_STARDICT_DICTS) {
        String name = entry.name();
        entry.close();

        if (!name.endsWith(".ifo")) continue;

        // Extract base name (without .ifo extension)
        String base_name = name.substring(0, name.length() - 4);
        String ifo_path = String("/stardict/") + name;
        String idx_path = String("/stardict/") + base_name + ".idx";
        String dict_path = String("/stardict/") + base_name + ".dict";

        // Verify companion files exist
        if (!SD.exists(idx_path.c_str()) || !SD.exists(dict_path.c_str())) {
            continue;
        }

        // Parse bookname and sametypesequence from .ifo
        String bookname;
        char sts = 'm';
        parse_ifo_file(ifo_path.c_str(), bookname, sts);

        dict_info_t &d = stardict_dicts[stardict_count];
        d.name = bookname;
        d.ifo_path = ifo_path;
        d.idx_path = idx_path;
        d.dict_path = dict_path;
        d.sametypesequence = sts;
        stardict_count++;
    }
    dir.close();

    shared_spi_unlock();
    return stardict_count;
}

bool dict_stardict_available()
{
    dict_scan_stardict();
    return stardict_count > 0;
}

int dict_get_stardict_count()
{
    dict_scan_stardict();
    return stardict_count;
}

const char *dict_get_stardict_name(int index)
{
    if (index < 0 || index >= stardict_count) return NULL;
    return stardict_dicts[index].name.c_str();
}

// Convert HTML definition to readable plain text (in-place).
// Handles common tags, HTML entities, and whitespace cleanup.
static void html_to_text(char *s)
{
    char *r = s;    // read pointer
    char *w = s;    // write pointer
    int ol_depth = 0;       // ordered list nesting
    int ol_counter = 0;     // current item number in <ol>
    bool in_blockquote = false;
    bool skip_div_source = false;
    int skip_depth = 0;

    while (*r) {
        if (*r == '<') {
            // Extract tag name (lowercase)
            const char *tag_start = r + 1;
            bool closing = false;
            if (*tag_start == '/') { closing = true; tag_start++; }
            char tag[32];
            int ti = 0;
            const char *tp = tag_start;
            while (*tp && *tp != '>' && *tp != ' ' && *tp != '/' && ti < 30) {
                tag[ti++] = (*tp >= 'A' && *tp <= 'Z') ? (*tp + 32) : *tp;
                tp++;
            }
            tag[ti] = '\0';

            // Check for <div class="source"> to skip
            if (!closing && strcmp(tag, "div") == 0) {
                // Look for class="source" in attributes
                const char *attr = tp;
                const char *end = r;
                while (*end && *end != '>') end++;
                // Simple check: search for "source" between tp and end
                bool is_source = false;
                for (const char *a = attr; a < end; a++) {
                    if (strncmp(a, "source", 6) == 0) { is_source = true; break; }
                }
                if (is_source) {
                    skip_div_source = true;
                    skip_depth = 1;
                    // Advance past >
                    while (*r && *r != '>') r++;
                    if (*r) r++;
                    continue;
                }
            }

            // Skip to end of tag
            while (*r && *r != '>') r++;
            if (*r) r++;

            if (skip_div_source) {
                if (!closing && strcmp(tag, "div") == 0) skip_depth++;
                if (closing && strcmp(tag, "div") == 0) {
                    skip_depth--;
                    if (skip_depth <= 0) skip_div_source = false;
                }
                continue;
            }

            // Handle specific tags
            if (strcmp(tag, "br") == 0) {
                *w++ = '\n';
            } else if (strcmp(tag, "p") == 0 && !closing) {
                if (w > s && *(w - 1) != '\n') *w++ = '\n';
                if (w > s && (w - 1 == s || *(w - 2) != '\n')) *w++ = '\n';
            } else if (strcmp(tag, "ol") == 0) {
                if (!closing) { ol_depth++; ol_counter = 0; if (w > s && *(w - 1) != '\n') *w++ = '\n'; }
                else { ol_depth--; if (ol_depth < 0) ol_depth = 0; }
            } else if (strcmp(tag, "ul") == 0) {
                if (!closing) { if (w > s && *(w - 1) != '\n') *w++ = '\n'; }
            } else if (strcmp(tag, "li") == 0 && !closing) {
                if (w > s && *(w - 1) != '\n') *w++ = '\n';
                if (ol_depth > 0) {
                    ol_counter++;
                    int n = snprintf((char *)w, 12, "%d. ", ol_counter);
                    w += n;
                } else {
                    *w++ = '-'; *w++ = ' ';
                }
            } else if (strcmp(tag, "blockquote") == 0) {
                if (!closing) {
                    if (w > s && *(w - 1) != '\n') *w++ = '\n';
                    *w++ = '"';
                    in_blockquote = true;
                } else {
                    *w++ = '"';
                    *w++ = '\n';
                    in_blockquote = false;
                }
            } else if (strcmp(tag, "h1") == 0 || strcmp(tag, "h2") == 0 ||
                       strcmp(tag, "h3") == 0 || strcmp(tag, "h4") == 0) {
                if (w > s && *(w - 1) != '\n') *w++ = '\n';
            }
            // All other tags: silently stripped
        } else if (*r == '&') {
            if (skip_div_source) { r++; continue; }
            // HTML entity decoding
            if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
            else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
            else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
            else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
            else if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; }
            else if (strncmp(r, "&nbsp;", 6) == 0) { *w++ = ' '; r += 6; }
            else if (strncmp(r, "&#", 2) == 0) {
                // Numeric character reference &#NNN;
                r += 2;
                int val = 0;
                while (*r >= '0' && *r <= '9') { val = val * 10 + (*r - '0'); r++; }
                if (*r == ';') r++;
                if (val > 0 && val < 128) *w++ = (char)val;
            } else {
                *w++ = *r++;
            }
        } else {
            if (skip_div_source) { r++; continue; }
            *w++ = *r++;
        }
    }
    *w = '\0';

    // Whitespace cleanup: collapse 3+ consecutive newlines to 2
    r = s; w = s;
    int nl_count = 0;
    while (*r) {
        if (*r == '\n') {
            nl_count++;
            if (nl_count <= 2) *w++ = *r;
        } else {
            nl_count = 0;
            *w++ = *r;
        }
        r++;
    }
    *w = '\0';

    // Trim leading whitespace
    r = s;
    while (*r == ' ' || *r == '\n' || *r == '\r' || *r == '\t') r++;
    if (r != s) {
        w = s;
        while (*r) *w++ = *r++;
        *w = '\0';
    }

    // Trim trailing whitespace
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

// Compare function for qsort/bsearch on idx_entry_t
static int idx_entry_cmp(const void *a, const void *b)
{
    return strcasecmp(((const idx_entry_t *)a)->word, ((const idx_entry_t *)b)->word);
}

// Load a StarDict .idx file into PSRAM for fast binary search.
// Must be called while SPI lock is held and SD is mounted.
static bool load_stardict_index(int dict_index)
{
    if (dict_index < 0 || dict_index >= stardict_count) return false;
    if (psram_idx_entries[dict_index]) return true;  // already loaded

    const char *idx_path = stardict_dicts[dict_index].idx_path.c_str();
    Serial.printf("[Dict] Loading index: %s\n", idx_path);

    File idx_file = SD.open(idx_path, FILE_READ);
    if (!idx_file) {
        Serial.println("[Dict] Failed to open idx file");
        return false;
    }

    size_t idx_size = idx_file.size();
    if (idx_size == 0 || idx_size > MAX_IDX_FILE_SIZE) {
        Serial.printf("[Dict] Index file too large or empty: %u bytes\n", (unsigned)idx_size);
        idx_file.close();
        return false;
    }

    // Allocate PSRAM for raw index data (+1 for safety null terminator)
    uint8_t *raw = (uint8_t *)ps_malloc(idx_size + 1);
    if (!raw) {
        Serial.println("[Dict] ps_malloc failed for index data");
        idx_file.close();
        return false;
    }

    // Read in 4KB blocks (much faster than byte-by-byte SPI)
    size_t total_read = 0;
    while (total_read < idx_size) {
        size_t chunk = idx_size - total_read;
        if (chunk > 4096) chunk = 4096;
        size_t n = idx_file.read(raw + total_read, chunk);
        if (n == 0) break;
        total_read += n;
    }
    idx_file.close();
    raw[total_read] = '\0';

    if (total_read != idx_size) {
        Serial.printf("[Dict] Partial read: %u / %u bytes\n", (unsigned)total_read, (unsigned)idx_size);
        free(raw);
        return false;
    }

    // First pass: count entries
    uint32_t count = 0;
    size_t pos = 0;
    while (pos < idx_size) {
        // Skip word string until null terminator
        while (pos < idx_size && raw[pos] != '\0') pos++;
        pos++;  // skip null terminator
        if (pos + 8 > idx_size) break;  // need 8 bytes for offset+size
        pos += 8;
        count++;
    }

    if (count == 0) {
        Serial.println("[Dict] No entries found in index");
        free(raw);
        return false;
    }

    // Allocate entry array in PSRAM
    idx_entry_t *entries = (idx_entry_t *)ps_malloc(count * sizeof(idx_entry_t));
    if (!entries) {
        Serial.println("[Dict] ps_malloc failed for entry array");
        free(raw);
        return false;
    }

    // Second pass: populate entries
    pos = 0;
    uint32_t idx = 0;
    while (pos < idx_size && idx < count) {
        const char *word_ptr = (const char *)(raw + pos);
        // Skip word string until null terminator
        while (pos < idx_size && raw[pos] != '\0') pos++;
        pos++;  // skip null terminator
        if (pos + 8 > idx_size) break;

        uint32_t data_offset = ((uint32_t)raw[pos] << 24) | ((uint32_t)raw[pos + 1] << 16) |
                               ((uint32_t)raw[pos + 2] << 8) | raw[pos + 3];
        uint32_t data_size = ((uint32_t)raw[pos + 4] << 24) | ((uint32_t)raw[pos + 5] << 16) |
                             ((uint32_t)raw[pos + 6] << 8) | raw[pos + 7];
        pos += 8;

        entries[idx].word = word_ptr;
        entries[idx].offset = data_offset;
        entries[idx].size = data_size;
        idx++;
    }

    // Sort by word (case-insensitive) for binary search
    qsort(entries, idx, sizeof(idx_entry_t), idx_entry_cmp);

    psram_idx_data[dict_index] = raw;
    psram_idx_entries[dict_index] = entries;
    psram_idx_count[dict_index] = idx;

    Serial.printf("[Dict] Index loaded: %u entries (%.1f KB data + %.1f KB entries)\n",
                  (unsigned)idx, idx_size / 1024.0, (idx * sizeof(idx_entry_t)) / 1024.0);
    return true;
}

// Binary search lookup using PSRAM-cached index.
// SPI lock must be held; reads definition from .dict file on SD.
static bool stardict_lookup_psram(int dict_index, const char *word, dict_result_t &result)
{
    result.found = false;
    if (!psram_idx_entries[dict_index] || psram_idx_count[dict_index] == 0)
        return false;

    idx_entry_t key;
    key.word = word;
    idx_entry_t *found = (idx_entry_t *)bsearch(&key, psram_idx_entries[dict_index],
                                                  psram_idx_count[dict_index],
                                                  sizeof(idx_entry_t), idx_entry_cmp);
    if (!found) return false;

    // Read definition from .dict file
    const char *dict_path = stardict_dicts[dict_index].dict_path.c_str();
    File dict_file = SD.open(dict_path, FILE_READ);
    if (!dict_file) return false;

    uint32_t data_size = found->size;
    if (data_size > 4096) data_size = 4096;

    dict_file.seek(found->offset);
    char *def_buf = (char *)malloc(data_size + 1);
    if (!def_buf) { dict_file.close(); return false; }

    dict_file.read((uint8_t *)def_buf, data_size);
    def_buf[data_size] = '\0';
    dict_file.close();

    if (stardict_dicts[dict_index].sametypesequence == 'h') {
        html_to_text(def_buf);
    }

    result.word = found->word;
    result.definition = def_buf;
    result.found = true;
    free(def_buf);
    return true;
}

// Look up in a single StarDict dictionary by its paths
static bool stardict_lookup_in(const String &idx_path, const String &dict_path,
                               const char *word, dict_result_t &result,
                               char sametypesequence = 'm')
{
    result.found = false;

    File idx_file = SD.open(idx_path.c_str(), FILE_READ);
    if (!idx_file) return false;

    // Linear scan through the .idx file
    // Each entry: word\0 + 4-byte offset (big-endian) + 4-byte size (big-endian)
    char entry_word[256];
    size_t file_size = idx_file.size();

    while (idx_file.position() < file_size) {
        // Read word (null-terminated string)
        int i = 0;
        while (i < 255) {
            int c = idx_file.read();
            if (c < 0) break;
            if (c == '\0') break;
            entry_word[i++] = (char)c;
        }
        entry_word[i] = '\0';

        if (idx_file.position() >= file_size) break;

        // Read 4-byte offset and 4-byte size (big-endian)
        uint8_t buf[8];
        if (idx_file.read(buf, 8) != 8) break;

        uint32_t data_offset = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                               ((uint32_t)buf[2] << 8) | buf[3];
        uint32_t data_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                             ((uint32_t)buf[6] << 8) | buf[7];

        if (strcasecmp(word, entry_word) == 0) {
            idx_file.close();

            // Read definition from .dict file
            File dict_file = SD.open(dict_path.c_str(), FILE_READ);
            if (!dict_file) return false;

            // Limit read size to prevent OOM
            if (data_size > 4096) data_size = 4096;

            dict_file.seek(data_offset);
            char *def_buf = (char *)malloc(data_size + 1);
            if (!def_buf) { dict_file.close(); return false; }

            dict_file.read((uint8_t *)def_buf, data_size);
            def_buf[data_size] = '\0';
            dict_file.close();

            if (sametypesequence == 'h') {
                html_to_text(def_buf);
            }

            result.word = entry_word;
            result.definition = def_buf;
            result.found = true;
            free(def_buf);
            return true;
        }
    }

    idx_file.close();
    return false;
}

bool dict_lookup_stardict(const char *word, dict_result_t &result)
{
    return dict_lookup_stardict_all(word, result);
}

bool dict_lookup_stardict_single(int dict_index, const char *word, dict_result_t &result)
{
    result.found = false;
    dict_scan_stardict();
    if (dict_index < 0 || dict_index >= stardict_count) return false;

    shared_spi_lock();
    sd_care_init();

    // Lazy-load PSRAM index on first lookup
    if (!psram_idx_entries[dict_index]) {
        load_stardict_index(dict_index);
    }

    bool found;
    dict_info_t &d = stardict_dicts[dict_index];
    if (psram_idx_entries[dict_index]) {
        // Fast path: binary search in PSRAM
        found = stardict_lookup_psram(dict_index, word, result);
    } else {
        // Fallback: original linear scan from SD
        found = stardict_lookup_in(d.idx_path, d.dict_path, word, result, d.sametypesequence);
    }
    shared_spi_unlock();

    if (found) {
        // Prepend dictionary name to definition
        string prefix = "[" + string(d.name.c_str()) + "]\n";
        result.definition = prefix + result.definition;
    }
    return found;
}

bool dict_lookup_stardict_all(const char *word, dict_result_t &result)
{
    result.found = false;
    dict_scan_stardict();

    if (stardict_count == 0) return false;

    shared_spi_lock();
    sd_care_init();
    for (int i = 0; i < stardict_count; i++) {
        // Lazy-load PSRAM index on first lookup
        if (!psram_idx_entries[i]) {
            load_stardict_index(i);
        }

        bool found;
        dict_info_t &d = stardict_dicts[i];
        if (psram_idx_entries[i]) {
            // Fast path: binary search in PSRAM
            found = stardict_lookup_psram(i, word, result);
        } else {
            // Fallback: original linear scan from SD
            found = stardict_lookup_in(d.idx_path, d.dict_path, word, result, d.sametypesequence);
        }

        if (found) {
            shared_spi_unlock();
            // Prepend dictionary name to definition
            string prefix = "[" + string(d.name.c_str()) + "]\n";
            result.definition = prefix + result.definition;
            return true;
        }
    }
    shared_spi_unlock();
    return false;
}

// ---- Prefix search (uses PSRAM index) ----

// Search a single dictionary's PSRAM index for words starting with prefix.
// Returns number of matches written to results (up to max_results).
static int prefix_search_in_index(int dict_index, const char *prefix,
                                  const char **results, int max_results)
{
    if (!psram_idx_entries[dict_index] || psram_idx_count[dict_index] == 0)
        return 0;

    int prefix_len = strlen(prefix);
    if (prefix_len == 0) return 0;

    idx_entry_t *entries = psram_idx_entries[dict_index];
    uint32_t count = psram_idx_count[dict_index];

    // Binary search for the first entry >= prefix (lower bound)
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (strncasecmp(entries[mid].word, prefix, prefix_len) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    // Iterate forward collecting matches
    int found = 0;
    for (uint32_t i = lo; i < count && found < max_results; i++) {
        if (strncasecmp(entries[i].word, prefix, prefix_len) != 0)
            break;
        results[found++] = entries[i].word;
    }
    return found;
}

int dict_prefix_search(const char *prefix, const char **results, int max_results, int dict_index)
{
    if (!prefix || prefix[0] == '\0' || max_results <= 0) return 0;

    dict_scan_stardict();

    // Ensure indices are loaded (may need SPI for lazy-load)
    bool need_spi = false;
    if (dict_index >= 0) {
        if (dict_index >= stardict_count) return 0;
        if (!psram_idx_entries[dict_index]) need_spi = true;
    } else {
        for (int i = 0; i < stardict_count; i++) {
            if (!psram_idx_entries[i]) { need_spi = true; break; }
        }
    }

    if (need_spi) {
        shared_spi_lock();
        sd_care_init();
        if (dict_index >= 0) {
            load_stardict_index(dict_index);
        } else {
            for (int i = 0; i < stardict_count; i++) {
                if (!psram_idx_entries[i]) load_stardict_index(i);
            }
        }
        shared_spi_unlock();
    }

    // Perform prefix search (pure PSRAM reads, no SPI needed)
    int total = 0;
    if (dict_index >= 0) {
        total = prefix_search_in_index(dict_index, prefix, results, max_results);
    } else {
        for (int i = 0; i < stardict_count && total < max_results; i++) {
            total += prefix_search_in_index(i, prefix,
                                            results + total, max_results - total);
        }
    }
    return total;
}

// ---- Online lookup ----

bool dict_lookup_online(const char *word, dict_result_t &result)
{
    result.found = false;
    if (WiFi.status() != WL_CONNECTED) return false;

    char url[256];
    snprintf(url, sizeof(url), "https://api.dictionaryapi.dev/api/v2/entries/en/%s", word);

    http_response_t resp = http_get(url, 8000);
    if (!resp.success) return false;

    cJSON *root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;

    // Response is an array
    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *entry = cJSON_GetArrayItem(root, 0);

    cJSON *word_j = cJSON_GetObjectItem(entry, "word");
    if (word_j && word_j->valuestring) result.word = word_j->valuestring;

    // Phonetic
    cJSON *phonetic = cJSON_GetObjectItem(entry, "phonetic");
    if (phonetic && phonetic->valuestring) result.phonetic = phonetic->valuestring;

    // Get first meaning
    cJSON *meanings = cJSON_GetObjectItem(entry, "meanings");
    if (meanings && cJSON_GetArraySize(meanings) > 0) {
        cJSON *meaning0 = cJSON_GetArrayItem(meanings, 0);

        cJSON *pos = cJSON_GetObjectItem(meaning0, "partOfSpeech");
        if (pos && pos->valuestring) result.part_of_speech = pos->valuestring;

        cJSON *defs = cJSON_GetObjectItem(meaning0, "definitions");
        if (defs && cJSON_GetArraySize(defs) > 0) {
            string all_defs;
            int n = cJSON_GetArraySize(defs);
            if (n > 3) n = 3;
            for (int i = 0; i < n; i++) {
                cJSON *def_item = cJSON_GetArrayItem(defs, i);
                cJSON *def_text = cJSON_GetObjectItem(def_item, "definition");
                if (def_text && def_text->valuestring) {
                    char num[8];
                    snprintf(num, sizeof(num), "%d. ", i + 1);
                    all_defs += num;
                    all_defs += def_text->valuestring;
                    all_defs += "\n";
                }
            }
            result.definition = all_defs;
        }
    }

    result.found = true;
    cJSON_Delete(root);
    return true;
}

// ---- Custom binary format ----

bool dict_offline_en_available()
{
    shared_spi_lock();
    bool sd_ok = sd_care_init();
    Serial.printf("[Dictionary] offline_en_available: installSD()=%d\n", sd_ok);
    if (!sd_ok) {
        shared_spi_unlock();
        return false;
    }
    bool exists = SD.exists("/dict_en.idx") && SD.exists("/dict_en.dat");
    Serial.printf("[Dictionary] dict_en files exist: %d\n", exists);
    shared_spi_unlock();
    return exists;
}

bool dict_lookup_offline_en(const char *word, dict_result_t &result)
{
    result.found = false;

    shared_spi_lock();
    sd_care_init();

    if (!SD.exists("/dict_en.idx") || !SD.exists("/dict_en.dat")) {
        shared_spi_unlock();
        return false;
    }

    File idx_file = SD.open("/dict_en.idx", FILE_READ);
    if (!idx_file) {
        shared_spi_unlock();
        return false;
    }

    // Binary search in index file
    // Index format: word\0 + 4-byte offset (big-endian) + 2-byte length (big-endian)
    size_t file_size = idx_file.size();
    size_t low = 0, high = file_size;
    char buf[256];
    bool found = false;
    uint32_t data_offset = 0;
    uint16_t data_len = 0;

    while (low < high) {
        size_t mid = (low + high) / 2;

        // Seek backwards to find start of entry (after a \0)
        idx_file.seek(mid);
        while (mid > low) {
            mid--;
            idx_file.seek(mid);
            char c = idx_file.read();
            if (c == '\0') {
                mid++;
                break;
            }
        }
        if (mid == low && low > 0) {
            idx_file.seek(mid);
        }

        // Read the word at this position
        idx_file.seek(mid);
        int i = 0;
        while (i < 255) {
            int c = idx_file.read();
            if (c < 0 || c == '\0') break;
            buf[i++] = (char)c;
        }
        buf[i] = '\0';

        int cmp = strcasecmp(word, buf);
        if (cmp == 0) {
            // Read offset and length
            uint8_t offset_bytes[4];
            uint8_t len_bytes[2];
            idx_file.read(offset_bytes, 4);
            idx_file.read(len_bytes, 2);
            data_offset = ((uint32_t)offset_bytes[0] << 24) | ((uint32_t)offset_bytes[1] << 16) |
                          ((uint32_t)offset_bytes[2] << 8) | offset_bytes[3];
            data_len = ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
            found = true;
            break;
        } else if (cmp < 0) {
            high = mid;
        } else {
            // Skip past this entry's offset+length to advance
            idx_file.seek(idx_file.position() + 6);
            low = idx_file.position();
        }
    }

    idx_file.close();

    if (!found) {
        shared_spi_unlock();
        return false;
    }

    // Read definition from data file
    File dat_file = SD.open("/dict_en.dat", FILE_READ);
    if (!dat_file) {
        shared_spi_unlock();
        return false;
    }

    dat_file.seek(data_offset);
    char *def_buf = (char *)malloc(data_len + 1);
    if (!def_buf) {
        dat_file.close();
        shared_spi_unlock();
        return false;
    }

    dat_file.read((uint8_t *)def_buf, data_len);
    def_buf[data_len] = '\0';
    dat_file.close();

    shared_spi_unlock();

    result.word = word;
    result.definition = def_buf;
    result.found = true;
    free(def_buf);
    return true;
}



/* ---- query history ------------------------------------------------------- */

void dict_history_log(const char *word)
{
    if (!word || word[0] == '\0') return;

    /* An unset clock would stamp everything 1970; leave the field empty. */
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    bool clock_ok = (tm.tm_year + 1900) >= 2024;

    char row[160];
    dict_history_format_row(row, sizeof(row), word, clock_ok ? &tm : NULL);

    shared_spi_lock();
    sd_care_init();
    bool fresh = !SD.exists(DICT_HISTORY_PATH);
    File f = SD.open(DICT_HISTORY_PATH, FILE_APPEND);
    if (f) {
        if (fresh) f.print("word,date\n");
        f.print(row);
        f.print("\n");
        f.close();
    } else {
        Serial.println("[Dict] history: cannot open " DICT_HISTORY_PATH);
    }
    shared_spi_unlock();
}

/* ---- dictionary selection ------------------------------------------------ */

void dict_unload_index(int index)
{
    if (index < 0 || index >= MAX_STARDICT_DICTS) return;
    if (!psram_idx_data[index] && !psram_idx_entries[index]) return;

    /* Both allocations belong to this dictionary; nothing else points at the
     * index once the entry table is gone. */
    if (psram_idx_entries[index]) { free(psram_idx_entries[index]); psram_idx_entries[index] = NULL; }
    if (psram_idx_data[index])    { free(psram_idx_data[index]);    psram_idx_data[index] = NULL; }
    psram_idx_count[index] = 0;
    Serial.printf("[Dict] Unloaded index %d (freed PSRAM)\n", index);
}

void dict_set_enabled(int index, bool enabled)
{
    if (index < 0 || index >= MAX_STARDICT_DICTS) return;
    if (stardict_enabled[index] == enabled) return;
    stardict_enabled[index] = enabled;
    if (!enabled) dict_unload_index(index);   /* reclaim the PSRAM immediately */
}

bool dict_is_enabled(int index)
{
    if (index < 0 || index >= MAX_STARDICT_DICTS) return false;
    return stardict_enabled[index];
}

int dict_enabled_count(void)
{
    int n = 0;
    for (int i = 0; i < stardict_count; i++) if (stardict_enabled[i]) n++;
    return n;
}

/* Selection is keyed by dictionary name, not index: adding or removing files
 * on the card reorders the scan, and an index-based key would silently enable
 * the wrong dictionary.
 *
 * The name is hashed rather than used directly because NVS keys are limited to
 * 15 characters (NVS_KEY_NAME_MAX_SIZE) — a truncated bookname would both
 * overflow that and collide between dictionaries sharing a prefix. */
static void dict_pref_key(int i, char *out, size_t cap)
{
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (const char *p = stardict_dicts[i].name.c_str(); *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    snprintf(out, cap, "d%08x", (unsigned)h);       /* 9 chars, fits NVS */
}

void dict_load_selection(void)
{
    Preferences p;
    p.begin("dict", true);
    for (int i = 0; i < stardict_count; i++) {
        char key[16];
        dict_pref_key(i, key, sizeof(key));
        stardict_enabled[i] = p.getBool(key, true);   /* default: enabled */
    }
    p.end();
}

void dict_save_selection(void)
{
    Preferences p;
    p.begin("dict", false);
    for (int i = 0; i < stardict_count; i++) {
        char key[16];
        dict_pref_key(i, key, sizeof(key));
        p.putBool(key, stardict_enabled[i]);
    }
    p.end();
}

int dict_preload_enabled(void (*progress)(const char *name, int n, int total))
{
    dict_scan_stardict();

    int total = dict_enabled_count();
    int done = 0, resident = 0;

    shared_spi_lock();
    sd_care_init();
    for (int i = 0; i < stardict_count; i++) {
        if (!stardict_enabled[i]) {
            dict_unload_index(i);            /* deselected: give the PSRAM back */
            continue;
        }
        if (!psram_idx_entries[i]) {
            done++;
            if (progress) progress(stardict_dicts[i].name.c_str(), done, total);
            load_stardict_index(i);
        }
        if (psram_idx_entries[i]) resident++;
    }
    shared_spi_unlock();

    Serial.printf("[Dict] %d/%d enabled dictionaries resident\n", resident, total);
    return resident;
}

bool dict_lookup_enabled(const char *word, dict_result_t &result)
{
    result.found = false;
    dict_scan_stardict();
    if (stardict_count == 0) return false;

    string combined;
    bool any = false;
    Serial.printf("[Dict] querying %d of %d dictionaries for \"%s\"\n",
                  dict_enabled_count(), stardict_count, word ? word : "");

    shared_spi_lock();
    sd_care_init();
    for (int i = 0; i < stardict_count; i++) {
        if (!stardict_enabled[i]) continue;

        if (!psram_idx_entries[i]) load_stardict_index(i);

        dict_result_t one;
        one.found = false;
        dict_info_t &d = stardict_dicts[i];
        bool found = psram_idx_entries[i]
                   ? stardict_lookup_psram(i, word, one)
                   : stardict_lookup_in(d.idx_path, d.dict_path, word, one, d.sametypesequence);
        Serial.printf("[Dict]   %s: %s\n", d.name.c_str(), found ? "hit" : "miss");
        if (!found) continue;

        if (any) combined += "\n\n";
        combined += "[" + string(d.name.c_str()) + "]\n";
        combined += one.definition;

        if (!any) {
            /* Keep the headword and phonetics from the first dictionary that
             * had them; later entries only contribute their definition. */
            result.word = one.word;
            result.phonetic = one.phonetic;
            result.part_of_speech = one.part_of_speech;
        }
        any = true;
    }
    shared_spi_unlock();

    if (any) {
        result.definition = combined;
        result.found = true;
    }
    return any;
}


#else
// Desktop stubs
bool dict_lookup_online(const char *word, dict_result_t &result) { result.found = false; return false; }
bool dict_lookup_stardict(const char *word, dict_result_t &result) { result.found = false; return false; }
bool dict_lookup_offline_en(const char *word, dict_result_t &result) { result.found = false; return false; }
bool dict_stardict_available() { return false; }
bool dict_offline_en_available() { return false; }
int dict_scan_stardict() { return 0; }
bool dict_lookup_stardict_single(int dict_index, const char *word, dict_result_t &result) { result.found = false; return false; }
bool dict_lookup_stardict_all(const char *word, dict_result_t &result) { result.found = false; return false; }
int dict_get_stardict_count() { return 0; }
const char *dict_get_stardict_name(int index) { return NULL; }
int dict_prefix_search(const char *prefix, const char **results, int max_results, int dict_index) { return 0; }
void dict_set_enabled(int index, bool enabled) { (void)index; (void)enabled; }
bool dict_is_enabled(int index) { (void)index; return false; }
int dict_enabled_count(void) { return 0; }
void dict_load_selection(void) {}
void dict_save_selection(void) {}
void dict_unload_index(int index) { (void)index; }
int dict_preload_enabled(void (*progress)(const char *, int, int)) { (void)progress; return 0; }
bool dict_lookup_enabled(const char *word, dict_result_t &result) { (void)word; result.found = false; return false; }
void dict_history_log(const char *word) { (void)word; }
#endif
