/* macOS Tahoe-style Control Center — backend de sistema (sin GTK).
 * Todo se resuelve con herramientas CLI presentes en Hyprland/Arch:
 * nmcli, rfkill, bluetoothctl?, wpctl/pactl, brightnessctl/light,
 * playerctl, dunstctl, gsettings, hyprctl, hyprshot.
 * Si una herramienta falta, la función devuelve un valor seguro y la UI
 * muestra "No disponible" en lugar de romperse. */

#ifndef CC_BACKEND_H
#define CC_BACKEND_H

#include <glib.h>

/* Ejecuta cmd con /bin/sh, captura stdout, recorta espacios finales.
 * Devuelve cadena nueva (g_free) o NULL si falla. Incluye stderr silenciado. */
char *sys_exec(const char *cmd);

/* Lanza un comando sin esperar (para abrir apps, capturas, etc). */
void sys_spawn(const char *cmd);

/* 1 si el binario existe en PATH. */
int sys_has(const char *bin);

/* --- Wi-Fi (NetworkManager) --- */
int   sys_wifi_supported(void);          /* nmcli existe */
int   sys_wifi_enabled(void);            /* radio wifi on */
int   sys_wifi_set(int on);
void  sys_wifi_all(int *enabled, char *ssid_out, size_t ssid_len, int *signal);
char *sys_wifi_ssid(void);               /* SSID activo o "" (g_free) */
int   sys_wifi_signal(void);             /* 0-100, -1 si se desconoce */
char *sys_wifi_scan_raw(void);           /* salida nmcli IN-USE,SSID,SIGNAL,SECURITY */
int   sys_wifi_connect(const char *ssid, const char *password);

/* --- Bluetooth --- */
int   sys_bt_hw(void);                   /* hay radio bt según rfkill */
int   sys_bt_powered(void);              /* no bloqueado */
int   sys_bt_set(int on);
int   sys_bt_ctl(void);                  /* bluetoothctl disponible */
char *sys_bt_devices_raw(void);          /* "devices" de bluetoothctl */

/* --- Sonido (PipeWire) --- */
int   sys_volume_get(int *muted);        /* 0-100, -1 si error */
int   sys_volume_set(int pct);
int   sys_mute_set(int mute);
int   sys_mute_toggle(void);
char *sys_sinks_raw(void);               /* pactl list short sinks */
int   sys_sink_default(const char *id_or_name);

/* --- Brillo --- */
int sys_brightness_get(void);            /* 0-100, -1 si error */
int sys_brightness_set(int pct);

/* --- Now Playing (MPRIS) --- */
char *sys_player_status(void);           /* Playing|Paused|Stopped|"" */
char *sys_player_meta(void);             /* "titulo|artista" (puede estar vacío) */
char *sys_player_name(void);             /* "firefox", "spotify"... */
void  sys_player_cmd(const char *action);/* play-pause|previous|next */

/* --- Focus / No molestar --- */
int sys_dnd_supported(void);             /* dunstctl o swaync o mako */
int sys_dnd_get(void);
int sys_dnd_set(int on);

/* --- Apariencia --- */
int sys_dark_get(void);
int sys_dark_set(int on);
int sys_nightlight_supported(void);
int sys_nightlight_get(void);
int sys_nightlight_set(int on);

/* --- Hyprland --- */
char *sys_monitors_raw(void);            /* nombres de monitores, uno por línea */
char *sys_focused_monitor(void);
int   sys_overview_toggle(void);         /* 0 ok, !=0 no soportado */
int   sys_mirror(const char *src, const char *dst);

/* --- Acciones --- */
void sys_open_calculator(void);          /* primera calculadora encontrada */
void sys_screenshot(void);               /* hyprshot región */
void sys_lock(void);                     /* hyprlock */

#endif
