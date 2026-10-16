/*
 * input_keyboard_map test: see CMakeLists.txt.
 *
 * keyboard.c resolves its key map through input_map.c. This checks that the
 * pad it makes is byte for byte the pad v0.14.0 made, against a frozen copy
 * of v0.14.0's map and xbox_InputKeyboardState (tag v0.14.0, eb0aa1d5,
 * src/input/keyboard.c, copied below unchanged apart from the names):
 * every subset of the map's keys up to four, all of them at once, and a
 * fixed pseudo-random sweep over all 256 codes. Only the packet number may
 * differ, and it has to keep moving.
 *
 * Also: the default map as binding text, which is what a port that lets the
 * player rebind starts from.
 */
#include "keyboard.h"
#include "input_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── v0.14.0, frozen ───────────────────────────────────────────────────── */

static const xbox_key_row ref_map[] = {
    { "Up arrow",        { XBOX_VK_UP },     XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_UP,     0, "D-pad up" },
    { "Down arrow",      { XBOX_VK_DOWN },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_DOWN,   0, "D-pad down" },
    { "Left arrow",      { XBOX_VK_LEFT },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_LEFT,   0, "D-pad left" },
    { "Right arrow",     { XBOX_VK_RIGHT },  XBOX_KEY_BUTTON, XBOX_GAMEPAD_DPAD_RIGHT,  0, "D-pad right" },
    { "Enter",           { XBOX_VK_RETURN }, XBOX_KEY_BUTTON, XBOX_GAMEPAD_START,       0, "START" },
    { "Backspace",       { XBOX_VK_BACK },   XBOX_KEY_BUTTON, XBOX_GAMEPAD_BACK,        0, "BACK" },
    { "Shift",           { XBOX_VK_SHIFT },  XBOX_KEY_BUTTON, XBOX_GAMEPAD_LEFT_THUMB,  0, "Left stick click" },
    { "Ctrl",            { XBOX_VK_CONTROL },XBOX_KEY_BUTTON, XBOX_GAMEPAD_RIGHT_THUMB, 0, "Right stick click" },
    { "Z",               { 'Z' },            XBOX_KEY_ANALOG, XBOX_BUTTON_A,            0, "A" },
    { "X",               { 'X' },            XBOX_KEY_ANALOG, XBOX_BUTTON_B,            0, "B" },
    { "C",               { 'C' },            XBOX_KEY_ANALOG, XBOX_BUTTON_X,            0, "X" },
    { "V",               { 'V' },            XBOX_KEY_ANALOG, XBOX_BUTTON_Y,            0, "Y" },
    { "Q",               { 'Q' },            XBOX_KEY_ANALOG, XBOX_BUTTON_WHITE,        0, "White" },
    { "E",               { 'E' },            XBOX_KEY_ANALOG, XBOX_BUTTON_BLACK,        0, "Black" },
    { "1",               { '1' },            XBOX_KEY_ANALOG, XBOX_BUTTON_LTRIGGER,     0, "Left trigger" },
    { "3",               { '3' },            XBOX_KEY_ANALOG, XBOX_BUTTON_RTRIGGER,     0, "Right trigger" },
    { "W or keypad 8",   { 'W', XBOX_VK_NUMPAD8 }, XBOX_KEY_AXIS, XBOX_KEY_LY, +1, "Left stick up" },
    { "S or keypad 2",   { 'S', XBOX_VK_NUMPAD2 }, XBOX_KEY_AXIS, XBOX_KEY_LY, -1, "Left stick down" },
    { "A or keypad 4",   { 'A', XBOX_VK_NUMPAD4 }, XBOX_KEY_AXIS, XBOX_KEY_LX, -1, "Left stick left" },
    { "D or keypad 6",   { 'D', XBOX_VK_NUMPAD6 }, XBOX_KEY_AXIS, XBOX_KEY_LX, +1, "Left stick right" },
    { "I",               { 'I' },            XBOX_KEY_AXIS,   XBOX_KEY_RY,             +1, "Right stick up" },
    { "K",               { 'K' },            XBOX_KEY_AXIS,   XBOX_KEY_RY,             -1, "Right stick down" },
    { "J",               { 'J' },            XBOX_KEY_AXIS,   XBOX_KEY_RX,             -1, "Right stick left" },
    { "L",               { 'L' },            XBOX_KEY_AXIS,   XBOX_KEY_RX,             +1, "Right stick right" },
};
#define REF_MAP_COUNT (sizeof ref_map / sizeof ref_map[0])

static int row_held(const xbox_key_row *r)
{
    return xbox_FramebufferKeyDown(r->vk[0])
        || (r->vk[1] && xbox_FramebufferKeyDown(r->vk[1]));
}

