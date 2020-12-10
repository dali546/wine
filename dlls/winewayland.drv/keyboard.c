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

WINE_DEFAULT_DEBUG_CHANNEL(keyboard);
WINE_DECLARE_DEBUG_CHANNEL(key);

static const UINT keycode_to_vkey[] =
{
    0,                   /* KEY_RESERVED  0 */
    VK_ESCAPE,           /* KEY_ESC   1 */
    '1',                 /* KEY_1   2 */
    '2',                 /* KEY_2   3 */
    '3',                 /* KEY_3   4 */
    '4',                 /* KEY_4   5 */
    '5',                 /* KEY_5   6 */
    '6',                 /* KEY_6   7 */
    '7',                 /* KEY_7   8 */
    '8',                 /* KEY_8   9 */
    '9',                 /* KEY_9   10 */
    '0',                 /* KEY_0   11 */
    VK_OEM_MINUS,        /* KEY_MINUS  12 */
    VK_OEM_PLUS,         /* KEY_EQUAL  13 */
    VK_BACK,             /* KEY_BACKSPACE 14 */
    VK_TAB,              /* KEY_TAB   15 */
    'Q',                 /* KEY_Q   16 */
    'W',                 /* KEY_W   17 */
    'E',                 /* KEY_E   18 */
    'R',                 /* KEY_R   19 */
    'T',                 /* KEY_T   20 */
    'Y',                 /* KEY_Y   21 */
    'U',                 /* KEY_U   22 */
    'I',                 /* KEY_I   23 */
    'O',                 /* KEY_O   24 */
    'P',                 /* KEY_P   25 */
    VK_OEM_4,            /* KEY_LEFTBRACE  26 */
    VK_OEM_6,            /* KEY_RIGHTBRACE  27 */
    VK_RETURN,           /* KEY_ENTER  28 */
    VK_LCONTROL,         /* KEY_LEFTCTRL  29 */
    'A',                 /* KEY_A   30 */
    'S',                 /* KEY_S   31 */
    'D',                 /* KEY_D   32 */
    'F',                 /* KEY_F   33 */
    'G',                 /* KEY_G   34 */
    'H',                 /* KEY_H   35 */
    'J',                 /* KEY_J   36 */
    'K',                 /* KEY_K   37 */
    'L',                 /* KEY_L   38 */
    VK_OEM_1,            /* KEY_SEMICOLON  39 */
    VK_OEM_7,            /* KEY_APOSTROPHE  40 */
    VK_OEM_3,            /* KEY_GRAVE  41 */
    VK_LSHIFT,           /* KEY_LEFTSHIFT  42 */
    VK_OEM_5,            /* KEY_BACKSLASH  43 */
    'Z',                 /* KEY_Z   44 */
    'X',                 /* KEY_X   45 */
    'C',                 /* KEY_C   46 */
    'V',                 /* KEY_V   47 */
    'B',                 /* KEY_B   48 */
    'N',                 /* KEY_N   49 */
    'M',                 /* KEY_M   50 */
    VK_OEM_COMMA,        /* KEY_COMMA  51 */
    VK_OEM_PERIOD,       /* KEY_DOT   52 */
    VK_OEM_2,            /* KEY_SLASH  53 */
    VK_RSHIFT,           /* KEY_RIGHTSHIFT  54 */
    VK_MULTIPLY,         /* KEY_KPASTERISK  55 */
    VK_LMENU,            /* KEY_LEFTALT  56 */
    VK_SPACE,            /* KEY_SPACE  57 */
    VK_CAPITAL,          /* KEY_CAPSLOCK  58 */
    VK_F1,               /* KEY_F1   59 */
    VK_F2,               /* KEY_F2   60 */
    VK_F3,               /* KEY_F3   61 */
    VK_F4,               /* KEY_F4   62 */
    VK_F5,               /* KEY_F5   63 */
    VK_F6,               /* KEY_F6   64 */
    VK_F7,               /* KEY_F7   65 */
    VK_F8,               /* KEY_F8   66 */
    VK_F9,               /* KEY_F9   67 */
    VK_F10,              /* KEY_F10   68 */
    VK_NUMLOCK,          /* KEY_NUMLOCK  69 */
    VK_SCROLL,           /* KEY_SCROLLLOCK  70 */
    VK_NUMPAD7,          /* KEY_KP7   71 */
    VK_NUMPAD8,          /* KEY_KP8   72 */
    VK_NUMPAD9,          /* KEY_KP9   73 */
    VK_SUBTRACT,         /* KEY_KPMINUS  74 */
    VK_NUMPAD4,          /* KEY_KP4   75 */
    VK_NUMPAD5,          /* KEY_KP5   76 */
    VK_NUMPAD6,          /* KEY_KP6   77 */
    VK_ADD,              /* KEY_KPPLUS  78 */
    VK_NUMPAD1,          /* KEY_KP1   79 */
    VK_NUMPAD2,          /* KEY_KP2   80 */
    VK_NUMPAD3,          /* KEY_KP3   81 */
    VK_NUMPAD0,          /* KEY_KP0   82 */
    VK_DECIMAL,          /* KEY_KPDOT  83 */
    0,                   /* 84 */
    0,                   /* KEY_ZENKAKUHANKAKU 85 */
    VK_OEM_102,          /* KEY_102ND  86 */
    VK_F11,              /* KEY_F11   87 */
    VK_F12,              /* KEY_F12   88 */
    0,                   /* KEY_RO   89 */
    0,                   /* KEY_KATAKANA  90 */
    0,                   /* KEY_HIRAGANA  91 */
    0,                   /* KEY_HENKAN  92 */
    0,                   /* KEY_KATAKANAHIRAGANA 93 */
    0,                   /* KEY_MUHENKAN  94 */
    0,                   /* KEY_KPJPCOMMA  95 */
    VK_RETURN,           /* KEY_KPENTER  96 */
    VK_RCONTROL,         /* KEY_RIGHTCTRL  97 */
    VK_DIVIDE,           /* KEY_KPSLASH  98 */
    VK_SNAPSHOT,         /* KEY_SYSRQ  99 */
    VK_RMENU,            /* KEY_RIGHTALT  100 */
    0,                   /* KEY_LINEFEED  101 */
    VK_HOME,             /* KEY_HOME  102 */
    VK_UP,               /* KEY_UP   103 */
    VK_PRIOR,            /* KEY_PAGEUP  104 */
    VK_LEFT,             /* KEY_LEFT  105 */
    VK_RIGHT,            /* KEY_RIGHT  106 */
    VK_END,              /* KEY_END   107 */
    VK_DOWN,             /* KEY_DOWN  108 */
    VK_NEXT,             /* KEY_PAGEDOWN  109 */
    VK_INSERT,           /* KEY_INSERT  110 */
    VK_DELETE,           /* KEY_DELETE  111 */
    0,                   /* KEY_MACRO  112 */
    VK_VOLUME_MUTE,      /* KEY_MUTE  113 */
    VK_VOLUME_DOWN,      /* KEY_VOLUMEDOWN  114 */
    VK_VOLUME_UP,        /* KEY_VOLUMEUP  115 */
    0,                   /* KEY_POWER  116  */
    0,                   /* KEY_KPEQUAL  117 */
    0,                   /* KEY_KPPLUSMINUS  118 */
    VK_PAUSE,            /* KEY_PAUSE  119 */
    0,                   /* KEY_SCALE  120  */
    0,                   /* KEY_KPCOMMA  121 */
    0,                   /* KEY_HANGEUL  122 */
    0,                   /* KEY_HANJA  123 */
    0,                   /* KEY_YEN   124 */
    VK_LWIN,             /* KEY_LEFTMETA  125 */
    VK_RWIN,             /* KEY_RIGHTMETA  126 */
    0,                   /* KEY_COMPOSE  127 */
    0,                   /* KEY_STOP  128  */
    0,                   /* KEY_AGAIN  129 */
    0,                   /* KEY_PROPS  130  */
    0,                   /* KEY_UNDO  131  */
    0,                   /* KEY_FRONT  132 */
    0,                   /* KEY_COPY  133  */
    0,                   /* KEY_OPEN  134  */
    0,                   /* KEY_PASTE  135  */
    0,                   /* KEY_FIND  136  */
    0,                   /* KEY_CUT   137  */
    0,                   /* KEY_HELP  138  */
    0,                   /* KEY_MENU  139  */
    0,                   /* KEY_CALC  140  */
    0,                   /* KEY_SETUP  141 */
    0,                   /* KEY_SLEEP  142  */
    0,                   /* KEY_WAKEUP  143  */
    0,                   /* KEY_FILE  144  */
    0,                   /* KEY_SENDFILE  145 */
    0,                   /* KEY_DELETEFILE  146 */
    0,                   /* KEY_XFER  147 */
    0,                   /* KEY_PROG1  148 */
    0,                   /* KEY_PROG2  149 */
    0,                   /* KEY_WWW   150  */
    0,                   /* KEY_MSDOS  151 */
    0,                   /* KEY_COFFEE  152 */
    0,                   /* KEY_ROTATE_DISPLAY 153  */
    0,                   /* KEY_CYCLEWINDOWS 154 */
    0,                   /* KEY_MAIL  155 */
    0,                   /* KEY_BOOKMARKS  156  */
    0,                   /* KEY_COMPUTER  157 */
    0,                   /* KEY_BACK  158  */
    0,                   /* KEY_FORWARD  159  */
    0,                   /* KEY_CLOSECD  160 */
    0,                   /* KEY_EJECTCD  161 */
    0,                   /* KEY_EJECTCLOSECD 162 */
    VK_MEDIA_NEXT_TRACK, /* KEY_NEXTSONG  163 */
    VK_MEDIA_PLAY_PAUSE, /* KEY_PLAYPAUSE  164 */
    VK_MEDIA_PREV_TRACK, /* KEY_PREVIOUSSONG 165 */
    0,                   /* KEY_STOPCD  166 */
    0,                   /* KEY_RECORD  167 */
    0,                   /* KEY_REWIND  168 */
    0,                   /* KEY_PHONE  169  */
    0,                   /* KEY_ISO   170 */
    0,                   /* KEY_CONFIG  171  */
    0,                   /* KEY_HOMEPAGE  172  */
    0,                   /* KEY_REFRESH  173  */
    0,                   /* KEY_EXIT  174  */
    0,                   /* KEY_MOVE  175 */
    0,                   /* KEY_EDIT  176 */
    0,                   /* KEY_SCROLLUP  177 */
    0,                   /* KEY_SCROLLDOWN  178 */
    0,                   /* KEY_KPLEFTPAREN  179 */
    0,                   /* KEY_KPRIGHTPAREN 180 */
    0,                   /* KEY_NEW   181  */
    0,                   /* KEY_REDO  182  */
    VK_F13,              /* KEY_F13   183 */
    VK_F14,              /* KEY_F14   184 */
    VK_F15,              /* KEY_F15   185 */
    VK_F16,              /* KEY_F16   186 */
    VK_F17,              /* KEY_F17   187 */
    VK_F18,              /* KEY_F18   188 */
    VK_F19,              /* KEY_F19   189 */
    VK_F20,              /* KEY_F20   190 */
    VK_F21,              /* KEY_F21   191 */
    VK_F22,              /* KEY_F22   192 */
    VK_F23,              /* KEY_F23   193 */
    VK_F24,              /* KEY_F24   194 */
    0,                   /* 195 */
    0,                   /* 196 */
    0,                   /* 197 */
    0,                   /* 198 */
    0,                   /* 199 */
    0,                   /* KEY_PLAYCD  200 */
    0,                   /* KEY_PAUSECD  201 */
    0,                   /* KEY_PROG3  202 */
    0,                   /* KEY_PROG4  203 */
    0,                   /* KEY_DASHBOARD  204  */
    0,                   /* KEY_SUSPEND  205 */
    0,                   /* KEY_CLOSE  206  */
    VK_PLAY,             /* KEY_PLAY  207 */
    0,                   /* KEY_FASTFORWARD  208 */
    0,                   /* KEY_BASSBOOST  209 */
    VK_PRINT,            /* KEY_PRINT  210  */
    0,                   /* KEY_HP   211 */
    0,                   /* KEY_CAMERA  212 */
    0,                   /* KEY_SOUND  213 */
    0,                   /* KEY_QUESTION  214  */
    0,                   /* KEY_EMAIL  215 */
    0,                   /* KEY_CHAT  216 */
    0,                   /* KEY_SEARCH  217 */
    0,                   /* KEY_CONNECT  218 */
    0,                   /* KEY_FINANCE  219  */
    0,                   /* KEY_SPORT  220 */
    0,                   /* KEY_SHOP  221 */
    0,                   /* KEY_ALTERASE  222 */
    0,                   /* KEY_CANCEL  223  */
    0,                   /* KEY_BRIGHTNESSDOWN 224 */
    0,                   /* KEY_BRIGHTNESSUP 225 */
    0,                   /* KEY_MEDIA  226 */
    0,                   /* KEY_SWITCHVIDEOMODE 227  */
    0,                   /* KEY_KBDILLUMTOGGLE 228 */
    0,                   /* KEY_KBDILLUMDOWN 229 */
    0,                   /* KEY_KBDILLUMUP  230 */
    0,                   /* KEY_SEND  231  */
    0,                   /* KEY_REPLY  232  */
    0,                   /* KEY_FORWARDMAIL  233  */
    0,                   /* KEY_SAVE  234  */
    0,                   /* KEY_DOCUMENTS  235 */
    0,                   /* KEY_BATTERY  236 */
    0,                   /* KEY_BLUETOOTH  237 */
    0,                   /* KEY_WLAN  238 */
    0,                   /* KEY_UWB   239  */
    0,                   /* KEY_UNKNOWN  240 */
    0,                   /* KEY_VIDEO_NEXT  241  */
    0,                   /* KEY_VIDEO_PREV  242  */
    0,                   /* KEY_BRIGHTNESS_CYCLE 243  */
    0,                   /* KEY_BRIGHTNESS_AUTO/ZERO 244 */
    0,                   /* KEY_DISPLAY_OFF  245  */
    0,                   /* KEY_WWAN  246  */
    0,                   /* KEY_RFKILL  247  */
    0,                   /* KEY_MICMUTE  248  */
};

