
#include <Adafruit_TCA8418.h>
#include "utilities.h"
#include "peripheral.h"
#include "screenshot.h"

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 10
#define KEYPAD_PRESS_VAL_MIN   129
#define KEYPAD_PRESS_VAL_MAX   163
#define KEYPAD_RELEASE_VAL_MIN 1
#define KEYPAD_RELEASE_VAL_MAX 35

// Row 3 physical layout (decoded from raw events):
//   col: 0  1  2  3  4  5      6    7      8    9
//        ?  ?  ?  ?  ?  LCtrl  Mic  Space  Sym  RCtrl

// Primary layer (normal)
const char keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\b'},
    {  0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', '\n'},
    {  0,   0,   0,   0,   0,   0,   0, ' ',   0,   0},
};

// Secondary layer (sym / shift)
const char keymap_sym[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'#', '1', '2', '3', '(', ')', '_', '-', '+', '@'},
    {'*', '4', '5', '6', '/', ':', ';', '\'','"', '\b'},
    {  0, '7', '8', '9', '?', '!', ',', '.', '0', '\n'},
    {  0,   0,   0,   0,   0,   0,   0, ' ',   0,   0},
};

// Shift + Sym: sparse overrides for punctuation the two base layers can't reach
// (markdown needs ` > [ ] in particular). A 0 here means "no override", so the
// plain sym character is used. Pairings follow the base key where there is an
// obvious relative: ( ) -> [ ], , . -> < >, / -> backslash.
const char keymap_sym_shift[KEYPAD_ROWS][KEYPAD_COLS] = {
//    #    1    2    3    (    )    _    -    +    @
    {  0,   0,   0,   0, '[', ']',   0, '~', '=', '&'},
//    *    4    5    6    /    :    ;    '    "    BS
    {  0,   0, '%',   0, '\\',  0, '|', '`',   0,   0},
//   ALT   7    8    9    ?    !    ,    .    0    CR
    {  0,   0,   0,   0, '{', '}', '<', '>',   0,   0},
    {  0,   0,   0,   0,   0,   0,   0, ' ',   0,   0},
};

// Modifier positions
#define KEY_ALT_ROW   2
#define KEY_ALT_COL   0
#define KEY_SYM_ROW   3
#define KEY_SYM_COL   8
// The two Ctrl keys were decoded but never mapped (see the row-3 note above),
// so they become the shift pair: left = one-shot shift, right = caps lock.
#define KEY_SHIFT_ROW 3
#define KEY_SHIFT_COL 5
#define KEY_CAPS_ROW  3
#define KEY_CAPS_COL  9

Adafruit_TCA8418 keypad;
keypad_cb keypad_listener = NULL;
char keypad_curr_val = ' ';
int keypad_state = KEYPAD_RELEASE;
bool keypad_update = false;
static bool sym_active = false;
static bool sym_lock = false;
static bool alt_held = false;     /* true only while the ALT key is physically down */
static bool shift_active = false; /* one-shot: consumed by the next character    */
static bool shift_lock = false;   /* caps lock                                    */

bool keypad_init(int address)
{
    if(!i2cIsInit(0)){
        Wire.begin(BOARD_KEYBOARD_SDA, BOARD_KEYBOARD_SCL);
        Wire.beginTransmission(address);
        Wire.endTransmission(true);
    }

    if (!keypad.begin(address, &Wire)) {
        // Serial.println("keypad not found, check wiring & pullups!");
        log_e("keypad not found, check wiring & pullups!");
        return false;
    }

    // configure the size of the keypad matrix.
    // all other pins will be inputs
    keypad.matrix(KEYPAD_ROWS, KEYPAD_COLS);

    // flush the internal buffer
    keypad.flush();

    return true;
}

/* ---- key buffer ----
 * The e-ink flush blocks for a few hundred milliseconds inside
 * lv_task_handler(), so keypad_loop() cannot run at all while the panel
 * updates. The TCA8418's own FIFO only holds ten events (five keystrokes,
 * since press and release both count), so anything typed faster than the
 * refresh rate used to be dropped. Drain the chip into this ring instead and
 * let callers consume at their own pace. */
#define KEY_RING_SIZE 32
static char     key_ring[KEY_RING_SIZE];
static uint8_t  key_head = 0, key_tail = 0;
static uint32_t key_last_ms = 0;

