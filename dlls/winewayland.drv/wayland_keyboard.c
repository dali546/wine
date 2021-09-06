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
#include "ime.h"

#include <linux/input.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wayland_keyboard_layout.h"

WINE_DEFAULT_DEBUG_CHANNEL(keyboard);
WINE_DECLARE_DEBUG_CHANNEL(key);

static xkb_layout_index_t _xkb_state_get_active_layout(struct xkb_state *xkb_state)
{
    struct xkb_keymap *xkb_keymap = xkb_state_get_keymap(xkb_state);
    xkb_layout_index_t num_layouts = xkb_keymap_num_layouts(xkb_keymap);
    xkb_layout_index_t layout;

    for (layout = 0; layout < num_layouts; layout++)
    {
        if (xkb_state_layout_index_is_active(xkb_state, layout,
                                             XKB_STATE_LAYOUT_LOCKED))
            return layout;
    }

    return XKB_LAYOUT_INVALID;
}

/**********************************************************************
 *          _xkb_keysyms_to_utf8
 *
 * Get the null-terminated UTF-8 string representation of a sequence of
 * keysyms. Returns the length of the UTF-8 string written, *not* including
 * the null byte. If no bytes were produced or in case of error returns 0
 * and produces a properly null-terminated empty string if possible.
 */
static int _xkb_keysyms_to_utf8(const xkb_keysym_t *syms, int nsyms, char *utf8,
                                int utf8_size)
{
    int i;
    int utf8_len = 0;

    if (utf8_size == 0) return 0;

    for (i = 0; i < nsyms; i++)
    {
        int nwritten = xkb_keysym_to_utf8(syms[i], utf8 + utf8_len,
                                          utf8_size - utf8_len);
        if (nwritten <= 0)
        {
            utf8_len = 0;
            break;
        }

        /* nwritten includes the terminating null byte */
        utf8_len += nwritten - 1;
    }

    utf8[utf8_len] = '\0';

    return utf8_len;
}

static int score_symbols(const xkb_keysym_t sym[MAIN_KEY_SYMBOLS_LEN],
                         const xkb_keysym_t ref[MAIN_KEY_SYMBOLS_LEN])
{
    int score = 0, i;

    for (i = 0; i < MAIN_KEY_SYMBOLS_LEN && ref[i]; i++)
    {
        if (ref[i] != sym[i]) return 0;
        score++;
    }

    return score;
}

static int score_layout(int layout,
                        const xkb_keysym_t symbols_for_keycode[256][MAIN_KEY_SYMBOLS_LEN])
{
    xkb_keycode_t xkb_keycode;
    int score = 0;
    int prev_key = 1000;
    char key_used[MAIN_KEY_LEN] = { 0 };

    for (xkb_keycode = 0; xkb_keycode < 256; xkb_keycode++)
    {
        int key, key_score = 0;
        const xkb_keysym_t *symbols = symbols_for_keycode[xkb_keycode];

        if (*symbols == 0)
            continue;

        for (key = 0; key < MAIN_KEY_LEN; key++)
        {
            if (key_used[key]) continue;
            key_score = score_symbols(symbols_for_keycode[xkb_keycode],
                                      (*main_key_tab[layout].symbols)[key]);
            if (key_score)
                break;
        }

        if (TRACE_ON(key))
        {
            char utf8[64];
            _xkb_keysyms_to_utf8(symbols, MAIN_KEY_SYMBOLS_LEN, utf8, sizeof(utf8));
            TRACE_(key)("xkb_keycode=%d syms={0x%x,0x%x} utf8='%s' key=%d score=%d order=%d\n",
                        xkb_keycode,
                        symbols_for_keycode[xkb_keycode][0],
                        symbols_for_keycode[xkb_keycode][1],
                        utf8, key, key_score, key_score && (key > prev_key));
        }

        if (key_score)
        {
            /* Multiply score by 100 to allow the key order bonus to break ties,
             * while not being a primary decision factor. */
            score += key_score * 100;

            /* xkb keycodes roughly follow a top left to bottom right direction
             * on the keyboard as they increase, similarly to the keys in
             * main_key_tab. Give a bonus to layouts that more closely match
             * the expected ordering. We compare with the last key to get
             * some reasonable (although local) measure of the order. */
            score += (key > prev_key);
            prev_key = key;
            key_used[key] = 1;
        }
    }

    return score;
}

