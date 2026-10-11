#include "backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *sys_exec(const char *cmd) {
    if (!cmd) return NULL;
    /* LC_ALL=C: salida estable en inglés y punto decimal (el sistema
     * puede estar en es_PE.UTF-8: "sí" en vez de "yes", "0,80"...). */
    char *full = g_strdup_printf("LC_ALL=C %s 2>/dev/null", cmd);
    FILE *fp = popen(full, "r");
    g_free(full);
    if (!fp) return NULL;
    GString *s = g_string_new(NULL);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        g_string_append_len(s, buf, n);
    pclose(fp);
    /* recortar espacios/saltos finales */
    while (s->len > 0 && g_ascii_isspace(s->str[s->len - 1]))
        g_string_truncate(s, s->len - 1);
    return g_string_free(s, FALSE);
}

void sys_spawn(const char *cmd) {
    if (!cmd) return;
    char *full = g_strdup_printf("sh -c '%s' &", cmd);
    /* igual que exec pero desacoplado; usamos GSpawn para no bloquear */
    g_spawn_command_line_async(full, NULL);
    g_free(full);
}

int sys_has(const char *bin) {
    char *p = g_find_program_in_path(bin);
    int ok = (p != NULL);
    g_free(p);
    return ok;
}

/* ============================ Wi-Fi ============================ */

int sys_wifi_supported(void) { return sys_has("nmcli"); }

int sys_wifi_enabled(void) {
    char *o = sys_exec("nmcli -t -f WIFI g");
    int on = (o && g_str_has_prefix(o, "enabled"));
    g_free(o);
    return on;
}

/* Una sola invocación para radio+SSID+señal (el poll la llama cada 2 s). */
void sys_wifi_all(int *enabled, char *ssid_out, size_t ssid_len, int *signal) {
    if (enabled) *enabled = sys_wifi_enabled();
    if (ssid_out && ssid_len) ssid_out[0] = '\0';
    if (signal) *signal = -1;
    char *o = sys_exec("nmcli -t -f ACTIVE,SSID,SIGNAL,IN-USE dev wifi");
    if (!o || !*o) { g_free(o); return; }
    char **lines = g_strsplit(o, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *ln = lines[i];
        if (!*ln) continue;
        /* por el final: ...:SIGNAL:INUSE (INUSE es '*' o vacío) */
        char *last = strrchr(ln, ':');
        if (!last) continue;
        int inuse = (last[1] == '*');
        *last = '\0';
        char *prev = strrchr(ln, ':');
        if (!prev) continue;
        int sig = atoi(prev + 1);
        *prev = '\0';
        /* por el principio: ACTIVE:SSID */
        char * mid = strchr(ln, ':');
        if (!mid) continue;
        *mid = '\0';
        int active = !strcmp(ln, "yes");
        if (active || inuse) {
            if (ssid_out && ssid_len) {
                g_strlcpy(ssid_out, mid + 1, ssid_len);
                /* des-escapar \: y \, de nmcli terse */
                char *rd = ssid_out, *wr = ssid_out;
                while (*rd) {
                    if (*rd == '\\' && (rd[1] == ':' || rd[1] == ',' || rd[1] == '\\'))
                        rd++;
                    *wr++ = *rd++;
                }
                *wr = '\0';
            }
            if (signal) *signal = sig;
            if (active) break;
        }
    }
    g_strfreev(lines);
    g_free(o);
}

int sys_wifi_set(int on) {
    char *o = sys_exec(on ? "nmcli radio wifi on" : "nmcli radio wifi off");
    g_free(o);
    return 0;
}

char *sys_wifi_ssid(void) {
    char *o = sys_exec("nmcli -t -f ACTIVE,SSID dev wifi");
    char *ssid = g_strdup("");
    if (o) {
        char **lines = g_strsplit(o, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], "yes:")) {
                g_free(ssid);
                ssid = g_strdup(lines[i] + 4);
                break;
            }
        }
        g_strfreev(lines);
        g_free(o);
    }
    return ssid;
}

