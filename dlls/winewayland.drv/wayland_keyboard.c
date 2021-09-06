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

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "config.h"

#include "waylanddrv.h"

#include "wine/debug.h"

#include "winuser.h"

#include <sys/mman.h>
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(keyboard);
WINE_DECLARE_DEBUG_CHANNEL(key);

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

/* Get the vkey corresponding to an xkb keycode, potentially translating it to
 * take into account the current keyboard state. */
static UINT translate_xkb_keycode_to_vkey(struct wayland_keyboard *keyboard,
                                          xkb_keycode_t xkb_keycode)
{
    UINT vkey = _xkb_keycode_to_vkey(keyboard, xkb_keycode);

    if (((vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9) ||
          vkey == VK_SEPARATOR || vkey == VK_DECIMAL) &&
        !xkb_state_mod_name_is_active(keyboard->xkb_state, XKB_MOD_NAME_NUM,
                                      XKB_STATE_MODS_EFFECTIVE))
    {
        switch (vkey)
        {
        case VK_NUMPAD0: vkey = VK_INSERT; break;
        case VK_NUMPAD1: vkey = VK_END; break;
        case VK_NUMPAD2: vkey = VK_DOWN; break;
        case VK_NUMPAD3: vkey = VK_NEXT; break;
        case VK_NUMPAD4: vkey = VK_LEFT; break;
        case VK_NUMPAD5: vkey = 0; break;
        case VK_NUMPAD6: vkey = VK_RIGHT; break;
        case VK_NUMPAD7: vkey = VK_HOME; break;
        case VK_NUMPAD8: vkey = VK_UP; break;
        case VK_NUMPAD9: vkey = VK_PRIOR; break;
        case VK_SEPARATOR: vkey = VK_DELETE; break;
        case VK_DECIMAL: vkey = VK_DELETE; break;
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
    if (scan & 0x100) flags |= KEYEVENTF_EXTENDEDKEY;

    send_keyboard_input(hwnd, vkey, scan & 0xff, flags);
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
    if (!keymap_str)
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
        struct wayland_surface *toplevel = wayland_surface;

        TRACE("surface=%p hwnd=%p\n", wayland_surface, wayland_surface->hwnd);
        wayland->keyboard.focused_surface = wayland_surface;
        wayland->keyboard.enter_serial = serial;

        while (toplevel->parent) toplevel = toplevel->parent;
        SetForegroundWindow(toplevel->hwnd);
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
    if (!wayland->keyboard.focused_surface && GetForegroundWindow() == hwnd)
        SetForegroundWindow(GetDesktopWindow());

    KillTimer(hwnd, timer_id);
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayland = data;
    struct wayland_surface *focused_surface = wayland->keyboard.focused_surface;

    if (focused_surface && focused_surface->wl_surface == surface)
    {
        TRACE("surface=%p hwnd=%p\n", focused_surface, focused_surface->hwnd);
        KillTimer(focused_surface->hwnd, (UINT_PTR)keyboard);
        /* This leave event may not signify a real loss of focus for the
         * window. Such a case occurs when the focus changes from the main
         * surface to a subsurface. Don't be too eager to lose the foreground
         * state in such cases, as some fullscreen applications may become
         * minimized. Instead wait a bit in case other enter events targeting a
         * (sub)surface of the same HWND arrive soon after. */
        SetTimer(focused_surface->hwnd,
                 (UINT_PTR)&wayland->keyboard.focused_surface, 50,
                 maybe_unset_from_foreground);
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

        SetTimer(hwnd, timer_id, wayland->keyboard.repeat_interval_ms,
                 repeat_key);
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
            SetTimer(focused_hwnd, repeat_key_timer_id, wayland->keyboard.repeat_delay_ms,
                     repeat_key);
        }
    }
    else
    {
        wayland->keyboard.pressed_key = 0;
        KillTimer(focused_hwnd, repeat_key_timer_id);
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
