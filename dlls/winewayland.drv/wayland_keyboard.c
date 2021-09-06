/*
 * Keyboard related functions
 *
 * Copyright 1993 Bob Amstadt
 * Copyright 1996 Albrecht Kleine
 * Copyright 1997 David Faure
 * Copyright 1998 Morten Welinder
 * Copyright 1998 Ulrich Weigand
 * Copyright 1999 Ove Kåven
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
 * Copyright 2013 Alexandre Julliard
 * Copyright 2015 Josh DuBois for CodeWeavers Inc.
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#include "ntuser.h"

#include <linux/input.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(keyboard);
WINE_DECLARE_DEBUG_CHANNEL(key);

/* xkb_keysym_t fixed maps from wayland_keyboard_layout.h. */
extern const WORD xkb_keysym_0xff00_to_vkey[256];
extern const WORD xkb_keysym_0xff00_to_scan[256];
extern const WORD xkb_keysym_xfree86_to_vkey[256];
extern const WORD xkb_keysym_xfree86_to_scan[256];

static const struct
{
    DWORD       vkey;
    const char *name;
} vkey_names[] = {
    { VK_ADD,                   "Num +" },
    { VK_BACK,                  "Backspace" },
    { VK_CAPITAL,               "Caps Lock" },
    { VK_CONTROL,               "Ctrl" },
    { VK_DECIMAL,               "Num Del" },
    { VK_DELETE,                "Delete" },
    { VK_DIVIDE,                "Num /" },
    { VK_DOWN,                  "Down" },
    { VK_END,                   "End" },
    { VK_ESCAPE,                "Esc" },
    { VK_F1,                    "F1" },
    { VK_F2,                    "F2" },
    { VK_F3,                    "F3" },
    { VK_F4,                    "F4" },
    { VK_F5,                    "F5" },
    { VK_F6,                    "F6" },
    { VK_F7,                    "F7" },
    { VK_F8,                    "F8" },
    { VK_F9,                    "F9" },
    { VK_F10,                   "F10" },
    { VK_F11,                   "F11" },
    { VK_F12,                   "F12" },
    { VK_F13,                   "F13" },
    { VK_F14,                   "F14" },
    { VK_F15,                   "F15" },
    { VK_F16,                   "F16" },
    { VK_F17,                   "F17" },
    { VK_F18,                   "F18" },
    { VK_F19,                   "F19" },
    { VK_F20,                   "F20" },
    { VK_F21,                   "F21" },
    { VK_F22,                   "F22" },
    { VK_F23,                   "F23" },
    { VK_F24,                   "F24" },
    { VK_HELP,                  "Help" },
    { VK_HOME,                  "Home" },
    { VK_INSERT,                "Insert" },
    { VK_LCONTROL,              "Ctrl" },
    { VK_LEFT,                  "Left" },
    { VK_LMENU,                 "Alt" },
    { VK_LSHIFT,                "Shift" },
    { VK_LWIN,                  "Win" },
    { VK_MENU,                  "Alt" },
    { VK_MULTIPLY,              "Num *" },
    { VK_NEXT,                  "Page Down" },
    { VK_NUMLOCK,               "Num Lock" },
    { VK_NUMPAD0,               "Num 0" },
    { VK_NUMPAD1,               "Num 1" },
    { VK_NUMPAD2,               "Num 2" },
    { VK_NUMPAD3,               "Num 3" },
    { VK_NUMPAD4,               "Num 4" },
    { VK_NUMPAD5,               "Num 5" },
    { VK_NUMPAD6,               "Num 6" },
    { VK_NUMPAD7,               "Num 7" },
    { VK_NUMPAD8,               "Num 8" },
    { VK_NUMPAD9,               "Num 9" },
    { VK_OEM_CLEAR,             "Num Clear" },
    { VK_OEM_NEC_EQUAL,         "Num =" },
    { VK_PRIOR,                 "Page Up" },
    { VK_RCONTROL,              "Right Ctrl" },
    { VK_RETURN,                "Return" },
    { VK_RETURN,                "Num Enter" },
    { VK_RIGHT,                 "Right" },
    { VK_RMENU,                 "Right Alt" },
    { VK_RSHIFT,                "Right Shift" },
    { VK_RWIN,                  "Right Win" },
    { VK_SEPARATOR,             "Num ," },
    { VK_SHIFT,                 "Shift" },
    { VK_SPACE,                 "Space" },
    { VK_SUBTRACT,              "Num -" },
    { VK_TAB,                   "Tab" },
    { VK_UP,                    "Up" },
    { VK_VOLUME_DOWN,           "Volume Down" },
    { VK_VOLUME_MUTE,           "Mute" },
    { VK_VOLUME_UP,             "Volume Up" },
    { VK_OEM_MINUS,             "-" },
    { VK_OEM_PLUS,              "=" },
    { VK_OEM_1,                 ";" },
    { VK_OEM_2,                 "/" },
    { VK_OEM_3,                 "`" },
    { VK_OEM_4,                 "[" },
    { VK_OEM_5,                 "\\" },
    { VK_OEM_6,                 "]" },
    { VK_OEM_7,                 "'" },
    { VK_OEM_COMMA,             "," },
    { VK_OEM_PERIOD,            "." },
};