static const WORD vkey_to_scancode[] =
{
    0,     /* 0x00 undefined */
    0,     /* VK_LBUTTON */
    0,     /* VK_RBUTTON */
    0,     /* VK_CANCEL */
    0,     /* VK_MBUTTON */
    0,     /* VK_XBUTTON1 */
    0,     /* VK_XBUTTON2 */
    0,     /* 0x07 undefined */
    0x0e,  /* VK_BACK */
    0x0f,  /* VK_TAB */
    0,     /* 0x0a undefined */
    0,     /* 0x0b undefined */
    0,     /* VK_CLEAR */
    0x1c,  /* VK_RETURN */
    0,     /* 0x0e undefined */
    0,     /* 0x0f undefined */
    0x2a,  /* VK_SHIFT */
    0x1d,  /* VK_CONTROL */
    0x38,  /* VK_MENU */
    0x45,  /* VK_PAUSE */
    0x3a,  /* VK_CAPITAL */
    0,     /* VK_KANA */
    0,     /* 0x16 undefined */
    0,     /* VK_JUNJA */
    0,     /* VK_FINAL */
    0,     /* VK_HANJA */
    0,     /* 0x1a undefined */
    0x01,  /* VK_ESCAPE */
    0,     /* VK_CONVERT */
    0,     /* VK_NONCONVERT */
    0,     /* VK_ACCEPT */
    0,     /* VK_MODECHANGE */
    0x39,  /* VK_SPACE */
    0x149, /* VK_PRIOR */
    0x151, /* VK_NEXT */
    0x14f, /* VK_END */
    0x147, /* VK_HOME */
    0x14b, /* VK_LEFT */
    0x148, /* VK_UP */
    0x14d, /* VK_RIGHT */
    0x150, /* VK_DOWN */
    0,     /* VK_SELECT */
    0,     /* VK_PRINT */
    0,     /* VK_EXECUTE */
    0,     /* VK_SNAPSHOT */
    0x152, /* VK_INSERT */
    0x153, /* VK_DELETE */
    0,     /* VK_HELP */
    0x0b,  /* VK_0 */
    0x02,  /* VK_1 */
    0x03,  /* VK_2 */
    0x04,  /* VK_3 */
    0x05,  /* VK_4 */
    0x06,  /* VK_5 */
    0x07,  /* VK_6 */
    0x08,  /* VK_7 */
    0x09,  /* VK_8 */
    0x0a,  /* VK_9 */
    0,     /* 0x3a undefined */
    0,     /* 0x3b undefined */
    0,     /* 0x3c undefined */
    0,     /* 0x3d undefined */
    0,     /* 0x3e undefined */
    0,     /* 0x3f undefined */
    0,     /* 0x40 undefined */
    0x1e,  /* VK_A */
    0x30,  /* VK_B */
    0x2e,  /* VK_C */
    0x20,  /* VK_D */
    0x12,  /* VK_E */
    0x21,  /* VK_F */
    0x22,  /* VK_G */
    0x23,  /* VK_H */
    0x17,  /* VK_I */
    0x24,  /* VK_J */
    0x25,  /* VK_K */
    0x26,  /* VK_L */
    0x32,  /* VK_M */
    0x31,  /* VK_N */
    0x18,  /* VK_O */
    0x19,  /* VK_P */
    0x10,  /* VK_Q */
    0x13,  /* VK_R */
    0x1f,  /* VK_S */
    0x14,  /* VK_T */
    0x16,  /* VK_U */
    0x2f,  /* VK_V */
    0x11,  /* VK_W */
    0x2d,  /* VK_X */
    0x15,  /* VK_Y */
    0x2c,  /* VK_Z */
    0x15b, /* VK_LWIN */
    0x15c, /* VK_RWIN */
    0,     /* VK_APPS */
    0,     /* 0x5e undefined */
    0,     /* VK_SLEEP */
    0x52,  /* VK_NUMPAD0 */
    0x4f,  /* VK_NUMPAD1 */
    0x50,  /* VK_NUMPAD2 */
    0x51,  /* VK_NUMPAD3 */
    0x4b,  /* VK_NUMPAD4 */
    0x4c,  /* VK_NUMPAD5 */
    0x4d,  /* VK_NUMPAD6 */
    0x47,  /* VK_NUMPAD7 */
    0x48,  /* VK_NUMPAD8 */
    0x49,  /* VK_NUMPAD9 */
    0x37,  /* VK_MULTIPLY */
    0x4e,  /* VK_ADD */
    0x7e,  /* VK_SEPARATOR */
    0x4a,  /* VK_SUBTRACT */
    0x53,  /* VK_DECIMAL */
    0135,  /* VK_DIVIDE */
    0x3b,  /* VK_F1 */
    0x3c,  /* VK_F2 */
    0x3d,  /* VK_F3 */
    0x3e,  /* VK_F4 */
    0x3f,  /* VK_F5 */
    0x40,  /* VK_F6 */
    0x41,  /* VK_F7 */
    0x42,  /* VK_F8 */
    0x43,  /* VK_F9 */
    0x44,  /* VK_F10 */
    0x57,  /* VK_F11 */
    0x58,  /* VK_F12 */
    0x64,  /* VK_F13 */
    0x65,  /* VK_F14 */
    0x66,  /* VK_F15 */
    0x67,  /* VK_F16 */
    0x68,  /* VK_F17 */
    0x69,  /* VK_F18 */
    0x6a,  /* VK_F19 */
    0x6b,  /* VK_F20 */
    0,     /* VK_F21 */
    0,     /* VK_F22 */
    0,     /* VK_F23 */
    0,     /* VK_F24 */
    0,     /* 0x88 undefined */
    0,     /* 0x89 undefined */
    0,     /* 0x8a undefined */
    0,     /* 0x8b undefined */
    0,     /* 0x8c undefined */
    0,     /* 0x8d undefined */
    0,     /* 0x8e undefined */
    0,     /* 0x8f undefined */
    0,     /* VK_NUMLOCK */
    0x46,  /* VK_SCROLL */
    0x10d, /* VK_OEM_NEC_EQUAL */
    0,     /* VK_OEM_FJ_JISHO */
    0,     /* VK_OEM_FJ_MASSHOU */
    0,     /* VK_OEM_FJ_TOUROKU */
    0,     /* VK_OEM_FJ_LOYA */
    0,     /* VK_OEM_FJ_ROYA */
    0,     /* 0x97 undefined */
    0,     /* 0x98 undefined */
    0,     /* 0x99 undefined */
    0,     /* 0x9a undefined */
    0,     /* 0x9b undefined */
    0,     /* 0x9c undefined */
    0,     /* 0x9d undefined */
    0,     /* 0x9e undefined */
    0,     /* 0x9f undefined */
    0x2a,  /* VK_LSHIFT */
    0x36,  /* VK_RSHIFT */
    0x1d,  /* VK_LCONTROL */
    0x11d, /* VK_RCONTROL */
    0x38,  /* VK_LMENU */
    0x138, /* VK_RMENU */
    0,     /* VK_BROWSER_BACK */
    0,     /* VK_BROWSER_FORWARD */
    0,     /* VK_BROWSER_REFRESH */
    0,     /* VK_BROWSER_STOP */
    0,     /* VK_BROWSER_SEARCH */
    0,     /* VK_BROWSER_FAVORITES */
    0,     /* VK_BROWSER_HOME */
    0x100, /* VK_VOLUME_MUTE */
    0x100, /* VK_VOLUME_DOWN */
    0x100, /* VK_VOLUME_UP */
    0,     /* VK_MEDIA_NEXT_TRACK */
    0,     /* VK_MEDIA_PREV_TRACK */
    0,     /* VK_MEDIA_STOP */
    0,     /* VK_MEDIA_PLAY_PAUSE */
    0,     /* VK_LAUNCH_MAIL */
    0,     /* VK_LAUNCH_MEDIA_SELECT */
    0,     /* VK_LAUNCH_APP1 */
    0,     /* VK_LAUNCH_APP2 */
    0,     /* 0xb8 undefined */
    0,     /* 0xb9 undefined */
    0x27,  /* VK_OEM_1 */
    0x0d,  /* VK_OEM_PLUS */
    0x33,  /* VK_OEM_COMMA */
    0x0c,  /* VK_OEM_MINUS */
    0x34,  /* VK_OEM_PERIOD */
    0x35,  /* VK_OEM_2 */
    0x29,  /* VK_OEM_3 */
    0,     /* 0xc1 undefined */
    0,     /* 0xc2 undefined */
    0,     /* 0xc3 undefined */
    0,     /* 0xc4 undefined */
    0,     /* 0xc5 undefined */
    0,     /* 0xc6 undefined */
    0,     /* 0xc7 undefined */
    0,     /* 0xc8 undefined */
    0,     /* 0xc9 undefined */
    0,     /* 0xca undefined */
    0,     /* 0xcb undefined */
    0,     /* 0xcc undefined */
    0,     /* 0xcd undefined */
    0,     /* 0xce undefined */
    0,     /* 0xcf undefined */
    0,     /* 0xd0 undefined */
    0,     /* 0xd1 undefined */
    0,     /* 0xd2 undefined */
    0,     /* 0xd3 undefined */
    0,     /* 0xd4 undefined */
    0,     /* 0xd5 undefined */
    0,     /* 0xd6 undefined */
    0,     /* 0xd7 undefined */
    0,     /* 0xd8 undefined */
    0,     /* 0xd9 undefined */
    0,     /* 0xda undefined */
    0x1a,  /* VK_OEM_4 */
    0x2b,  /* VK_OEM_5 */
    0x1b,  /* VK_OEM_6 */
    0x28,  /* VK_OEM_7 */
    0,     /* VK_OEM_8 */
    0,     /* 0xe0 undefined */
    0,     /* VK_OEM_AX */
    0x56,  /* VK_OEM_102 */
    0,     /* VK_ICO_HELP */
    0,     /* VK_ICO_00 */
    0,     /* VK_PROCESSKEY */
    0,     /* VK_ICO_CLEAR */
    0,     /* VK_PACKET */
    0,     /* 0xe8 undefined */
    0x71,  /* VK_OEM_RESET */
    0,     /* VK_OEM_JUMP */
    0,     /* VK_OEM_PA1 */
    0,     /* VK_OEM_PA2 */
    0,     /* VK_OEM_PA3 */
    0,     /* VK_OEM_WSCTRL */
    0,     /* VK_OEM_CUSEL */
    0,     /* VK_OEM_ATTN */
    0,     /* VK_OEM_FINISH */
    0,     /* VK_OEM_COPY */
    0,     /* VK_OEM_AUTO */
    0,     /* VK_OEM_ENLW */
    0,     /* VK_OEM_BACKTAB */
    0,     /* VK_ATTN */
    0,     /* VK_CRSEL */
    0,     /* VK_EXSEL */
    0,     /* VK_EREOF */
    0,     /* VK_PLAY */
    0,     /* VK_ZOOM */
    0,     /* VK_NONAME */
    0,     /* VK_PA1 */
    0x59,  /* VK_OEM_CLEAR */
    0,     /* 0xff undefined */
};

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
    { VK_DELETE | 0x100,        "Delete" },
    { VK_DIVIDE | 0x100,        "Num /" },
    { VK_DOWN | 0x100,          "Down" },
    { VK_END | 0x100,           "End" },
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
    { VK_HELP | 0x100,          "Help" },
    { VK_HOME | 0x100,          "Home" },
    { VK_INSERT | 0x100,        "Insert" },
    { VK_LCONTROL,              "Ctrl" },
    { VK_LEFT | 0x100,          "Left" },
    { VK_LMENU,                 "Alt" },
    { VK_LSHIFT,                "Shift" },
    { VK_LWIN | 0x100,          "Win" },
    { VK_MENU,                  "Alt" },
    { VK_MULTIPLY,              "Num *" },
    { VK_NEXT | 0x100,          "Page Down" },
    { VK_NUMLOCK | 0x100,       "Num Lock" },
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
    { VK_OEM_NEC_EQUAL | 0x100, "Num =" },
    { VK_PRIOR | 0x100,         "Page Up" },
    { VK_RCONTROL | 0x100,      "Right Ctrl" },
    { VK_RETURN,                "Return" },
    { VK_RETURN | 0x100,        "Num Enter" },
    { VK_RIGHT | 0x100,         "Right" },
    { VK_RMENU | 0x100,         "Right Alt" },
    { VK_RSHIFT,                "Right Shift" },
    { VK_RWIN | 0x100,          "Right Win" },
    { VK_SEPARATOR,             "Num ," },
    { VK_SHIFT,                 "Shift" },
    { VK_SPACE,                 "Space" },
    { VK_SUBTRACT,              "Num -" },
    { VK_TAB,                   "Tab" },
    { VK_UP | 0x100,            "Up" },
    { VK_VOLUME_DOWN | 0x100,   "Volume Down" },
    { VK_VOLUME_MUTE | 0x100,   "Mute" },
    { VK_VOLUME_UP | 0x100,     "Volume Up" },
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