static void _xkb_keymap_populate_symbols_for_keycode(
    struct xkb_keymap *xkb_keymap,
    xkb_layout_index_t layout,
    xkb_keysym_t symbols_for_keycode[256][MAIN_KEY_SYMBOLS_LEN])
{
    xkb_keycode_t xkb_keycode, min_xkb_keycode, max_xkb_keycode;

    min_xkb_keycode = xkb_keymap_min_keycode(xkb_keymap);
    max_xkb_keycode = xkb_keymap_max_keycode(xkb_keymap);
    if (max_xkb_keycode > 255) max_xkb_keycode = 255;

    for (xkb_keycode = min_xkb_keycode; xkb_keycode <= max_xkb_keycode; xkb_keycode++)
    {
        xkb_level_index_t num_levels =
            xkb_keymap_num_levels_for_key(xkb_keymap, xkb_keycode, layout);
        xkb_level_index_t level;

        if (num_levels > MAIN_KEY_SYMBOLS_LEN) num_levels = MAIN_KEY_SYMBOLS_LEN;

        for (level = 0; level < num_levels; level++)
        {
            const xkb_keysym_t *syms;
            int nsyms = xkb_keymap_key_get_syms_by_level(xkb_keymap, xkb_keycode,
                                                         layout, level, &syms);
            if (nsyms)
                symbols_for_keycode[xkb_keycode][level] = syms[0];
        }
    }
}

static int detect_main_key_layout(struct wayland_keyboard *keyboard,
                                  const xkb_keysym_t symbols_for_keycode[256][MAIN_KEY_SYMBOLS_LEN])
{
    int max_score = 0;
    int max_i = 0;

    for (int i = 0; i < ARRAY_SIZE(main_key_tab); i++)
    {
        int score = score_layout(i, symbols_for_keycode);
        if (score > max_score)
        {
            max_i = i;
            max_score = score;
        }
        TRACE("evaluated layout '%s' score %d\n", main_key_tab[i].name, score);
    }

    if (max_score == 0)
    {
        max_i = 0;
        while (strcmp(main_key_tab[max_i].name, "us")) max_i++;
        TRACE("failed to detect layout, falling back to layout 'us'\n");
    }
    else
    {
        TRACE("detected layout '%s' (score %d)\n", main_key_tab[max_i].name, max_score);
    }

    return max_i;
}

/* Populate the xkb_keycode_to_vkey[] and xkb_keycode_to_scan[] arrays based on
 * the specified main_key layout (see wayland_keyboard_layout.h) and the
 * xkb_keycode to xkb_keysym_t mappings which have been created from the
 * currently active Wayland keymap. */
static void populate_xkb_keycode_maps(struct wayland_keyboard *keyboard, int main_key_layout,
                                      const xkb_keysym_t symbols_for_keycode[256][MAIN_KEY_SYMBOLS_LEN])
{
    xkb_keycode_t xkb_keycode;
    char key_used[MAIN_KEY_LEN] = { 0 };
    const xkb_keysym_t (*lsymbols)[MAIN_KEY_SYMBOLS_LEN] =
        (*main_key_tab[main_key_layout].symbols);
    const WORD *lvkey = (*main_key_tab[main_key_layout].vkey);
    const WORD *lscan = (*main_key_tab[main_key_layout].scan);

    for (xkb_keycode = 0; xkb_keycode < 256; xkb_keycode++)
    {
        int max_key = -1;
        int max_score = 0;
        xkb_keysym_t xkb_keysym = symbols_for_keycode[xkb_keycode][0];
        UINT vkey = 0;
        WORD scan = 0;

        if ((xkb_keysym >> 8) == 0xFF)
        {
            vkey = xkb_keysym_0xff00_to_vkey[xkb_keysym & 0xff];
            scan = xkb_keysym_0xff00_to_scan[xkb_keysym & 0xff];
        }
        else if ((xkb_keysym >> 8) == 0x1008FF)
        {
            vkey = xkb_keysym_xfree86_to_vkey[xkb_keysym & 0xff];
            scan = xkb_keysym_xfree86_to_scan[xkb_keysym & 0xff];
        }
        else if (xkb_keysym == 0x20)
        {
            vkey = VK_SPACE;
            scan = 0x39;
        }
        else
        {
            int key;

            for (key = 0; key < MAIN_KEY_LEN; key++)
            {
                int score = score_symbols(symbols_for_keycode[xkb_keycode],
                                          lsymbols[key]);
                /* Consider this key if it has a better score, or the same
                 * score as a previous match that is already in use (in order
                 * to prefer unused keys). */
                if (score > max_score ||
                    (max_key >= 0 && score == max_score && key_used[max_key]))
                {
                    max_key = key;
                    max_score = score;
                }
            }

            if (max_key >= 0)
            {
                vkey = lvkey[max_key];
                scan = lscan[max_key];
                key_used[max_key] = 1;
            }
        }

        keyboard->xkb_keycode_to_vkey[xkb_keycode] = vkey;
        keyboard->xkb_keycode_to_scancode[xkb_keycode] = scan;

        if (TRACE_ON(key))
        {
            char utf8[64];
            _xkb_keysyms_to_utf8(symbols_for_keycode[xkb_keycode],
                                 MAIN_KEY_SYMBOLS_LEN, utf8, sizeof(utf8));
            TRACE_(key)("Mapped xkb_keycode=%d syms={0x%x,0x%x} utf8='%s' => "
                        "vkey=0x%x scan=0x%x\n",
                        xkb_keycode,
                        symbols_for_keycode[xkb_keycode][0],
                        symbols_for_keycode[xkb_keycode][1],
                        utf8, vkey, scan);
        }
    }
}