static DWORD _xkb_keycode_to_scancode(struct wayland_keyboard *keyboard,
                                      xkb_keycode_t xkb_keycode)
{
    return xkb_keycode < ARRAY_SIZE(keyboard->xkb_keycode_to_scancode) ?
           keyboard->xkb_keycode_to_scancode[xkb_keycode] : 0;
}

static xkb_keycode_t scancode_to_xkb_keycode(struct wayland_keyboard *keyboard, WORD scan)
{
    UINT j;

    if (scan == 0) return 0;

    for (j = 0; j < ARRAY_SIZE(keyboard->xkb_keycode_to_scancode); j++)
        if (keyboard->xkb_keycode_to_scancode[j] == scan) return j;

    return 0;
}

static UINT _xkb_keycode_to_vkey(struct wayland_keyboard *keyboard,
                                 xkb_keycode_t xkb_keycode)
{
    return xkb_keycode < ARRAY_SIZE(keyboard->xkb_keycode_to_vkey) ?
           keyboard->xkb_keycode_to_vkey[xkb_keycode] : 0;
}

static xkb_keycode_t vkey_to_xkb_keycode(struct wayland_keyboard *keyboard, UINT vkey)
{
    xkb_keycode_t i, candidate = 0;

    if (vkey == 0) return 0;

    switch (vkey)
    {
    case VK_NUMPAD0: vkey = VK_INSERT; break;
    case VK_NUMPAD1: vkey = VK_END; break;
    case VK_NUMPAD2: vkey = VK_DOWN; break;
    case VK_NUMPAD3: vkey = VK_NEXT; break;
    case VK_NUMPAD4: vkey = VK_LEFT; break;
    case VK_NUMPAD5: vkey = VK_CLEAR; break;
    case VK_NUMPAD6: vkey = VK_RIGHT; break;
    case VK_NUMPAD7: vkey = VK_HOME; break;
    case VK_NUMPAD8: vkey = VK_UP; break;
    case VK_NUMPAD9: vkey = VK_PRIOR; break;
    case VK_DECIMAL: vkey = VK_DELETE; break;

    case VK_INSERT: case VK_END: case VK_DOWN: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_HOME: case VK_UP:
    case VK_PRIOR: case VK_DELETE:
        vkey |= 0xe000;
        break;
    default: break;
    }

    for (i = 0; i < ARRAY_SIZE(keyboard->xkb_keycode_to_vkey); i++)
    {
        if (keyboard->xkb_keycode_to_vkey[i] == (vkey & 0xff))
        {
            candidate = i;
            if ((keyboard->xkb_keycode_to_scancode[i] & 0xff00) == (vkey & 0xff00))
                break;
        }
    }

    return candidate;
}

static WORD scancode_to_vkey_fixed(UINT scan)
{
    int i;

    if (scan == 0) return 0;

    for (i = 0; i <= 0xff; i++)
    {
        if (xkb_keysym_0xff00_to_scan[i] == scan &&
            xkb_keysym_0xff00_to_vkey[i] != 0)
        {
            return xkb_keysym_0xff00_to_vkey[i];
        }
    }

    for (i = 0; i <= 0xff; i++)
    {
        if (xkb_keysym_xfree86_to_scan[i] == scan &&
            xkb_keysym_xfree86_to_vkey[i] != 0)
        {
            return xkb_keysym_xfree86_to_vkey[i];
        }
    }

    return 0;
}