int sys_wifi_signal(void) {
    char *o = sys_exec("nmcli -t -f IN-USE,SIGNAL dev wifi");
    int sig = -1;
    if (o) {
        char **lines = g_strsplit(o, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], "*:"))
                sig = atoi(lines[i] + 2);
        }
        g_strfreev(lines);
        g_free(o);
    }
    return sig;
}

char *sys_wifi_scan_raw(void) {
    /* Sin --rescan para no bloquear la UI; el botón Actualizar sí reescanea */
    return sys_exec("nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list --rescan no");
}

int sys_wifi_connect(const char *ssid, const char *password) {
    if (!ssid || !*ssid) return -1;
    char *qssid = g_shell_quote(ssid);
    char *cmd;
    if (password && *password) {
        char *qpw = g_shell_quote(password);
        cmd = g_strdup_printf("nmcli dev wifi connect %s password %s", qssid, qpw);
        g_free(qpw);
    } else {
        cmd = g_strdup_printf("nmcli dev wifi connect %s", qssid);
    }
    g_free(qssid);
    char *o = sys_exec(cmd);
    g_free(cmd);
    g_free(o);
    return 0;
}

/* ============================ Bluetooth ============================ */

int sys_bt_hw(void) {
    char *o = sys_exec("rfkill list bluetooth");
    int hw = (o && *o);
    g_free(o);
    return hw;
}

int sys_bt_powered(void) {
    char *o = sys_exec("rfkill list bluetooth");
    int on = 0;
    if (o) {
        /* encendido = ningún "Soft blocked: yes" */
        on = (strstr(o, "Soft blocked: yes") == NULL);
        g_free(o);
    }
    return on;
}

int sys_bt_set(int on) {
    char *o = sys_exec(on ? "rfkill unblock bluetooth" : "rfkill block bluetooth");
    g_free(o);
    return 0;
}

int sys_bt_ctl(void) { return sys_has("bluetoothctl"); }

char *sys_bt_devices_raw(void) {
    if (!sys_bt_ctl()) return g_strdup("");
    return sys_exec("bluetoothctl devices Paired");
}

/* ============================ Sonido ============================ */

int sys_volume_get(int *muted) {
    if (muted) *muted = 0;
    char *o = sys_exec("wpctl get-volume @DEFAULT_AUDIO_SINK@");
    int vol = -1;
    if (o && *o) {
        /* Formato: "Volume: 0.55" o "Volume: 0.55 [MUTED]".
         * g_ascii_strtod: independiente del locale del proceso
         * (GTK fija es_PE y sscanf leería "0.80" como 0). */
        const char *p = strstr(o, "Volume:");
        double v = g_ascii_strtod(p ? p + 7 : o, NULL);
        vol = (int)(v * 100.0 + 0.5);
        if (muted) *muted = (strstr(o, "MUTED") != NULL);
        g_free(o);
    }
    return vol;
}

int sys_volume_set(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char *cmd = g_strdup_printf("wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%", pct);
    char *o = sys_exec(cmd);
    g_free(cmd);
    g_free(o);
    return 0;
}

int sys_mute_set(int mute) {
    char *o = sys_exec(mute ? "wpctl set-mute @DEFAULT_AUDIO_SINK@ 1"
                            : "wpctl set-mute @DEFAULT_AUDIO_SINK@ 0");
    g_free(o);
    return 0;
}