/***********************************************************************
 *           wayland_keyboard_update_layout
 *
 * Updates the internal weston_keyboard layout information (xkb keycode
 * mappings etc) based on the current XKB layout.
 */
static void wayland_keyboard_update_layout(struct wayland_keyboard *keyboard)
{
    xkb_layout_index_t layout;
    struct xkb_state *xkb_state = keyboard->xkb_state;
    struct xkb_keymap *xkb_keymap;
    xkb_keysym_t symbols_for_keycode[256][MAIN_KEY_SYMBOLS_LEN] = { 0 };
    int main_key_layout;

    if (!xkb_state)
    {
        TRACE("no xkb state, returning\n");
        return;
    }

    layout = _xkb_state_get_active_layout(xkb_state);
    if (layout == XKB_LAYOUT_INVALID)
    {
        TRACE("no active layout, returning\n");
        return;
    }

    xkb_keymap = xkb_state_get_keymap(xkb_state);

    _xkb_keymap_populate_symbols_for_keycode(xkb_keymap, layout, symbols_for_keycode);

    main_key_layout = detect_main_key_layout(keyboard, symbols_for_keycode);

    populate_xkb_keycode_maps(keyboard, main_key_layout, symbols_for_keycode);
}

static DWORD _xkb_keycode_to_scancode(struct wayland_keyboard *keyboard,
                                      xkb_keycode_t xkb_keycode)
{
    return xkb_keycode < ARRAY_SIZE(keyboard->xkb_keycode_to_scancode) ?
           keyboard->xkb_keycode_to_scancode[xkb_keycode] : 0;
}