static inline bool key_ring_empty(void) { return key_head == key_tail; }

static void key_ring_push(char c)
{
    uint8_t next = (uint8_t)((key_tail + 1) % KEY_RING_SIZE);
    if (next == key_head) return;          /* full: drop the newest */
    key_ring[key_tail] = c;
    key_tail = next;
}

int keypad_get_val(char *c)
{
    if (key_ring_empty()) return 0;
    if (c) *c = key_ring[key_head];
    return 1;
}

void keypad_set_flag(void)
{
    if (key_ring_empty()) return;
    key_head = (uint8_t)((key_head + 1) % KEY_RING_SIZE);
}

uint32_t keypad_last_activity_ms(void)
{
    return key_last_ms;
}

/* Handle one raw event from the controller. */
static void keypad_handle_event(int k)
{
    char c = 0;
    int state = -1;
    int row, col;

    if (k >= KEYPAD_RELEASE_VAL_MIN && k <= KEYPAD_RELEASE_VAL_MAX) {
        k = k - KEYPAD_RELEASE_VAL_MIN;
        state = KEYPAD_RELEASE;
    }

    if (k >= KEYPAD_PRESS_VAL_MIN && k <= KEYPAD_PRESS_VAL_MAX) {
        k = k - KEYPAD_PRESS_VAL_MIN;
        state = KEYPAD_PRESS;
    }

    if (state < 0) return;

    row = k / KEYPAD_COLS;
    col = (KEYPAD_COLS - 1) - k % KEYPAD_COLS;

    if (row == KEY_SYM_ROW && col == KEY_SYM_COL) {
        if (state == KEYPAD_PRESS) {
            sym_lock = !sym_lock;
            sym_active = sym_lock;
            Serial.printf("[KBD] sym_lock=%d\n", sym_lock);
        }
        return;
    }

    if (row == KEY_ALT_ROW && col == KEY_ALT_COL) {
        alt_held = (state == KEYPAD_PRESS);
        sym_active = (state == KEYPAD_PRESS);
        Serial.printf("[KBD] alt=%d\n", sym_active);
        return;
    }

    if (row == KEY_SHIFT_ROW && col == KEY_SHIFT_COL) {
        if (state == KEYPAD_PRESS) {
            shift_active = true;
            Serial.println("[KBD] shift");
        }
        return;
    }

    if (row == KEY_CAPS_ROW && col == KEY_CAPS_COL) {
        if (state == KEYPAD_PRESS) {
            shift_lock = !shift_lock;
            shift_active = shift_lock;
            Serial.printf("[KBD] caps_lock=%d\n", shift_lock);
        }
        return;
    }

    if (state == KEYPAD_PRESS) {
        /* Alt + P = screenshot (physical 'p' is row 0, col 9). Handled before
         * character mapping so it doesn't also emit the sym-layer character. */
        if (alt_held && row == 0 && col == 9) {
            Serial.println("[KBD] screenshot (Alt+P)");
            screenshot_capture();
            return;
        }

        bool sym   = (sym_active || sym_lock);
        bool shift = (shift_active || shift_lock);

        if (sym) {
            /* Shift only overrides where keymap_sym_shift has an entry. */
            c = shift ? keymap_sym_shift[row][col] : 0;
            if (c == 0) c = keymap_sym[row][col];
        } else {
            c = keymap[row][col];
            if (shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        }

        if (sym_active && !sym_lock)     sym_active = false;
        if (shift_active && !shift_lock) shift_active = false;

        if (c == 0) return;

        Serial.printf("[KBD] row=%d col=%d char='%c' (0x%02x)\n", row, col, c >= 0x20 ? c : '?', c);

        keypad_curr_val = c;
        keypad_state = state;
        keypad_update = true;
        key_ring_push(c);
    }
}

void keypad_loop(void)
{
    /* Drain the controller's FIFO rather than taking one event per call: the
     * panel refresh keeps this function from running for a few hundred ms at a
     * time, so several keystrokes are usually waiting. Bounded so a stuck key
     * can't spin here forever. */
    for (int n = 0; n < 24; n++) {
        int k = keypad.getEvent();
        if (k == 0) break;
        key_last_ms = millis();
        keypad_handle_event(k);
    }
}

void keypad_regetser_cb(keypad_cb cb)
{
    keypad_listener = cb;
}