static const SHORT char_vkey_map[] =
{
    0x332, 0x241, 0x242, 0x003, 0x244, 0x245, 0x246, 0x247, 0x008, 0x009,
    0x20d, 0x24b, 0x24c, 0x00d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x255, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x01b, 0x2dc, 0x2dd,
    0x336, 0x3bd, 0x020, 0x131, 0x1de, 0x133, 0x134, 0x135, 0x137, 0x0de,
    0x139, 0x130, 0x138, 0x1bb, 0x0bc, 0x0bd, 0x0be, 0x0bf, 0x030, 0x031,
    0x032, 0x033, 0x034, 0x035, 0x036, 0x037, 0x038, 0x039, 0x1ba, 0x0ba,
    0x1bc, 0x0bb, 0x1be, 0x1bf, 0x132, 0x141, 0x142, 0x143, 0x144, 0x145,
    0x146, 0x147, 0x148, 0x149, 0x14a, 0x14b, 0x14c, 0x14d, 0x14e, 0x14f,
    0x150, 0x151, 0x152, 0x153, 0x154, 0x155, 0x156, 0x157, 0x158, 0x159,
    0x15a, 0x0db, 0x0dc, 0x0dd, 0x136, 0x1bd, 0x0c0, 0x041, 0x042, 0x043,
    0x044, 0x045, 0x046, 0x047, 0x048, 0x049, 0x04a, 0x04b, 0x04c, 0x04d,
    0x04e, 0x04f, 0x050, 0x051, 0x052, 0x053, 0x054, 0x055, 0x056, 0x057,
    0x058, 0x059, 0x05a, 0x1db, 0x1dc, 0x1dd, 0x1c0, 0x208
};