static UINT scancode_to_vkey(struct wayland_keyboard *keyboard, DWORD scan)
{
    UINT vkey = _xkb_keycode_to_vkey(keyboard, scancode_to_xkb_keycode(keyboard, scan));
    if (!vkey) vkey = scancode_to_vkey_fixed(scan);
    return vkey;
}

static const char* vkey_to_name(UINT vkey)
{
    UINT j;

    for (j = 0; j < ARRAY_SIZE(vkey_names); j++)
        if (vkey_names[j].vkey == vkey)
            return vkey_names[j].name;

    return NULL;
}

/* xkb keycodes are offset by 8 from linux input keycodes. */
static inline xkb_keycode_t linux_input_keycode_to_xkb(uint32_t key)
{
    return key + 8;
}

static void send_keyboard_input(HWND hwnd, WORD vkey, WORD scan, DWORD flags)
{
    INPUT input;

    input.type             = INPUT_KEYBOARD;
    input.u.ki.wVk         = vkey;
    input.u.ki.wScan       = scan;
    input.u.ki.dwFlags     = flags;
    input.u.ki.time        = 0;
    input.u.ki.dwExtraInfo = 0;

    __wine_send_input(hwnd, &input, NULL);
}

static WCHAR dead_xkb_keysym_to_wchar(xkb_keysym_t xkb_keysym)
{
    switch (xkb_keysym)
    {
    case XKB_KEY_dead_grave: return 0x0060;
    case XKB_KEY_dead_acute: return 0x00B4;
    case XKB_KEY_dead_circumflex: return 0x005E;
    case XKB_KEY_dead_tilde: return 0x007E;
    case XKB_KEY_dead_macron: return 0x00AF;
    case XKB_KEY_dead_breve: return 0x02D8;
    case XKB_KEY_dead_abovedot: return 0x02D9;
    case XKB_KEY_dead_diaeresis: return 0x00A8;
    case XKB_KEY_dead_abovering: return 0x02DA;
    case XKB_KEY_dead_doubleacute: return 0x02DD;
    case XKB_KEY_dead_caron: return 0x02C7;
    case XKB_KEY_dead_cedilla: return 0x00B8;
    case XKB_KEY_dead_ogonek: return 0x02DB;
    case XKB_KEY_dead_iota: return 0x037A;
    case XKB_KEY_dead_voiced_sound: return 0x309B;
    case XKB_KEY_dead_semivoiced_sound: return 0x309C;
    case XKB_KEY_dead_belowdot: return 0x002E;
    case XKB_KEY_dead_stroke: return 0x002D;
    case XKB_KEY_dead_abovecomma: return 0x1FBF;
    case XKB_KEY_dead_abovereversedcomma: return 0x1FFE;
    case XKB_KEY_dead_doublegrave: return 0x02F5;
    case XKB_KEY_dead_belowring: return 0x02F3;
    case XKB_KEY_dead_belowmacron: return 0x02CD;
    case XKB_KEY_dead_belowtilde: return 0x02F7;
    case XKB_KEY_dead_currency: return 0x00A4;
    case XKB_KEY_dead_lowline: return 0x005F;
    case XKB_KEY_dead_aboveverticalline: return 0x02C8;
    case XKB_KEY_dead_belowverticalline: return 0x02CC;
    case XKB_KEY_dead_longsolidusoverlay: return 0x002F;
    case XKB_KEY_dead_a: return 0x0061;
    case XKB_KEY_dead_A: return 0x0041;
    case XKB_KEY_dead_e: return 0x0065;
    case XKB_KEY_dead_E: return 0x0045;
    case XKB_KEY_dead_i: return 0x0069;
    case XKB_KEY_dead_I: return 0x0049;
    case XKB_KEY_dead_o: return 0x006F;
    case XKB_KEY_dead_O: return 0x004F;
    case XKB_KEY_dead_u: return 0x0075;
    case XKB_KEY_dead_U: return 0x0055;
    case XKB_KEY_dead_small_schwa: return 0x0259;
    case XKB_KEY_dead_capital_schwa: return 0x018F;
    /* The following are non-spacing characters, couldn't find good
     * spacing alternatives. */
    case XKB_KEY_dead_hook: return 0x0309;
    case XKB_KEY_dead_horn: return 0x031B;
    case XKB_KEY_dead_belowcircumflex: return 0x032D;
    case XKB_KEY_dead_belowbreve: return 0x032E;
    case XKB_KEY_dead_belowdiaeresis: return 0x0324;
    case XKB_KEY_dead_invertedbreve: return 0x0311;
    case XKB_KEY_dead_belowcomma: return 0x0326;
    default: return 0;
    }
}

