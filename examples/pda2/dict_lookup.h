/**
 * @file      dict_lookup.h
 * @author    LilyGo
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-04-01
 * @brief     Dictionary lookup backend (offline SD card + online API).
 *
 * Supported offline formats on SD card:
 *   - StarDict: Place .ifo, .idx, and .dict (or .dict.dz) files in /stardict/ on SD.
 *               The .ifo file must contain wordcount and idxfilesize fields.
 *               Compressed .dict.dz is NOT supported (use uncompressed .dict).
 *               Multiple dictionaries are supported - place multiple sets of files.
 *   - Custom binary: /dict_en.idx + /dict_en.dat (binary search format).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <string>

#ifdef ARDUINO
#include <Arduino.h>
#endif

using namespace std;

#define MAX_STARDICT_DICTS  8
#define MAX_SUGGESTIONS     10

typedef struct {
    string word;
    string phonetic;
    string part_of_speech;
    string definition;
    bool found;
} dict_result_t;

typedef struct {
#ifdef ARDUINO
    String name;        // from .ifo "bookname"
    String ifo_path;
    String idx_path;
    String dict_path;
#else
    string name;
    string ifo_path;
    string idx_path;
    string dict_path;
#endif
    char sametypesequence;  // from .ifo, default 'm' (plain text), 'h' = HTML
} dict_info_t;

/**
 * @brief Look up an English word using the online Free Dictionary API.
 * @param word The word to look up.
 * @param result Output result struct.
 * @return true if found.
 */
bool dict_lookup_online(const char *word, dict_result_t &result);

/**
 * @brief Look up a word from StarDict files on SD card (first match across all dicts).
 * @param word The word to look up (case-insensitive).
 * @param result Output result struct.
 * @return true if found.
 */
bool dict_lookup_stardict(const char *word, dict_result_t &result);

/**
 * @brief Look up an English word from the custom binary dictionary on SD card.
 * @param word The word to look up.
 * @param result Output result struct.
 * @return true if found.
 */
bool dict_lookup_offline_en(const char *word, dict_result_t &result);

/**
 * @brief Check if StarDict dictionary files are available on SD card.
 * @return true if at least one .ifo file found in /stardict/.
 */
bool dict_stardict_available();

/**
 * @brief Check if the custom binary offline dictionary is available on SD card.
 * @return true if dictionary files exist.
 */
bool dict_offline_en_available();

/**
 * @brief Scan /stardict/ for all .ifo files and populate the internal dictionary list.
 * @return Number of dictionaries found (0 to MAX_STARDICT_DICTS).
 */
int dict_scan_stardict();

/**
 * @brief Look up a word in a specific StarDict dictionary (by index from scan results).
 * @param dict_index Index of the dictionary (0-based, from scan results).
 * @param word The word to look up (case-insensitive).
 * @param result Output result struct.
 * @return true if found.
 */
bool dict_lookup_stardict_single(int dict_index, const char *word, dict_result_t &result);

/**
 * @brief Look up a word across all scanned StarDict dictionaries (returns first match).
 * @param word The word to look up (case-insensitive).
 * @param result Output result struct.
 * @return true if found.
 */
bool dict_lookup_stardict_all(const char *word, dict_result_t &result);

/* ---- dictionary selection ----
 *
 * Which dictionaries take part in lookups. Selection is persisted, and it also
 * bounds memory: each loaded index sits in PSRAM (up to 4 MB apiece, never
 * released on its own), so disabling a dictionary frees its index. */

/** @brief Enable/disable a dictionary; frees its index when disabled. */
void dict_set_enabled(int index, bool enabled);

/** @brief True if the dictionary takes part in lookups (default: all enabled). */
bool dict_is_enabled(int index);

/** @brief Number of enabled dictionaries. */
int dict_enabled_count(void);

/** @brief Load selection from NVS; call after dict_scan_stardict(). */
void dict_load_selection(void);

/** @brief Persist the current selection to NVS. */
void dict_save_selection(void);

/** @brief Drop a dictionary's PSRAM index (it reloads on next use). */
void dict_unload_index(int index);

/**
 * @brief Load indexes for enabled dictionaries and free the rest.
 *
 * Called when the app opens so the first lookup doesn't stall on a multi-MB
 * read from SD. `progress` (may be NULL) is invoked before each load with the
 * dictionary name and its 1-based position.
 * @return number of indexes resident afterwards.
 */
int dict_preload_enabled(void (*progress)(const char *name, int n, int total));

/* ---- query history ----
 *
 * Every lookup is appended to a two-column CSV on the card so the words you
 * looked up can be reviewed (or imported into a flashcard deck) later:
 *
 *     word,date
 *     "ephemeral",2026-08-17 09:14:03
 *
 * Fields follow RFC 4180: the word is quoted and any embedded quote doubled,
 * so commas and quotation marks in a phrase survive. The date is local time,
 * and is left empty when the clock has not been set this boot rather than
 * recording a 1970 timestamp. */
#define DICT_HISTORY_PATH "/dict_history.csv"

/**
 * @brief Format one history row (without newline) into `out`.
 *        Pure string formatting, separated out so it can be tested off-device.
 * @param tm Local time, or NULL when the clock is not set (date left empty).
 * @return number of characters written (excluding the terminator).
 */
int dict_history_format_row(char *out, size_t cap, const char *word, const struct tm *tm);

/** @brief Append a query to DICT_HISTORY_PATH, creating it with a header. */
void dict_history_log(const char *word);

/**
 * @brief Look up a word in every enabled dictionary and concatenate the hits.
 *
 * Each entry is prefixed with "[bookname]". Definitions are joined into
 * result.definition; result.found is true if any dictionary matched.
 * @return true if at least one dictionary matched.
 */
bool dict_lookup_enabled(const char *word, dict_result_t &result);

/**
 * @brief Get count of available StarDict dictionaries (after scan).
 * @return Number of dictionaries found.
 */
int dict_get_stardict_count();

/**
 * @brief Get the display name of a scanned StarDict dictionary.
 * @param index Index of the dictionary (0-based).
 * @return Name string, or NULL if index out of range.
 */
const char *dict_get_stardict_name(int index);

/**
 * @brief Search for words starting with a prefix (uses PSRAM index).
 * @param prefix       The prefix to search for (case-insensitive).
 * @param results      Output array of matching words (pointers into PSRAM, valid until dict unloaded).
 * @param max_results  Maximum number of results to return.
 * @param dict_index   Dictionary index (-1 for all dictionaries).
 * @return Number of matching words found.
 */
int dict_prefix_search(const char *prefix, const char **results, int max_results, int dict_index);