static void ref_KeyboardState(XBOX_INPUT_STATE *out)
{
    static DWORD packet;
    /* Per thumb axis, whether a key holds it each way: a digital key means
     * full deflection, and opposite keys together cancel. */
    int pos[4] = { 0, 0, 0, 0 }, neg[4] = { 0, 0, 0, 0 }, axis[4];

    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < REF_MAP_COUNT; i++) {
        const xbox_key_row *r = &ref_map[i];
        if (!row_held(r))
            continue;
        switch (r->kind) {
        case XBOX_KEY_BUTTON:
            out->Gamepad.wButtons |= r->target;
            break;
        case XBOX_KEY_ANALOG:
            /* Analog on the console, so a key is 255 rather than a flag --
             * a title that reads these as a pressure never sees a press if
             * they are 1. */
            out->Gamepad.bAnalogButtons[r->target] = 255;
            break;
        case XBOX_KEY_AXIS:
            if (r->sign > 0) pos[r->target] = 1; else neg[r->target] = 1;
            break;
        }
    }
    for (int a = 0; a < 4; a++)
        axis[a] = (pos[a] - neg[a]) * 32767;
    out->Gamepad.sThumbLX = (SHORT)axis[XBOX_KEY_LX];
    out->Gamepad.sThumbLY = (SHORT)axis[XBOX_KEY_LY];
    out->Gamepad.sThumbRX = (SHORT)axis[XBOX_KEY_RX];
    out->Gamepad.sThumbRY = (SHORT)axis[XBOX_KEY_RY];

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    out->dwPacketNumber = ++packet;
}

/* ── comparison ────────────────────────────────────────────────────────── */

static int g_keys[64], g_nkeys;
static DWORD g_last_packet;
static unsigned long g_compared;

static void compare_now(const char *what)
{
    XBOX_INPUT_STATE got, want;
    memset(&want, 0, sizeof want);
    ref_KeyboardState(&want);
    memset(&got, 0xA5, sizeof got);       /* the adopter must clear it */
    xbox_InputKeyboardState(&got);
    CHECK(got.dwPacketNumber != g_last_packet, "%s: packet number did not move", what);
    g_last_packet = got.dwPacketNumber;
    got.dwPacketNumber = want.dwPacketNumber = 0;
    g_compared++;
    if (memcmp(&got, &want, sizeof got)) {
        CHECK(0, "%s: buttons 0x%04X/0x%04X analog %d,%d,%d,%d,%d,%d,%d,%d / %d,%d,%d,%d,%d,%d,%d,%d "
              "sticks %d,%d,%d,%d / %d,%d,%d,%d", what,
              got.Gamepad.wButtons, want.Gamepad.wButtons,
              got.Gamepad.bAnalogButtons[0], got.Gamepad.bAnalogButtons[1], got.Gamepad.bAnalogButtons[2],
              got.Gamepad.bAnalogButtons[3], got.Gamepad.bAnalogButtons[4], got.Gamepad.bAnalogButtons[5],
              got.Gamepad.bAnalogButtons[6], got.Gamepad.bAnalogButtons[7],
              want.Gamepad.bAnalogButtons[0], want.Gamepad.bAnalogButtons[1], want.Gamepad.bAnalogButtons[2],
              want.Gamepad.bAnalogButtons[3], want.Gamepad.bAnalogButtons[4], want.Gamepad.bAnalogButtons[5],
              want.Gamepad.bAnalogButtons[6], want.Gamepad.bAnalogButtons[7],
              got.Gamepad.sThumbLX, got.Gamepad.sThumbLY, got.Gamepad.sThumbRX, got.Gamepad.sThumbRY,
              want.Gamepad.sThumbLX, want.Gamepad.sThumbLY, want.Gamepad.sThumbRX, want.Gamepad.sThumbRY);
    } else {
        g_checks++;
    }
}

/* Every subset of g_keys of size `left` more, from index `from` on. */
static void subsets(int from, int left)
{
    compare_now("subset");
    if (!left)
        return;
    for (int i = from; i < g_nkeys; i++) {
        xbox_InputKeySet(g_keys[i], 1);
        subsets(i + 1, left - 1);
        xbox_InputKeySet(g_keys[i], 0);
    }
}