static BOOL _xkb_keycode_is_keypad_num(xkb_keycode_t xkb_keycode)
{
    switch (xkb_keycode - 8)
    {
    case KEY_KP0: case KEY_KP1: case KEY_KP2: case KEY_KP3:
    case KEY_KP4: case KEY_KP5: case KEY_KP6: case KEY_KP7:
    case KEY_KP8: case KEY_KP9: case KEY_KPDOT:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Get the vkey corresponding to an xkb keycode, potentially translating it to
 * take into account the current keyboard state. */
static UINT translate_xkb_keycode_to_vkey(struct wayland_keyboard *keyboard,
                                          xkb_keycode_t xkb_keycode)
{
    UINT vkey = _xkb_keycode_to_vkey(keyboard, xkb_keycode);

    if (_xkb_keycode_is_keypad_num(xkb_keycode) &&
        xkb_state_mod_name_is_active(keyboard->xkb_state, XKB_MOD_NAME_NUM,
                                     XKB_STATE_MODS_EFFECTIVE))
    {
        switch (vkey)
        {
        case VK_INSERT: vkey = VK_NUMPAD0; break;
        case VK_END: vkey = VK_NUMPAD1; break;
        case VK_DOWN: vkey = VK_NUMPAD2; break;
        case VK_NEXT: vkey = VK_NUMPAD3; break;
        case VK_LEFT: vkey = VK_NUMPAD4; break;
        case VK_CLEAR: vkey = VK_NUMPAD5; break;
        case VK_RIGHT: vkey = VK_NUMPAD6; break;
        case VK_HOME: vkey = VK_NUMPAD7; break;
        case VK_UP: vkey = VK_NUMPAD8; break;
        case VK_PRIOR: vkey = VK_NUMPAD9; break;
        case VK_DELETE: vkey = VK_DECIMAL; break;
        default: break;
        }
    }
    else if (vkey == VK_PAUSE &&
             xkb_state_mod_name_is_active(keyboard->xkb_state,
                                          XKB_MOD_NAME_CTRL,
                                          XKB_STATE_MODS_EFFECTIVE))
    {
        vkey = VK_CANCEL;
    }

    return vkey;
}

static void wayland_keyboard_emit(struct wayland_keyboard *keyboard, uint32_t key,
                                  uint32_t state, HWND hwnd)
{
    xkb_keycode_t xkb_keycode = linux_input_keycode_to_xkb(key);
    UINT vkey = translate_xkb_keycode_to_vkey(keyboard, xkb_keycode);
    UINT scan = _xkb_keycode_to_scancode(keyboard, xkb_keycode);
    DWORD flags;

    TRACE_(key)("xkb_keycode=%u vkey=0x%x scan=0x%x state=%d hwnd=%p\n",
                xkb_keycode, vkey, scan, state, hwnd);

    if (vkey == 0) return;

    flags = 0;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) flags |= KEYEVENTF_KEYUP;
    if (scan & 0xff00) flags |= KEYEVENTF_EXTENDEDKEY;

    send_keyboard_input(hwnd, vkey, scan & 0xff, flags);
}

static struct xkb_state *_xkb_state_new_from_wine(struct wayland_keyboard *keyboard,
                                                  const BYTE *keystate)
{
    struct xkb_state *xkb_state;
    UINT mods[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU};
    UINT toggles[] = {VK_CAPITAL, VK_NUMLOCK, VK_SCROLL};
    int i;

    /* Create a new xkb_state using the currently active layout. */
    xkb_state = xkb_state_new(xkb_state_get_keymap(keyboard->xkb_state));
    if (!xkb_state) return NULL;
    xkb_state_update_mask(xkb_state, 0, 0, 0, 0, 0,
                          xkb_state_serialize_layout(keyboard->xkb_state,
                                                     XKB_STATE_LAYOUT_LOCKED));

    /* Update the xkb_state from the windows keyboard state by simulating
     * keypresses. */
    for (i = 0 ; i < ARRAY_SIZE(mods); i++)
    {
        if ((keystate[mods[i]] & 0x80))
        {
            xkb_state_update_key(xkb_state,
                                 vkey_to_xkb_keycode(keyboard, mods[i]),
                                 XKB_KEY_DOWN);
        }
    }

    for (i = 0 ; i < ARRAY_SIZE(toggles); i++)
    {
        if ((keystate[toggles[i]] & 0x01))
        {
            xkb_state_update_key(xkb_state,
                                 vkey_to_xkb_keycode(keyboard, toggles[i]),
                                 XKB_KEY_DOWN);
            xkb_state_update_key(xkb_state,
                                 vkey_to_xkb_keycode(keyboard, toggles[i]),
                                 XKB_KEY_UP);

        }
    }

    return xkb_state;
}

/**********************************************************************
 *          Keyboard handling
 */

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                   uint32_t format, int fd, uint32_t size)
{
    struct wayland *wayland = data;
    struct xkb_keymap *xkb_keymap = NULL;
    struct xkb_state *xkb_state = NULL;
    char *keymap_str;

    TRACE("format=%d fd=%d size=%d\n", format, fd, size);

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ||
        !wayland->keyboard.xkb_context)
        goto out;

    keymap_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (keymap_str == MAP_FAILED)
        goto out;

    xkb_keymap = xkb_keymap_new_from_string(wayland->keyboard.xkb_context,
                                            keymap_str,
                                            XKB_KEYMAP_FORMAT_TEXT_V1,
                                            0);
    munmap(keymap_str, size);
    if (!xkb_keymap)
        goto out;

    xkb_state = xkb_state_new(xkb_keymap);
    xkb_keymap_unref(xkb_keymap);
    if (!xkb_state)
        goto out;

    xkb_state_unref(wayland->keyboard.xkb_state);
    wayland->keyboard.xkb_state = xkb_state;
    if (wayland->keyboard.xkb_compose_state)
        xkb_compose_state_reset(wayland->keyboard.xkb_compose_state);

    wayland_keyboard_update_layout(&wayland->keyboard);