int sys_mute_toggle(void) {
    char *o = sys_exec("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
    g_free(o);
    return 0;
}

char *sys_sinks_raw(void) {
    if (sys_has("pactl"))
        return sys_exec("pactl list short sinks");
    return g_strdup("");
}

int sys_sink_default(const char *id_or_name) {
    if (!id_or_name) return -1;
    char *q = g_shell_quote(id_or_name);
    char *cmd = g_strdup_printf("pactl set-default-sink %s", q);
    g_free(q);
    char *o = sys_exec(cmd);
    g_free(cmd);
    g_free(o);
    return 0;
}

/* ============================ Brillo ============================ */

int sys_brightness_get(void) {
    if (sys_has("brightnessctl")) {
        char *o = sys_exec("brightnessctl -m");
        int pct = -1;
        if (o && *o) {
            /* machine-readable: disp,clase,actual,PORC%,máx -> buscar el % */
            char **cols = g_strsplit(o, ",", -1);
            for (int i = 0; cols[i]; i++) {
                if (strchr(cols[i], '%')) { pct = atoi(cols[i]); break; }
            }
            g_strfreev(cols);
            g_free(o);
        }
        return pct;
    }
    if (sys_has("light")) {
        char *o = sys_exec("light -G");
        int pct = (o && *o) ? (int)(g_ascii_strtod(o, NULL) + 0.5) : -1;
        g_free(o);
        return pct;
    }
    return -1;
}

int sys_brightness_set(int pct) {
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    char *cmd;
    if (sys_has("brightnessctl"))
        cmd = g_strdup_printf("brightnessctl set %d%% -q", pct);
    else if (sys_has("light"))
        cmd = g_strdup_printf("light -S %d", pct);
    else
        return -1;
    char *o = sys_exec(cmd);
    g_free(cmd);
    g_free(o);
    return 0;
}

/* ============================ Now Playing ============================ */

static char *first_player(void) {
    char *o = sys_exec("playerctl -l");
    if (!o || !*o) { g_free(o); return NULL; }
    char **lines = g_strsplit(o, "\n", -1);
    char *p = g_strdup(lines[0]);
    g_strfreev(lines);
    g_free(o);
    return p;
}

char *sys_player_status(void) {
    char *p = first_player();
    if (!p) return g_strdup("");
    char *q = g_shell_quote(p);
    char *cmd = g_strdup_printf("playerctl -p %s status", q);
    g_free(q); g_free(p);
    char *o = sys_exec(cmd);
    g_free(cmd);
    if (!o) return g_strdup("");
    return o;
}

char *sys_player_meta(void) {
    char *p = first_player();
    if (!p) return g_strdup("|");
    char *q = g_shell_quote(p);
    char *cmd = g_strdup_printf(
        "playerctl -p %s metadata --format '{{title}}|{{artist}}'", q);
    g_free(q); g_free(p);
    char *o = sys_exec(cmd);
    g_free(cmd);
    if (!o) return g_strdup("|");
    return o;
}

char *sys_player_name(void) {
    char *p = first_player();
    if (!p) return g_strdup("");
    /* "firefox.instance_1_401" -> "firefox" */
    char *dot = strchr(p, '.');
    if (dot) *dot = '\0';
    return p;
}

void sys_player_cmd(const char *action) {
    if (!action) return;
    char *cmd = g_strdup_printf("playerctl %s", action);
    sys_spawn(cmd);
    g_free(cmd);
}

/* ============================ Focus / DND ============================ */

int sys_dnd_supported(void) {
    return sys_has("dunstctl") || sys_has("swaync-client") || sys_has("makoctl");
}

int sys_dnd_get(void) {
    if (sys_has("dunstctl")) {
        char *o = sys_exec("dunstctl is-paused");
        int v = (o && g_str_has_prefix(o, "true"));
        g_free(o);
        return v;
    }
    if (sys_has("swaync-client")) {
        char *o = sys_exec("swaync-client -D");
        int v = (o && strstr(o, "true") != NULL);
        g_free(o);
        return v;
    }
    return 0;
}

int sys_dnd_set(int on) {
    char *o = NULL;
    if (sys_has("dunstctl"))
        o = sys_exec(on ? "dunstctl set-paused true" : "dunstctl set-paused false");
    else if (sys_has("swaync-client"))
        o = sys_exec(on ? "swaync-client -D" : "swaync-client -df");
    g_free(o);
    return 0;
}

/* ============================ Apariencia ============================ */

int sys_dark_get(void) {
    char *o = sys_exec("gsettings get org.gnome.desktop.interface gtk-theme");
    int dark = (o && (strstr(o, "dark") || strstr(o, "Dark")));
    g_free(o);
    return dark;
}

int sys_dark_set(int on) {
    char *o;
    if (on) {
        o = sys_exec("gsettings set org.gnome.desktop.interface gtk-theme 'Adwaita-dark'");
        g_free(o);
        o = sys_exec("gsettings set org.gnome.desktop.interface color-scheme 'prefer-dark'");
    } else {
        o = sys_exec("gsettings set org.gnome.desktop.interface gtk-theme 'Adwaita'");
        g_free(o);
        o = sys_exec("gsettings set org.gnome.desktop.interface color-scheme 'default'");
    }
    g_free(o);
    return 0;
}

int sys_nightlight_supported(void) {
    return sys_has("gammastep") || sys_has("wlsunset");
}

int sys_nightlight_get(void) {
    /* heurística: proceso en ejecución */
    char *o = sys_exec("pgrep -x gammastep || pgrep -x wlsunset");
    int v = (o && *o);
    g_free(o);
    return v;
}

int sys_nightlight_set(int on) {
    if (on) {
        if (sys_has("gammastep")) sys_spawn("gammastep -O 4500K");
        else if (sys_has("wlsunset")) sys_spawn("wlsunset -t 4500 -T 6500");
    } else {
        char *o = sys_exec("pkill -x gammastep; pkill -x wlsunset");
        g_free(o);
    }
    return 0;
}

/* ============================ Hyprland ============================ */

char *sys_monitors_raw(void) {
    char *o = sys_exec("hyprctl monitors");
    GString *out = g_string_new(NULL);
    if (o) {
        char **lines = g_strsplit(o, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], "Monitor ")) {
                char *name = lines[i] + 8;
                char *sp = strchr(name, ' ');
                if (sp) *sp = '\0';
                if (*name) {
                    if (out->len) g_string_append_c(out, '\n');
                    g_string_append(out, name);
                }
            }
        }
        g_strfreev(lines);
        g_free(o);
    }
    return g_string_free(out, FALSE);
}

