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

#include "wine/unicode.h"
#include "wine/server.h"
#include "wine/debug.h"

#include "waylanddrv.h"
#include "winuser.h"
#include "ime.h"

WINE_DEFAULT_DEBUG_CHANNEL(keyboard);
WINE_DECLARE_DEBUG_CHANNEL(key);

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

    for (j = 0; j < ARRAY_SIZE(keyboard->xkb_keycode_to_scancode); j++)
        if ((keyboard->xkb_keycode_to_scancode[j] & 0xff) == (scan & 0xff))
            return j;

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
    xkb_keycode_t i;

    for (i = 0; i < ARRAY_SIZE(keyboard->xkb_keycode_to_vkey); i++)
    {
        if (keyboard->xkb_keycode_to_vkey[i] == vkey)
            return i;
    }

    return 0;
}

static WORD vkey_to_scancode(struct wayland_keyboard *keyboard, UINT vkey)
{
    return _xkb_keycode_to_scancode(keyboard, vkey_to_xkb_keycode(keyboard, vkey));
}

static UINT scancode_to_vkey(struct wayland_keyboard *keyboard, DWORD scan)
{
    return _xkb_keycode_to_vkey(keyboard, scancode_to_xkb_keycode(keyboard, scan));
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

static WCHAR _xkb_keysyms_to_wchar(const xkb_keysym_t *syms, int nsyms)
{
    char utf8[64];
    int utf8_len;
    WCHAR wchars[8];
    WCHAR normalized[8];
    int nchars;

    utf8_len = _xkb_keysyms_to_utf8(syms, nsyms, utf8, sizeof(utf8));

    nchars = MultiByteToWideChar(CP_UTF8, 0, utf8, utf8_len, wchars,
                                 ARRAY_SIZE(wchars));
    if (nchars == 0)
        return 0;

    if (NormalizeString(NormalizationC, wchars, nchars, normalized,
                        ARRAY_SIZE(normalized)) != 1)
        return 0;

    return normalized[0];
}

static SHORT _xkb_mod_mask_to_win32(struct xkb_keymap *xkb_keymap,
                                    xkb_mod_mask_t mod_mask)
{
    xkb_mod_index_t num_mods, i;
    SHORT ret = 0;

    num_mods = xkb_keymap_num_mods(xkb_keymap);
    for (i = 0; i < num_mods; i++)
    {
        if (mod_mask & (1 << i))
        {
            const char *mod_name = xkb_keymap_mod_get_name(xkb_keymap, i);

            if (!strcmp(mod_name, XKB_MOD_NAME_SHIFT))
                ret |= 0x0100;
            else if (!strcmp(mod_name, XKB_MOD_NAME_CTRL))
                ret |= 0x0200;
            else if (!strcmp(mod_name, XKB_MOD_NAME_ALT))
                ret |= 0x0400;
        }
    }

    return ret;
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

static UINT map_vkey_to_wchar_with_deadchar_bit(struct wayland_keyboard *keyboard, UINT vkey)
{
    WCHAR wchar;
    xkb_keycode_t xkb_keycode;
    struct xkb_keymap *xkb_keymap;
    xkb_layout_index_t layout;
    const xkb_keysym_t *syms;
    int nsyms;

    if (!keyboard->xkb_state) return 0;

    layout = _xkb_state_get_active_layout(keyboard->xkb_state);
    if (layout == XKB_LAYOUT_INVALID)
    {
        TRACE_(key)("no active layout, returning wchar 0\n");
        return 0;
    }

    xkb_keymap = xkb_state_get_keymap(keyboard->xkb_state);
    xkb_keycode = vkey_to_xkb_keycode(keyboard, vkey);

    nsyms = xkb_keymap_key_get_syms_by_level(xkb_keymap, xkb_keycode,
                                             layout, 0, &syms);
    if (nsyms > 0)
    {
        /* Set the high bit to 1 if this is dead char. */
        if ((wchar = dead_xkb_keysym_to_wchar(syms[0])))
            wchar |= 0x80000000;
        else
            wchar = _xkb_keysyms_to_wchar(syms, nsyms);
    }
    else
    {
        wchar = 0;
    }

    TRACE_(key)("vkey=0x%x xkb_keycode=%d nsyms=%d xkb_keysym[0]=0x%x => wchar=0x%x\n",
                vkey, xkb_keycode, nsyms, nsyms ? syms[0] : 0, wchar);

    return wchar;
}

/***********************************************************************
 *           wayland_keyboard_emit
 *
 * Emits a keyboard event to a window. The key and state arguments
 * are interpreted according to the wl_keyboard documentation.
 */
void wayland_keyboard_emit(struct wayland_keyboard *keyboard, uint32_t key,
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

/***********************************************************************
 *           WAYLAND_ToUnicodeEx
 */
INT CDECL WAYLAND_ToUnicodeEx(UINT virt, UINT scan, const BYTE *state,
                              LPWSTR buf, int nchars, UINT flags, HKL hkl)
{
    struct wayland *wayland = thread_init_wayland();
    char utf8[64];
    int utf8_len = 0;
    struct xkb_compose_state *compose_state = wayland->keyboard.xkb_compose_state;
    enum xkb_compose_status compose_status = XKB_COMPOSE_NOTHING;
    xkb_keycode_t xkb_keycode;
    xkb_keysym_t xkb_keysym;

    if (!wayland->keyboard.xkb_state) return 0;

    if (scan & 0x8000) return 0;  /* key up */

    xkb_keycode = vkey_to_xkb_keycode(&wayland->keyboard, virt);

    /* Try to compose */
    xkb_keysym = xkb_state_key_get_one_sym(wayland->keyboard.xkb_state, xkb_keycode);
    if (xkb_keysym != XKB_KEY_NoSymbol && compose_state &&
        xkb_compose_state_feed(compose_state, xkb_keysym) == XKB_COMPOSE_FEED_ACCEPTED)
    {
        compose_status = xkb_compose_state_get_status(compose_state);
    }

    TRACE_(key)("vkey=0x%x scan=0x%x xkb_keycode=%d xkb_keysym=0x%x compose_status=%d\n",
                virt, scan, xkb_keycode, xkb_keysym, compose_status);

    if (compose_status == XKB_COMPOSE_NOTHING)
    {
        utf8_len = xkb_state_key_get_utf8(wayland->keyboard.xkb_state,
                                          xkb_keycode, utf8, sizeof(utf8));
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
            return -1;
        }
    }

    TRACE_(key)("utf8 len=%d '%s'\n", utf8_len, utf8_len ? utf8 : "");

    return MultiByteToWideChar(CP_UTF8, 0, utf8, utf8_len, buf, nchars);
}

/***********************************************************************
 *           GetKeyNameText
 */
INT CDECL WAYLAND_GetKeyNameText(LONG lparam, LPWSTR buffer, INT size)
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

    len = MultiByteToWideChar(CP_UTF8, 0, name, -1, buffer, size);
    if (len) len--;

    if (!len)
    {
        static const WCHAR format[] = {'K','e','y',' ','0','x','%','0','2','x',0};
        snprintfW(buffer, size, format, vkey);
        len = strlenW(buffer);
    }

    TRACE_(key)("lparam 0x%08x -> %s\n", lparam, debugstr_w(buffer));
    return len;
}

/***********************************************************************
 *           WAYLAND_MapVirtualKeyEx
 */
UINT CDECL WAYLAND_MapVirtualKeyEx(UINT code, UINT maptype, HKL hkl)
{
    struct wayland *wayland = thread_init_wayland();
    UINT ret = 0;

    TRACE_(key)("code=0x%x, maptype=%d, hkl %p\n", code, maptype, hkl);

    switch (maptype)
    {
    case MAPVK_VK_TO_VSC_EX:
    case MAPVK_VK_TO_VSC:
        /* vkey to scancode */
        switch (code)
        {
        case VK_SHIFT:
            code = VK_LSHIFT;
            break;
        case VK_CONTROL:
            code = VK_LCONTROL;
            break;
        case VK_MENU:
            code = VK_LMENU;
            break;
        }

        ret = vkey_to_scancode(&wayland->keyboard, code);

        /* set scan code prefix */
        if (maptype == MAPVK_VK_TO_VSC_EX &&
            (code == VK_RCONTROL || code == VK_RMENU))
            ret |= 0xe000;
        break;
    case MAPVK_VSC_TO_VK:
    case MAPVK_VSC_TO_VK_EX:
        /* scancode to vkey */
        ret = scancode_to_vkey(&wayland->keyboard, code);
        if (maptype == MAPVK_VSC_TO_VK)
        {
            switch (ret)
            {
            case VK_LSHIFT:
            case VK_RSHIFT:
                ret = VK_SHIFT; break;
            case VK_LCONTROL:
            case VK_RCONTROL:
                ret = VK_CONTROL; break;
            case VK_LMENU:
            case VK_RMENU:
                ret = VK_MENU; break;
            }
        }
        break;
    case MAPVK_VK_TO_CHAR:
        ret = map_vkey_to_wchar_with_deadchar_bit(&wayland->keyboard, code);
        break;
    default:
        FIXME("Unknown maptype %d\n", maptype);
        break;
    }
    TRACE_(key)("returning 0x%04x\n", ret);
    return ret;
}


/***********************************************************************
 *           WAYLAND_GetKeyboardLayout
 */
HKL CDECL WAYLAND_GetKeyboardLayout(DWORD thread_id)
{
    ULONG_PTR layout = GetUserDefaultLCID();
    LANGID langid;

    langid = PRIMARYLANGID(LANGIDFROMLCID(layout));
    if (langid == LANG_CHINESE || langid == LANG_JAPANESE || langid == LANG_KOREAN)
        layout = MAKELONG(layout, 0xe001); /* IME */
    else
        layout |= layout << 16;

    return (HKL)layout;
}

/***********************************************************************
 *           WAYLAND_VkKeyScanEx
 */
SHORT CDECL WAYLAND_VkKeyScanEx(WCHAR ch, HKL hkl)
{
    struct wayland *wayland = thread_init_wayland();
    xkb_layout_index_t layout;
    struct xkb_state *xkb_state = wayland->keyboard.xkb_state;
    struct xkb_keymap *xkb_keymap;
    xkb_keycode_t xkb_keycode, min_xkb_keycode, max_xkb_keycode;

    TRACE_(key)("ch %04x hkl %p ...\n", ch, hkl);

    if (!xkb_state)
    {
        TRACE_(key)("... no xkb state , returning -1\n");
        return -1;
    }

    layout = _xkb_state_get_active_layout(xkb_state);
    if (layout == XKB_LAYOUT_INVALID)
    {
        TRACE_(key)("... no active layout, returning -1\n");
        return -1;
    }

    xkb_keymap = xkb_state_get_keymap(xkb_state);
    min_xkb_keycode = xkb_keymap_min_keycode(xkb_keymap);
    max_xkb_keycode = xkb_keymap_max_keycode(xkb_keymap);

    /* Search through all keycodes and their shift levels for one that
     * produces a keysym that matches the requested character. */
    for (xkb_keycode = min_xkb_keycode; xkb_keycode <= max_xkb_keycode; xkb_keycode++)
    {
        xkb_level_index_t num_levels =
            xkb_keymap_num_levels_for_key(xkb_keymap, xkb_keycode, layout);
        xkb_level_index_t level;

        for (level = 0; level < num_levels; level++)
        {
            const xkb_keysym_t *syms;
            int nsyms = xkb_keymap_key_get_syms_by_level(xkb_keymap, xkb_keycode,
                                                         layout, level, &syms);

            if (_xkb_keysyms_to_wchar(syms, nsyms) == ch)
            {
                UINT vkey;
                xkb_mod_mask_t mod_mask;
                SHORT ret;

                vkey = _xkb_keycode_to_vkey(&wayland->keyboard, xkb_keycode);
                if (vkey == 0)
                    continue;

                xkb_keymap_key_get_mods_for_level(xkb_keymap, xkb_keycode,
                                                  layout, level, &mod_mask, 1);
                ret = _xkb_mod_mask_to_win32(xkb_keymap, mod_mask) | vkey;

                TRACE_(key)("... returning %04x\n", ret);
                return ret;
            }
        }
    }

    TRACE_(key)("... matching vkey not found, returning -1\n");
    return -1;
}