out:
    close(fd);
}

static BOOL wayland_surface_for_window_is_mapped(HWND hwnd)
{
    DWORD_PTR res;

    if (!send_message_timeout(hwnd, WM_WAYLAND_QUERY_SURFACE_MAPPED,
                              0, 0, SMTO_BLOCK, 50, &res))
    {
        return FALSE;
    }

    return res;
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys)
{
    struct wayland *wayland = data;
    struct wayland_surface *wayland_surface =
        surface ? wl_surface_get_user_data(surface) : NULL;

    /* Since keyboard events can arrive in multiple threads, ensure we only
     * handle them in the thread that owns the surface, to avoid passing
     * duplicate events to Wine. */
    if (wayland_surface && wayland_surface->hwnd &&
        wayland_surface->wayland == wayland)
    {
        HWND foreground = NtUserGetForegroundWindow();
        BOOL foreground_is_visible;
        BOOL foreground_is_mapped;

        if (foreground == NtUserGetDesktopWindow()) foreground = NULL;
        if (foreground)
        {
            foreground_is_visible = NtUserGetWindowLongW(foreground, GWL_STYLE) & WS_VISIBLE;
            foreground_is_mapped = wayland_surface_for_window_is_mapped(foreground);
        }
        else
        {
            foreground_is_visible = FALSE;
            foreground_is_mapped = FALSE;
        }

        TRACE("surface=%p hwnd=%p foreground=%p visible=%d mapped=%d\n",
              wayland_surface, wayland_surface->hwnd, foreground,
              foreground_is_visible, foreground_is_mapped);

        wayland->keyboard.focused_surface = wayland_surface;
        wayland->keyboard.enter_serial = serial;

        /* Promote the just entered window to the foreground unless we have a
         * existing visible foreground window that is not mapped from the
         * Wayland perspective. In that case the surface may not have had the
         * chance to acquire the keyboard focus and if we change the foreground
         * window now, we may cause side effects, e.g., some fullscreen games
         * minimize if they lose focus. To avoid such side effects, err on the
         * side of maintaining the Wine foreground state, with the expectation
         * that the current foreground window will eventually also gain the
         * Wayland keyboard focus. */
        if (!foreground || !foreground_is_visible || foreground_is_mapped)
        {
            struct wayland_surface *toplevel = wayland_surface;
            while (toplevel->parent) toplevel = toplevel->parent;
            NtUserSetForegroundWindow(toplevel->hwnd);
        }
    }
}