static UINT scancode_to_vkey(UINT scan)
{
    UINT j;

    for (j = 0; j < ARRAY_SIZE(vkey_to_scancode); j++)
        if (vkey_to_scancode[j] == scan)
            return j;
    return 0;
}

static const char* vkey_to_name(UINT vkey)
{
    UINT j;

    for (j = 0; j < ARRAY_SIZE(vkey_names); j++)
        if (vkey_names[j].vkey == vkey)
            return vkey_names[j].name;
    return NULL;
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

    __wine_send_input(hwnd, &input);
}

/***********************************************************************
 *           wayland_keyboard_emit
 *
 * Emits a keyboard event to a window. The key and state arguments
 * are interpreted according to the wl_keyboard documentation.
 */
void wayland_keyboard_emit(uint32_t key, uint32_t state, HWND hwnd)
{
    UINT vkey = keycode_to_vkey[key];

    send_keyboard_input(hwnd, vkey, vkey_to_scancode[vkey],
                        state == WL_KEYBOARD_KEY_STATE_PRESSED ? 0 : KEYEVENTF_KEYUP);
}

/***********************************************************************
 *           WAYLAND_ToUnicodeEx
 */
INT CDECL WAYLAND_ToUnicodeEx(UINT virt, UINT scan, const BYTE *state,
                               LPWSTR buf, int size, UINT flags, HKL hkl)
{
    WCHAR buffer[2];
    BOOL shift = state[VK_SHIFT] & 0x80;
    BOOL ctrl = state[VK_CONTROL] & 0x80;
    BOOL numlock = state[VK_NUMLOCK] & 0x01;

    buffer[0] = buffer[1] = 0;

    if (scan & 0x8000) return 0;  /* key up */

    /* FIXME: hardcoded layout */

    if (!ctrl)
    {
        switch (virt)
        {
        case VK_BACK:       buffer[0] = '\b'; break;
        case VK_OEM_1:      buffer[0] = shift ? ':' : ';'; break;
        case VK_OEM_2:      buffer[0] = shift ? '?' : '/'; break;
        case VK_OEM_3:      buffer[0] = shift ? '~' : '`'; break;
        case VK_OEM_4:      buffer[0] = shift ? '{' : '['; break;
        case VK_OEM_5:      buffer[0] = shift ? '|' : '\\'; break;
        case VK_OEM_6:      buffer[0] = shift ? '}' : ']'; break;
        case VK_OEM_7:      buffer[0] = shift ? '"' : '\''; break;
        case VK_OEM_COMMA:  buffer[0] = shift ? '<' : ','; break;
        case VK_OEM_MINUS:  buffer[0] = shift ? '_' : '-'; break;
        case VK_OEM_PERIOD: buffer[0] = shift ? '>' : '.'; break;
        case VK_OEM_PLUS:   buffer[0] = shift ? '+' : '='; break;
        case VK_RETURN:     buffer[0] = '\r'; break;
        case VK_SPACE:      buffer[0] = ' '; break;
        case VK_TAB:        buffer[0] = '\t'; break;
        case VK_MULTIPLY:   buffer[0] = '*'; break;
        case VK_ADD:        buffer[0] = '+'; break;
        case VK_SUBTRACT:   buffer[0] = '-'; break;
        case VK_DIVIDE:     buffer[0] = '/'; break;
        default:
            if (virt >= '0' && virt <= '9')
            {
                buffer[0] = shift ? ")!@#$%^&*("[virt - '0'] : virt;
                break;
            }
            if (virt >= 'A' && virt <= 'Z')
            {
                buffer[0] =  shift || (state[VK_CAPITAL] & 0x01) ? virt : virt + 'a' - 'A';
                break;
            }
            if (virt >= VK_NUMPAD0 && virt <= VK_NUMPAD9 && numlock && !shift)
            {
                buffer[0] = '0' + virt - VK_NUMPAD0;
                break;
            }
            if (virt == VK_DECIMAL && numlock && !shift)
            {
                buffer[0] = '.';
                break;
            }
            break;
        }
    }
    else /* Control codes */
    {
        if (virt >= 'A' && virt <= 'Z')
            buffer[0] = virt - 'A' + 1;
        else
        {
            switch (virt)
            {
            case VK_OEM_4:
                buffer[0] = 0x1b;
                break;
            case VK_OEM_5:
                buffer[0] = 0x1c;
                break;
            case VK_OEM_6:
                buffer[0] = 0x1d;
                break;
            case VK_SUBTRACT:
                buffer[0] = 0x1e;
                break;
            }
        }
    }

    lstrcpynW(buf, buffer, size);
    TRACE("returning %d / %s\n", strlenW(buffer), debugstr_wn(buf, strlenW(buffer)));
    return strlenW(buffer);
}