char *sys_focused_monitor(void) {
    /* awk: recuerda el último "Monitor X" y lo imprime al ver focused: yes */
    char *o = sys_exec("hyprctl monitors | awk '/^Monitor /{m=$2} /focused: yes/{print m}'");
    char *name = g_strdup((o && *o) ? o : "");
    g_free(o);
    return name;
}

int sys_overview_toggle(void) {
    /* overview integrado de Hyprland (>=0.45). Si no existe, hyprctl falla. */
    int rc = system("hyprctl dispatch overview:toggle >/dev/null 2>&1");
    return rc;
}

int sys_mirror(const char *src, const char *dst) {
    if (!src || !dst || !*src || !*dst) return -1;
    char *q = g_shell_quote(src);
    char *qd = g_shell_quote(dst);
    /* mirror <src> en <dst>: el monitor dst refleja a src */
    char *cmd = g_strdup_printf(
        "hyprctl keyword monitor %s,preferred,auto,auto,mirror,%s", q, qd);
    g_free(q); g_free(qd);
    char *o = sys_exec(cmd);
    g_free(cmd);
    g_free(o);
    return 0;
}

/* ============================ Acciones ============================ */

void sys_open_calculator(void) {
    const char *cands[] = {
        "gnome-calculator", "qalculate-gtk", "kcalc", "galculator", "gnome-clocks", NULL
    };
    for (int i = 0; cands[i]; i++)
        if (sys_has(cands[i])) { sys_spawn(cands[i]); return; }
}

void sys_screenshot(void) {
    if (sys_has("hyprshot"))
        sys_spawn("hyprshot -m region");
}

void sys_lock(void) {
    if (sys_has("hyprlock"))
        sys_spawn("hyprlock");
    else if (sys_has("swaylock"))
        sys_spawn("swaylock");
}