static void CALLBACK maybe_unset_from_foreground(HWND hwnd, UINT msg,
                                                 UINT_PTR timer_id,
                                                 DWORD elapsed)
{
    struct wayland *wayland = thread_wayland();

    TRACE("wayland=%p hwnd=%p\n", wayland, hwnd);

    /* If no enter events have arrived since the previous leave event,
     * the loss of focus was likely not transient, so drop the foreground state.
     * We only drop the foreground state if it's ours to drop, i.e., some
     * other window hasn't become foreground in the meantime. */
    if (!wayland->keyboard.focused_surface && NtUserGetForegroundWindow() == hwnd)
        NtUserSetForegroundWindow(NtUserGetDesktopWindow());

    NtUserKillTimer(hwnd, timer_id);
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayland = data;
    struct wayland_surface *focused_surface = wayland->keyboard.focused_surface;

    if (focused_surface && focused_surface->wl_surface == surface)
    {
        TRACE("surface=%p hwnd=%p\n", focused_surface, focused_surface->hwnd);
        NtUserKillTimer(focused_surface->hwnd, (UINT_PTR)keyboard);
        /* This leave event may not signify a real loss of focus for the
         * window. Such a case occurs when the focus changes from the main
         * surface to a subsurface. Don't be too eager to lose the foreground
         * state in such cases, as some fullscreen applications may become
         * minimized. Instead wait a bit in case other enter events targeting a
         * (sub)surface of the same HWND arrive soon after. */
        NtUserSetTimer(focused_surface->hwnd,
                       (UINT_PTR)&wayland->keyboard.focused_surface, 50,
                       maybe_unset_from_foreground, TIMERV_DEFAULT_COALESCING);
        wayland->keyboard.focused_surface = NULL;
        wayland->keyboard.enter_serial = 0;
    }
}