static UINT _xkb_keycode_to_vkey(struct wayland_keyboard *keyboard,
                                 xkb_keycode_t xkb_keycode)
{
    return xkb_keycode < ARRAY_SIZE(keyboard->xkb_keycode_to_vkey) ?
           keyboard->xkb_keycode_to_vkey[xkb_keycode] : 0;
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

static BOOL wayland_keyboard_emit(struct wayland_keyboard *keyboard, uint32_t key,
                                  uint32_t state, HWND hwnd)
{
    xkb_keycode_t xkb_keycode = linux_input_keycode_to_xkb(key);
    UINT vkey = translate_xkb_keycode_to_vkey(keyboard, xkb_keycode);
    UINT scan = _xkb_keycode_to_scancode(keyboard, xkb_keycode);
    DWORD flags;

    TRACE_(key)("xkb_keycode=%u vkey=0x%x scan=0x%x state=%d hwnd=%p\n",
                xkb_keycode, vkey, scan, state, hwnd);

    if (vkey == 0) return FALSE;

    flags = 0;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) flags |= KEYEVENTF_KEYUP;
    if (scan & 0xff00) flags |= KEYEVENTF_EXTENDEDKEY;

    send_keyboard_input(hwnd, vkey, scan & 0xff, flags);

    return TRUE;
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
            foreground_is_visible =
                !!(NtUserGetWindowLongW(foreground, GWL_STYLE) & WS_VISIBLE);
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

static void maybe_unset_from_foreground(void *data)
{
    struct wayland *wayland = thread_wayland();
    HWND hwnd = (HWND)data;

    TRACE("wayland=%p hwnd=%p\n", wayland, hwnd);

    /* If no enter events have arrived since the previous leave event,
     * the loss of focus was likely not transient, so drop the foreground state.
     * We only drop the foreground state if it's ours to drop, i.e., some
     * other window hasn't become foreground in the meantime. */
    if (!wayland->keyboard.focused_surface && NtUserGetForegroundWindow() == hwnd)
        NtUserSetForegroundWindow(NtUserGetDesktopWindow());
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayland = data;
    struct wayland_surface *focused_surface = wayland->keyboard.focused_surface;

    if (focused_surface && focused_surface->wl_surface == surface)
    {
        TRACE("surface=%p hwnd=%p\n", focused_surface, focused_surface->hwnd);
        wayland_cancel_thread_callback((uintptr_t)keyboard);
        /* This leave event may not signify a real loss of focus for the
         * window. Such a case occurs when the focus changes from the main
         * surface to a subsurface. Don't be too eager to lose the foreground
         * state in such cases, as some fullscreen applications may become
         * minimized. Instead wait a bit in case other enter events targeting a
         * (sub)surface of the same HWND arrive soon after. */
        wayland_schedule_thread_callback((uintptr_t)&wayland->keyboard.focused_surface,
                                         50, maybe_unset_from_foreground,
                                         focused_surface->hwnd);
        wayland->keyboard.focused_surface = NULL;
        wayland->keyboard.enter_serial = 0;
    }
}

static void repeat_key(void *data)
{
    struct wayland *wayland = thread_wayland();
    HWND hwnd = data;

    if (wayland->keyboard.repeat_interval_ms > 0)
    {
        wayland->last_dispatch_mask |= QS_KEY | QS_HOTKEY;

        wayland_keyboard_emit(&wayland->keyboard, wayland->keyboard.last_pressed_key,
                              WL_KEYBOARD_KEY_STATE_PRESSED, hwnd);

        wayland_schedule_thread_callback((uintptr_t)wayland->keyboard.wl_keyboard,
                                         wayland->keyboard.repeat_interval_ms,
                                         repeat_key, hwnd);
    }
}

static BOOL wayland_keyboard_is_modifier_key(struct wayland_keyboard *keyboard,
                                             uint32_t key)
{
    xkb_keycode_t xkb_keycode = linux_input_keycode_to_xkb(key);
    UINT vkey = _xkb_keycode_to_vkey(keyboard, xkb_keycode);

    return vkey == VK_CAPITAL || vkey == VK_LWIN || vkey == VK_RWIN ||
           vkey == VK_NUMLOCK || vkey == VK_SCROLL ||
           vkey == VK_LSHIFT || vkey == VK_RSHIFT ||
           vkey == VK_LCONTROL || vkey == VK_RCONTROL ||
           vkey == VK_LMENU || vkey == VK_RMENU;
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    struct wayland *wayland = data;
    HWND focused_hwnd = wayland->keyboard.focused_surface ?
                        wayland->keyboard.focused_surface->hwnd : 0;
    uintptr_t repeat_key_timer_id = (uintptr_t)keyboard;

    if (!focused_hwnd)
        return;

    TRACE("key=%d state=%#x focused_hwnd=%p\n", key, state, focused_hwnd);

    wayland->last_dispatch_mask |= QS_KEY | QS_HOTKEY;

    if (!wayland_keyboard_emit(&wayland->keyboard, key, state, focused_hwnd))
        return;

    /* Do not repeat modifier keys. */
    if (wayland_keyboard_is_modifier_key(&wayland->keyboard, key))
        return;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        wayland->keyboard.last_pressed_key = key;
        if (wayland->keyboard.repeat_interval_ms > 0)
        {
            wayland_schedule_thread_callback(repeat_key_timer_id,
                                             wayland->keyboard.repeat_delay_ms,
                                             repeat_key, focused_hwnd);
        }
    }
    else if (key == wayland->keyboard.last_pressed_key)
    {
        wayland->keyboard.last_pressed_key = 0;
        wayland_cancel_thread_callback(repeat_key_timer_id);
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
     * rate of 0 disables repeat. */
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

    wl_keyboard_add_listener(keyboard->wl_keyboard, &keyboard_listener, wayland);
}

/***********************************************************************
 *           wayland_keyboard_deinit
 */
void wayland_keyboard_deinit(struct wayland_keyboard *keyboard)
{
    if (keyboard->wl_keyboard)
        wl_keyboard_destroy(keyboard->wl_keyboard);

    xkb_state_unref(keyboard->xkb_state);
    xkb_context_unref(keyboard->xkb_context);

    memset(keyboard, 0, sizeof(*keyboard));
}
