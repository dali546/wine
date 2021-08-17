# GDI driver

@ cdecl wine_get_gdi_driver(long) WAYLAND_get_gdi_driver

# USER driver

@ cdecl GetKeyNameText(long ptr long) WAYLAND_GetKeyNameText
@ cdecl GetKeyboardLayout(long) WAYLAND_GetKeyboardLayout
@ cdecl MapVirtualKeyEx(long long long) WAYLAND_MapVirtualKeyEx
@ cdecl ToUnicodeEx(long long ptr ptr long long long) WAYLAND_ToUnicodeEx
@ cdecl VkKeyScanEx(long long) WAYLAND_VkKeyScanEx
@ cdecl SetCursor(long) WAYLAND_SetCursor
@ cdecl ClipCursor(ptr) WAYLAND_ClipCursor
@ cdecl ChangeDisplaySettingsEx(ptr ptr long long long) WAYLAND_ChangeDisplaySettingsEx
@ cdecl EnumDisplaySettingsEx(ptr long ptr long) WAYLAND_EnumDisplaySettingsEx
@ cdecl CreateWindow(long) WAYLAND_CreateWindow
@ cdecl DestroyWindow(long) WAYLAND_DestroyWindow
@ cdecl MsgWaitForMultipleObjectsEx(long ptr long long long) WAYLAND_MsgWaitForMultipleObjectsEx
@ cdecl SetCapture(long long) WAYLAND_SetCapture
@ cdecl SetLayeredWindowAttributes(long long long long) WAYLAND_SetLayeredWindowAttributes
@ cdecl SetParent(long long long) WAYLAND_SetParent
@ cdecl SetWindowRgn(long long long) WAYLAND_SetWindowRgn
@ cdecl SetWindowStyle(ptr long ptr) WAYLAND_SetWindowStyle
@ cdecl ShowWindow(long long ptr long) WAYLAND_ShowWindow
@ cdecl SysCommand(long long long) WAYLAND_SysCommand
@ cdecl UpdateLayeredWindow(long ptr ptr) WAYLAND_UpdateLayeredWindow
@ cdecl WindowMessage(long long long long) WAYLAND_WindowMessage
@ cdecl WindowPosChanging(long long long ptr ptr ptr ptr) WAYLAND_WindowPosChanging
@ cdecl WindowPosChanged(long long long ptr ptr ptr ptr ptr) WAYLAND_WindowPosChanged
@ cdecl ThreadDetach() WAYLAND_ThreadDetach