static void test_equivalence(void)
{
    /* The keys of the frozen map, and the live map has the same ones. */
    g_nkeys = 0;
    for (size_t r = 0; r < REF_MAP_COUNT; r++)
        for (int k = 0; k < 2; k++)
            if (ref_map[r].vk[k])
                g_keys[g_nkeys++] = ref_map[r].vk[k];
    CHECK(g_nkeys == 28, "frozen map has %d keys", g_nkeys);
    CHECK(xbox_key_map_count == REF_MAP_COUNT, "map has %u rows, v0.14.0 had %u",
          (unsigned)xbox_key_map_count, (unsigned)REF_MAP_COUNT);
    for (size_t r = 0; r < xbox_key_map_count && r < REF_MAP_COUNT; r++)
        CHECK(!memcmp(xbox_key_map[r].vk, ref_map[r].vk, 2) && xbox_key_map[r].kind == ref_map[r].kind
              && xbox_key_map[r].target == ref_map[r].target && xbox_key_map[r].sign == ref_map[r].sign
              && !strcmp(xbox_key_map[r].keys, ref_map[r].keys)
              && !strcmp(xbox_key_map[r].guest, ref_map[r].guest),
              "row %u (%s) differs from v0.14.0", (unsigned)r, ref_map[r].keys);

    xbox_InputKeysClear();
    subsets(0, 4);

    /* Every key of the map at once. */
    for (int i = 0; i < g_nkeys; i++)
        xbox_InputKeySet(g_keys[i], 1);
    compare_now("all held");
    /* And every code there is. */
    for (int vk = 0; vk < 256; vk++)
        xbox_InputKeySet(vk, 1);
    compare_now("all 256 held");
    xbox_InputKeysClear();

    /* Each code alone, mapped or not (side-specific modifiers, Space, the
     * mouse buttons: none of them is in the map). */
    for (int vk = 0; vk < 256; vk++) {
        xbox_InputKeysClear();
        xbox_InputKeySet(vk, 1);
        compare_now("one code");
    }

    /* A fixed pseudo-random sweep: map keys and other codes together. */
    {
        unsigned s = 0x2031u;
        for (int n = 0; n < 200000; n++) {
            s = s * 1103515245u + 12345u;
            xbox_InputKeySet((s >> 9) & 1 ? g_keys[(s >> 11) % (unsigned)g_nkeys] : (int)((s >> 11) & 255),
                             (s >> 20) & 3 ? 1 : 0);
            compare_now("sweep");
        }
    }
    xbox_InputKeysClear();
}

/* ── the default map as bindings ───────────────────────────────────────── */

static void test_bindings_text(void)
{
    static const char *const want[INPUT_CONTROL_COUNT] = {
        "up", "down", "left", "right", "enter", "backspace", "shift", "ctrl",
        "Z", "X", "C", "V", "E", "Q", "1", "3",
        "W, Numpad8", "S, Numpad2", "A, Numpad4", "D, Numpad6",
        "I", "K", "J", "L",
    };
    InputBindings b, back;
    char text[64];

    memset(&b, 0xA5, sizeof b);
    xbox_InputKeyboardBindings(&b);
    CHECK(b.diagonal == INPUT_DIAGONAL_FULL, "diagonal %d", b.diagonal);
    memset(&back, 0, sizeof back);
    for (int c = 0; c < INPUT_CONTROL_COUNT; c++) {
        xbox_InputMap_BindingsFormat(&b, c, text, sizeof text);
        CHECK(!strcmp(text, want[c]), "%s = \"%s\", want \"%s\"",
              xbox_InputMap_ControlName(c), text, want[c]);
        /* Read back, the text is the same bindings. */
        CHECK(xbox_InputMap_BindingsParse(&back, c, text) == 0, "%s did not parse", text);
    }
    CHECK(!memcmp(back.key, b.key, sizeof b.key) && !memcmp(back.count, b.count, sizeof b.count),
          "bindings read back differ");

    /* The same text through KeysResolve is the keyboard's pad. */
    {
        uint8_t down[INPUT_KEY_COUNT] = { 0 };
        XBOX_GAMEPAD p;
        XBOX_INPUT_STATE st;
        down['W'] = down['D'] = down['Z'] = down[XBOX_VK_RETURN] = 1;
        xbox_InputMap_KeysResolve(&back, down, &p);
        xbox_InputKeysClear();
        xbox_InputKeySet('W', 1); xbox_InputKeySet('D', 1);
        xbox_InputKeySet('Z', 1); xbox_InputKeySet(XBOX_VK_RETURN, 1);
        xbox_InputKeyboardState(&st);
        CHECK(!memcmp(&p, &st.Gamepad, sizeof p), "parsed defaults resolve differently");
        CHECK(p.sThumbLX == 32767 && p.sThumbLY == 32767, "diagonal %d,%d", p.sThumbLX, p.sThumbLY);
        xbox_InputKeysClear();
    }
}

int main(void)
{
    test_equivalence();
    test_bindings_text();
    printf("input_keyboard_map: %lu states compared, %d checks, %d failed\n",
           g_compared, g_checks, g_fail);
    return g_fail ? 1 : 0;
}