static void CALLBACK repeat_key(HWND hwnd, UINT msg, UINT_PTR timer_id, DWORD elapsed)
{
    struct wayland *wayland = thread_wayland();

    if (wayland->keyboard.repeat_interval_ms > 0)
    {
        wayland_keyboard_emit(&wayland->keyboard, wayland->keyboard.pressed_key,
                              WL_KEYBOARD_KEY_STATE_PRESSED, hwnd);

        NtUserSetTimer(hwnd, timer_id, wayland->keyboard.repeat_interval_ms,
                       repeat_key, TIMERV_DEFAULT_COALESCING);
    }
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->keyboard.focused_surface ?
                        wayland->keyboard.focused_surface->hwnd : 0;
    UINT_PTR repeat_key_timer_id = (UINT_PTR)keyboard;

    if (!focused_hwnd)
        return;

    TRACE("key=%d state=%#x focused_hwnd=%p\n", key, state, focused_hwnd);

    wayland->last_dispatch_mask |= QS_KEY | QS_HOTKEY;

    wayland_keyboard_emit(&wayland->keyboard, key, state, focused_hwnd);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        wayland->keyboard.pressed_key = key;
        if (wayland->keyboard.repeat_interval_ms > 0)
        {
            NtUserSetTimer(focused_hwnd, repeat_key_timer_id, wayland->keyboard.repeat_delay_ms,
                           repeat_key, TIMERV_DEFAULT_COALESCING);
        }
    }
    else
    {
        wayland->keyboard.pressed_key = 0;
        NtUserKillTimer(focused_hwnd, repeat_key_timer_id);
    }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group)
{
    struct wayland *wayland = data;
    uint32_t last_group;

    TRACE("depressed=0x%x latched=0x%x locked=0x%x group=%d\n",
          mods_depressed, mods_latched, mods_locked, group);

    if (!wayland->keyboard.xkb_state) return;

    last_group = _xkb_state_get_active_layout(wayland->keyboard.xkb_state);

    xkb_state_update_mask(wayland->keyboard.xkb_state,
                          mods_depressed, mods_latched, mods_locked, 0, 0, group);

    if (group != last_group)
        wayland_keyboard_update_layout(&wayland->keyboard);

    /* TODO: Sync wine modifier state with XKB modifier state. */
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
                                        int rate, int delay)
{
    struct wayland *wayland = data;

    TRACE("rate=%d delay=%d\n", rate, delay);

    /* Handle non-negative rate values, ignore invalid (negative) values.  A
     * rate of 0 disables repeat. Note that a requested rate value larger than
     * 100 may not actually lead to the desired repeat rate, since we are
     * constrained by the USER_TIMER_MINIMUM (=10ms) resolution of win32
     * timers. */
    if (rate > 1000)
        wayland->keyboard.repeat_interval_ms = 1;
    else if (rate > 0)
        wayland->keyboard.repeat_interval_ms = 1000 / rate;
    else if (rate == 0)
        wayland->keyboard.repeat_interval_ms = 0;

    wayland->keyboard.repeat_delay_ms = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

/***********************************************************************
 *           wayland_keyboard_init
 */
void wayland_keyboard_init(struct wayland_keyboard *keyboard, struct wayland *wayland,
                           struct wl_keyboard *wl_keyboard)
{
    struct xkb_compose_table *compose_table;
    const char *locale;

    locale = getenv("LC_ALL");
    if (!locale || !*locale)
        locale = getenv("LC_CTYPE");
    if (!locale || !*locale)
        locale = getenv("LANG");
    if (!locale || !*locale)
        locale = "C";

    keyboard->wl_keyboard = wl_keyboard;
    /* Some sensible default values for the repeat rate and delay. */
    keyboard->repeat_interval_ms = 40;
    keyboard->repeat_delay_ms = 400;
    keyboard->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!keyboard->xkb_context)
    {
        ERR("Failed to create XKB context\n");
        return;
    }
    compose_table =
        xkb_compose_table_new_from_locale(keyboard->xkb_context, locale,
                                          XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!compose_table)
    {
        ERR("Failed to create XKB compose table\n");
        return;
    }

    keyboard->xkb_compose_state =
        xkb_compose_state_new(compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    xkb_compose_table_unref(compose_table);
    if (!keyboard->xkb_compose_state)
        ERR("Failed to create XKB compose table\n");

    wl_keyboard_add_listener(keyboard->wl_keyboard, &keyboard_listener, wayland);
}

/***********************************************************************
 *           wayland_keyboard_deinit
 */
void wayland_keyboard_deinit(struct wayland_keyboard *keyboard)
{
    if (keyboard->wl_keyboard)
        wl_keyboard_destroy(keyboard->wl_keyboard);

    xkb_compose_state_unref(keyboard->xkb_compose_state);
    xkb_state_unref(keyboard->xkb_state);
    xkb_context_unref(keyboard->xkb_context);

    memset(keyboard, 0, sizeof(*keyboard));
}

/***********************************************************************
 *           WAYLAND_ToUnicodeEx
 */
INT WAYLAND_ToUnicodeEx(UINT virt, UINT scan, const BYTE *state,
                        LPWSTR buf, int nchars, UINT flags, HKL hkl)
{
    struct wayland *wayland = thread_init_wayland();
    char utf8[64];
    int utf8_len = 0;
    struct xkb_compose_state *compose_state = wayland->keyboard.xkb_compose_state;
    enum xkb_compose_status compose_status = XKB_COMPOSE_NOTHING;
    xkb_keycode_t xkb_keycode;
    xkb_keysym_t xkb_keysym;
    struct xkb_state *xkb_state;
    INT ret;

    if (!wayland->keyboard.xkb_state) return 0;

    if (scan & 0x8000) return 0;  /* key up */

    xkb_keycode = vkey_to_xkb_keycode(&wayland->keyboard, virt);
    xkb_state = _xkb_state_new_from_wine(&wayland->keyboard, state);
    if (!xkb_state) return 0;

    /* Try to compose */
    xkb_keysym = xkb_state_key_get_one_sym(xkb_state, xkb_keycode);
    if (xkb_keysym != XKB_KEY_NoSymbol && compose_state &&
        xkb_compose_state_feed(compose_state, xkb_keysym) == XKB_COMPOSE_FEED_ACCEPTED)
    {
        compose_status = xkb_compose_state_get_status(compose_state);
    }

    TRACE_(key)("vkey=0x%x scan=0x%x xkb_keycode=%d xkb_keysym=0x%x compose_status=%d\n",
                virt, scan, xkb_keycode, xkb_keysym, compose_status);

    if (compose_status == XKB_COMPOSE_NOTHING)
    {
        /* Windows converts some Ctrl modified key combinations to strings in a
         * way different from Linux/xkbcommon (or doesn't convert them at all).
         * Handle such combinations manually here. */
        if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL,
                                         XKB_STATE_MODS_EFFECTIVE))
        {
            if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT,
                                             XKB_STATE_MODS_EFFECTIVE))
            {
                ret = 0;
                goto out;
            }
            if (((xkb_keysym >= XKB_KEY_exclam) && (xkb_keysym < XKB_KEY_at)) ||
                (xkb_keysym == XKB_KEY_grave) || (xkb_keysym == XKB_KEY_Tab))
            {
                ret = 0;
                goto out;
            }
            if (xkb_keysym == XKB_KEY_Return)
            {
                if (xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_SHIFT,
                                                 XKB_STATE_MODS_EFFECTIVE))
                {
                    ret = 0;
                }
                else
                {
                    buf[0] = '\n';
                    ret = 1;
                }
                goto out;
            }
            if (xkb_keysym == XKB_KEY_space)
            {
                buf[0] = ' ';
                ret = 1;
                goto out;
            }
        }

        utf8_len = xkb_state_key_get_utf8(xkb_state, xkb_keycode, utf8, sizeof(utf8));
    }
    else if (compose_status == XKB_COMPOSE_COMPOSED)
    {
        utf8_len = xkb_compose_state_get_utf8(compose_state, utf8, sizeof(utf8));
        TRACE_(key)("composed\n");
    }
    else if (compose_status == XKB_COMPOSE_COMPOSING && nchars > 0)
    {
        if ((buf[0] = dead_xkb_keysym_to_wchar(xkb_keysym)))
        {
            TRACE_(key)("returning dead char 0x%04x\n", buf[0]);
            buf[1] = 0;
            ret = -1;
            goto out;
        }
    }

    TRACE_(key)("utf8 len=%d '%s'\n", utf8_len, utf8_len ? utf8 : "");

    if (RtlUTF8ToUnicodeN(buf, nchars, (DWORD *)&ret, utf8, utf8_len)) ret = 0;
    else ret /= sizeof(WCHAR);

