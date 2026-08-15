
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

// Modifier positions
#define KEY_ALT_ROW   2
#define KEY_ALT_COL   0
#define KEY_SYM_ROW   3
#define KEY_SYM_COL   8

Adafruit_TCA8418 keypad;
keypad_cb keypad_listener = NULL;
char keypad_curr_val = ' ';
int keypad_state = KEYPAD_RELEASE;
bool keypad_update = false;
static bool sym_active = false;
static bool sym_lock = false;
static bool alt_held = false;   /* true only while the ALT key is physically down */

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

int keypad_get_val(char *c)
{
    if(c){
        *c = keypad_curr_val;
    }
    return keypad_update;
} 

void keypad_set_flag(void)
{
    keypad_update = false;
}

void keypad_loop(void)
{
    char c = 0;
    int state = -1;
    int row, col;
    int k = keypad.getEvent();

    if (k == 0) return;

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

    if (state == KEYPAD_PRESS) {
        /* Alt + P = screenshot (physical 'p' is row 0, col 9). Handled before
         * character mapping so it doesn't also emit the sym-layer character. */
        if (alt_held && row == 0 && col == 9) {
            Serial.println("[KBD] screenshot (Alt+P)");
            screenshot_capture();
            return;
        }

        c = (sym_active || sym_lock) ? keymap_sym[row][col] : keymap[row][col];
        if (sym_active && !sym_lock) sym_active = false;

        if (c == 0) return;

        Serial.printf("[KBD] row=%d col=%d char='%c' (0x%02x)\n", row, col, c >= 0x20 ? c : '?', c);

        keypad_curr_val = c;
        keypad_state = state;
        keypad_update = true;
    }
}

void keypad_regetser_cb(keypad_cb cb)
{
    keypad_listener = cb;
}