/***********************************************************************
 *           GetKeyNameText
 */
INT CDECL WAYLAND_GetKeyNameText(LONG lparam, LPWSTR buffer, INT size)
{
    int scancode, vkey, len;
    const char *name;
    char key[2];

    scancode = (lparam >> 16) & 0x1FF;
    vkey = scancode_to_vkey(scancode);

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

    if (scancode & 0x100) vkey |= 0x100;

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

    TRACE("lparam 0x%08x -> %s\n", lparam, debugstr_w(buffer));
    return len;
}


/***********************************************************************
 *           WAYLAND_MapVirtualKeyEx
 */
UINT CDECL WAYLAND_MapVirtualKeyEx(UINT code, UINT maptype, HKL hkl)
{
    UINT ret = 0;
    const char *s;

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
        if (code < ARRAY_SIZE(vkey_to_scancode)) ret = vkey_to_scancode[code];

        /* set scan code prefix */
        if (maptype == MAPVK_VK_TO_VSC_EX &&
            (code == VK_RCONTROL || code == VK_RMENU))
            ret |= 0xe000;
        break;
    case MAPVK_VSC_TO_VK:
    case MAPVK_VSC_TO_VK_EX:
        /* scancode to vkey */
        ret = scancode_to_vkey(code);
        if (maptype == MAPVK_VSC_TO_VK)
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
        break;
    case MAPVK_VK_TO_CHAR:
        s = vkey_to_name(code);
        if (s && (strlen(s) == 1))
            ret = s[0];
        else
            ret = 0;
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
    static int once;

    langid = PRIMARYLANGID(LANGIDFROMLCID(layout));
    if (langid == LANG_CHINESE || langid == LANG_JAPANESE || langid == LANG_KOREAN)
        layout = MAKELONG(layout, 0xe001); /* IME */
    else
        layout |= layout << 16;

    if (!once++) FIXME("returning %lx\n", layout);
    return (HKL)layout;
}


/***********************************************************************
 *           WAYLAND_VkKeyScanEx
 */
SHORT CDECL WAYLAND_VkKeyScanEx(WCHAR ch, HKL hkl)
{
    SHORT ret = -1;
    if (ch < ARRAY_SIZE(char_vkey_map)) ret = char_vkey_map[ch];
    TRACE_(key)("ch %04x hkl %p -> %04x\n", ch, hkl, ret);
    return ret;
}