out:
    /* Zero terminate the returned string. */
    if (ret >= 0 && ret < nchars) buf[ret] = 0;
    xkb_state_unref(xkb_state);
    return ret;
}

/***********************************************************************
 *           GetKeyNameText
 */
INT WAYLAND_GetKeyNameText(LONG lparam, LPWSTR buffer, INT size)
{
    struct wayland *wayland = thread_init_wayland();
    int scan, vkey, len;
    const char *name;
    char key[2];

    scan = (lparam >> 16) & 0x1FF;
    vkey = scancode_to_vkey(&wayland->keyboard, scan);

    if (lparam & (1 << 25))
    {
        /* Caller doesn't care about distinctions between left and
           right keys. */
        switch (vkey)
        {
        case VK_LSHIFT:
        case VK_RSHIFT:
            vkey = VK_SHIFT; break;
        case VK_LCONTROL:
        case VK_RCONTROL:
            vkey = VK_CONTROL; break;
        case VK_LMENU:
        case VK_RMENU:
            vkey = VK_MENU; break;
        }
    }

    if ((vkey >= 0x30 && vkey <= 0x39) || (vkey >= 0x41 && vkey <= 0x5a))
    {
        key[0] = vkey;
        if (vkey >= 0x41)
            key[0] += 0x20;
        key[1] = 0;
        name = key;
    }
    else
    {
        name = vkey_to_name(vkey);
    }

    if (!name || RtlUTF8ToUnicodeN(buffer, size, (DWORD *)&len, name, strlen(name) + 1)) len = 0;
    else len = len / sizeof(WCHAR) - 1;

    if (!len)
    {
        char buf[16];
        len = snprintf(buf, sizeof(buf), "Key 0x%02x", vkey);
        if (len > sizeof(buf)) len = sizeof(buf);
        len = ascii_to_unicode_z(buffer, size, buf, len);
        if (len > 0) len--;
    }

    TRACE_(key)("lparam 0x%08x -> %s\n", lparam, debugstr_w(buffer));
    return len;
}
