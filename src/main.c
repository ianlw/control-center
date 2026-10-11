/* Control Center estilo macOS Tahoe para Hyprland + HyprGlass.
 *
 * CADA BOTÓN ES SU PROPIA SUPERFICIE Wayland. HyprGlass calcula bordes,
 * refracción, fresnel y aberración desde el quad de cada superficie
 * (getRoundedBoxSDF(uv) en Shaders.hpp): con ventanas ajustadas al
 * contenido, cada botón recibe su propio entorno de cristal con su
 * propio centro y bisel proporcional, como ventanas independientes.
 * (Ventanas fullscreen comparten un único campo de bordes: por eso antes
 * solo se veía cristal arriba.)
 *
 * Superficies (namespace "control-center", salvo el catcher):
 * - 1 catcher fullscreen transparente (cierra al clicar fuera; SIN cristal)
 * - 1 banner flotante ("X recently")
 * - 15 ventanas-botón posicionadas por métricas exactas (ver GEOMETRÍA)
 * - 7 ventanas-detalle ajustadas al contenido
 *
 * Compilar: make
 * Probar sin GUI: ./control-center --check
 */

#include <gtk/gtk.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <string.h>

#include "backend.h"

#define APP_ID "control-center"
#define CATCHER_ID "control-center-catcher" /* sin hg.layer: sin cristal */
#define AIRDROP_OFF 0
#define AIRDROP_CONTACTS 1
#define AIRDROP_EVERYONE 2

/* ------------------------------------------------------------------
 * GEOMETRÍA (píxeles lógicos; las maneja el compositor con la escala).
 * Medida con texto de ancho máximo (peor caso) para que la geometría
 * sea estable la cambie lo que cambie el texto (elipsis acota).
 * Si cambias CSS/fuente, re-deriva: x,y,w,h de cada tarjeta relativos
 * al marco de 396px; MT = 66+y ; MR = 24+(382-x-w).
 * ------------------------------------------------------------------ */
#define PANEL_MT 52
#define PANEL_MR 10
#define GEOM_DX 66   /* 52 margen + 14 padding */
#define GEOM_RX 24   /* 10 margen + 14 padding */
#define GEOM_W 382   /* ancho útil del grid */

typedef struct { int x, y, w, h; } Geom;

static const Geom GEOMS[15] = {
    [0]  = { 14, 14, 169, 70  },   /* wifi */
    [1]  = { 193, 14, 189, 150 },  /* now playing (alto 2) */
    [2]  = { 14, 94, 169, 70  },   /* bluetooth */
    [3]  = { 14, 174, 169, 70 },   /* airdrop */
    [4]  = { 193, 174, 90, 70 },   /* stage */
    [5]  = { 293, 174, 89, 70 },   /* mirror */
    [6]  = { 14, 254, 79, 70  },   /* dnd */
    [7]  = { 103, 254, 80, 70 },   /* mute */
    [8]  = { 193, 254, 189, 70 },  /* focus */
    [9]  = { 14, 334, 368, 95 },   /* display */
    [10] = { 14, 439, 368, 95 },   /* sound */
    [11] = { 14, 544, 79, 60  },   /* dark */
    [12] = { 103, 544, 80, 60 },   /* calc */
    [13] = { 193, 544, 90, 60 },   /* shot */
    [14] = { 293, 544, 89, 60 },   /* lock */
};

enum {
    S_WIFI, S_NP, S_BT, S_AD, S_STAGE, S_MIRROR, S_DND, S_MUTE,
    S_FOCUS, S_DISPLAY, S_SOUND, S_DARK, S_CALC, S_SHOT, S_LOCK,
    NSLOT
};

enum { D_WIFI, D_BT, D_SOUND, D_DISPLAY, D_FOCUS, D_MIRROR, D_AD, NDETAIL };

typedef struct {
    GtkWidget *catcher_win;
    GtkWidget *banner_win, *banner_label, *banner_pill;
    GtkWidget *btn_wins[NSLOT];
    GtkWidget *detail_wins[NDETAIL];
    GtkWidget *detail_frames[NDETAIL];
    GtkWidget *mirror_list;

    /* tarjetas principales (una instancia por botón) */
    GtkWidget *wifi_icon, *wifi_title, *wifi_sub, *wifi_pill;
    GtkWidget *bt_icon, *bt_title, *bt_sub, *bt_pill;
    GtkWidget *ad_icon, *ad_title, *ad_sub, *ad_pill;
    GtkWidget *np_title, *np_artist, *np_play_icon, *np_card;
    GtkWidget *stage_btn, *mirror_btn;
    GtkWidget *dnd_btn, *dnd_icon, *mute_btn, *mute_icon;
    GtkWidget *focus_pill, *focus_sub, *focus_icon;
    GtkWidget *display_scale, *sound_scale, *sound_mute_icon;
    GtkWidget *dark_btn, *dark_icon;
    GtkWidget *calc_btn, *shot_btn, *lock_btn;

    /* widgets de detalle (singletons) */
    GtkWidget *wifi_switch, *wifi_status, *wifi_list, *wifi_scan_btn;
    GtkWidget *bt_switch, *bt_hint, *bt_list;
    GtkWidget *snd_scale, *snd_mute_switch, *snd_list;
    GtkWidget *dsp_scale, *dsp_dark_switch, *dsp_night_row, *dsp_night_switch;
    GtkWidget *focus_switch;

    GtkWidget *all_wins[2 + NSLOT + NDETAIL];
    int n_wins;
    int awaiting_maps; /* superficies-botón pendientes de mapear */
    int reveal_quick;  /* 1 = revelado sin cascada (volver de detalle) */

    /* animación de entrada: apiladas -> posiciones con rebote */
    guint anim_timer;
    gint64 anim_t0;
    int anim_dur;
    int anim_stagger;

    int airdrop_mode;
    int slider_busy;
    int modal_open;
    int ready;
    guint focus_timer;
    guint banner_timer;
} App;

/* ---------------- utilidades ---------------- */

static const char *pick_icon(const char *primary, const char *fallback) {
    GtkIconTheme *th = gtk_icon_theme_get_default();
    if (primary && gtk_icon_theme_has_icon(th, primary)) return primary;
    if (fallback && gtk_icon_theme_has_icon(th, fallback)) return fallback;
    return "image-missing";
}

static void set_img(GtkWidget *img, const char *primary, const char *fallback) {
    gtk_image_set_from_icon_name(GTK_IMAGE(img), pick_icon(primary, fallback),
                                 GTK_ICON_SIZE_LARGE_TOOLBAR);
}

static void pill_active(GtkWidget *pill, int on) {
    GtkStyleContext *sc = gtk_widget_get_style_context(pill);
    if (on) gtk_style_context_add_class(sc, "on");
    else gtk_style_context_remove_class(sc, "on");
}

static void round_active(GtkWidget *btn, int on) {
    GtkStyleContext *sc = gtk_widget_get_style_context(btn);
    if (on) gtk_style_context_add_class(sc, "active");
    else gtk_style_context_remove_class(sc, "active");
}

static GtkWidget *sym(const char *primary, const char *fallback) {
    GtkWidget *img = gtk_image_new_from_icon_name(
        pick_icon(primary, fallback), GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    return img;
}

static void quit_all(void) { gtk_main_quit(); }

static void reg_win(App *a, GtkWidget *w) {
    if (a->n_wins < (int)(sizeof a->all_wins / sizeof a->all_wins[0]))
        a->all_wins[a->n_wins++] = w;
}

static gboolean hide_banner(gpointer d) {
    App *a = (App *)d;
    a->banner_timer = 0;
    gtk_widget_hide(a->banner_win);
    return G_SOURCE_REMOVE;
}

static void show_banner(App *a, const char *text) {
    gtk_label_set_text(GTK_LABEL(a->banner_label), text);
    gtk_widget_show(a->banner_win);
    if (a->banner_timer) g_source_remove(a->banner_timer);
    a->banner_timer = g_timeout_add_seconds(4, hide_banner, a);
}

/* switch con título + subtítulo, como macOS */
static GtkWidget *switch_row(const char *title, const char *sub,
                             GtkWidget **out_switch) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkStyleContext *sc = gtk_widget_get_style_context(box);
    gtk_style_context_add_class(sc, "detail-row");
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(box), txt, TRUE, TRUE, 0);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    GtkStyleContext *tc = gtk_widget_get_style_context(t);
    gtk_style_context_add_class(tc, "row-title");
    GtkWidget *s = gtk_label_new(sub);
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    GtkStyleContext *uc = gtk_widget_get_style_context(s);
    gtk_style_context_add_class(uc, "row-sub");
    gtk_box_pack_start(GTK_BOX(txt), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), s, FALSE, FALSE, 0);
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);
    if (out_switch) *out_switch = sw;
    return box;
}

/* ---------------- navegación ---------------- */

static void show_main(App *a);
static void start_entrance(App *a, int staggered);

static void show_main(App *a) {
    /* re-mapeo sincronizado: invisibles hasta estar las 15, luego
     * revelado conjunto + entrada rápida */
    a->reveal_quick = 1;
    a->awaiting_maps = NSLOT;
    for (int i = 0; i < NSLOT; i++) {
        gtk_widget_set_opacity(a->btn_wins[i], 0.0);
        gtk_widget_set_sensitive(a->btn_wins[i], FALSE);
        gtk_widget_show(a->btn_wins[i]);
    }
    for (int i = 0; i < NDETAIL; i++)
        gtk_widget_hide(a->detail_wins[i]);
}

/* rebote ligero (easeOutBack suave) */
static double ease_out_back(double t) {
    const double c1 = 1.2, c3 = c1 + 1.0;
    double u = t - 1.0;
    return 1.0 + c3 * u * u * u + c1 * u * u;
}

static int slot_order(int k) {
    /* cascada por filas: arriba-izquierda primero */
    int row = GEOMS[k].y / 80;
    int col = GEOMS[k].x > 150 ? 1 : 0;
    return row * 2 + col;
}

/* márgenes finales de un slot */
static void slot_margins(int k, int *mt, int *mr) {
    *mt = GEOM_DX + GEOMS[k].y;
    *mr = GEOM_RX + (GEOM_W - GEOMS[k].x - GEOMS[k].w);
}

static gboolean anim_tick(gpointer d) {
    App *a = (App *)d;
    double el = (g_get_monotonic_time() - a->anim_t0) / 1000.0;
    /* origen: fuera de pantalla, borde derecho (entran volando) */
    int smt = PANEL_MT, smr = 2000;
    int done = 1;
    for (int k = 0; k < NSLOT; k++) {
        int mt1, mr1;
        slot_margins(k, &mt1, &mr1);
        double delay = a->anim_stagger ? slot_order(k) * 18.0 : 0.0;
        double lt = (el - delay) / a->anim_dur;
        int mt, mr;
        if (lt < 0) { mt = smt; mr = smr; done = 0; }
        else if (lt >= 1) { mt = mt1; mr = mr1; }
        else {
            done = 0;
            double e = ease_out_back(lt);
            mt = (int)(mt1 + (smt - mt1) * (1.0 - e));
            mr = (int)(mr1 + (smr - mr1) * (1.0 - e));
        }
        gtk_layer_set_margin(GTK_WINDOW(a->btn_wins[k]),
                             GTK_LAYER_SHELL_EDGE_TOP, mt);
        gtk_layer_set_margin(GTK_WINDOW(a->btn_wins[k]),
                             GTK_LAYER_SHELL_EDGE_RIGHT, mr);
    }
    if (done) { a->anim_timer = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void start_entrance(App *a, int staggered) {
    if (a->anim_timer) { g_source_remove(a->anim_timer); a->anim_timer = 0; }
    a->anim_dur = staggered ? 320 : 150;
    a->anim_stagger = staggered;
    /* todas parten fuera de pantalla (ver stack_place) */
    for (int k = 0; k < NSLOT; k++) {
        gtk_layer_set_margin(GTK_WINDOW(a->btn_wins[k]),
                             GTK_LAYER_SHELL_EDGE_TOP, PANEL_MT);
        gtk_layer_set_margin(GTK_WINDOW(a->btn_wins[k]),
                             GTK_LAYER_SHELL_EDGE_RIGHT, 2000);
    }
    a->anim_t0 = g_get_monotonic_time();
    a->anim_timer = g_timeout_add(16, anim_tick, a);
}

static void refresh_detail(App *a, int idx);

static void show_detail(App *a, int idx) {
    for (int i = 0; i < NSLOT; i++)
        gtk_widget_hide(a->btn_wins[i]);
    for (int i = 0; i < NDETAIL; i++) {
        if (i == idx) gtk_widget_show(a->detail_wins[i]);
        else gtk_widget_hide(a->detail_wins[i]);
    }
    refresh_detail(a, idx);
}

static void on_back(GtkButton *b, gpointer d) {
    (void)b;
    show_main((App *)d);
}

static GtkWidget *page_header(App *a, const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkStyleContext *sc0 = gtk_widget_get_style_context(box);
    gtk_style_context_add_class(sc0, "detail-row");
    gtk_style_context_add_class(sc0, "page-head");
    GtkWidget *back = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(back),
                         sym("go-previous-symbolic", "go-previous"));
    GtkStyleContext *sc = gtk_widget_get_style_context(back);
    gtk_style_context_add_class(sc, "back");
    gtk_widget_set_tooltip_text(back, "Atrás");
    g_signal_connect(back, "clicked", G_CALLBACK(on_back), a);
    gtk_box_pack_start(GTK_BOX(box), back, FALSE, FALSE, 0);
    GtkWidget *t = gtk_label_new(title);
    GtkStyleContext *tc = gtk_widget_get_style_context(t);
    gtk_style_context_add_class(tc, "page-title");
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    return box;
}

/* ---------------- estado: polling en hilo, aplicado en la UI ---------------- */

typedef struct {
    int wifi_sup, wifi_on, sig;
    char *ssid;
    int bt_hw, bt_on;
    char *p_status, *p_title, *p_artist;
    int dnd, vol, muted, bright, dark;
} Snap;

typedef struct { App *a; Snap *s; } ApplyReq;

static int poll_busy = 0;

static gboolean apply_snap(gpointer d);

static gpointer poll_thread(gpointer d) {
    App *a = (App *)d;
    Snap *s = g_new0(Snap, 1);
    s->wifi_sup = sys_wifi_supported();
    if (s->wifi_sup) {
        char ssid[128];
        sys_wifi_all(&s->wifi_on, ssid, sizeof ssid, &s->sig);
        s->ssid = g_strdup(ssid);
    } else {
        s->ssid = g_strdup("");
    }
    s->bt_hw = sys_bt_hw();
    if (s->bt_hw) s->bt_on = sys_bt_powered();
    s->p_status = sys_player_status();
    char *meta = sys_player_meta();
    char *bar = meta ? strchr(meta, '|') : NULL;
    if (bar) {
        *bar = '\0';
        s->p_title = g_strdup(meta);
        s->p_artist = g_strdup(bar + 1);
    } else {
        s->p_title = g_strdup(meta ? meta : "");
        s->p_artist = g_strdup("");
    }
    g_free(meta);
    s->dnd = sys_dnd_get();
    s->vol = sys_volume_get(&s->muted);
    s->bright = sys_brightness_get();
    s->dark = sys_dark_get();
    ApplyReq *r = g_new0(ApplyReq, 1);
    r->a = a;
    r->s = s;
    g_idle_add(apply_snap, r);
    return NULL;
}

static gboolean tick(gpointer d) {
    if (!poll_busy) {
        poll_busy = 1;
        g_thread_new("cc-poll", poll_thread, d);
    }
    return G_SOURCE_CONTINUE;
}

static void paint_all(App *a, Snap *s) {

    if (s->wifi_sup) {
        pill_active(a->wifi_pill, s->wifi_on);
        gtk_label_set_text(GTK_LABEL(a->wifi_title), "Wi-Fi");
        gtk_label_set_text(GTK_LABEL(a->wifi_sub),
                           s->wifi_on ? (*s->ssid ? s->ssid : "Activado") : "Off");
        if (!s->wifi_on)
            set_img(a->wifi_icon, "network-wireless-disabled-symbolic",
                    "network-wireless-symbolic");
        else if (s->sig >= 70)
            set_img(a->wifi_icon, "network-wireless-signal-excellent-symbolic",
                    "network-wireless-symbolic");
        else if (s->sig >= 40)
            set_img(a->wifi_icon, "network-wireless-signal-good-symbolic",
                    "network-wireless-symbolic");
        else if (s->sig >= 0)
            set_img(a->wifi_icon, "network-wireless-signal-weak-symbolic",
                    "network-wireless-symbolic");
        else
            set_img(a->wifi_icon, "network-wireless-symbolic",
                    "network-wireless");
    } else {
        gtk_label_set_text(GTK_LABEL(a->wifi_sub), "No disponible");
    }

    if (s->bt_hw) {
        pill_active(a->bt_pill, s->bt_on);
        gtk_label_set_text(GTK_LABEL(a->bt_sub), s->bt_on ? "On" : "Off");
        set_img(a->bt_icon, s->bt_on ? "bluetooth-active-symbolic" : "bluetooth-disabled-symbolic",
                "bluetooth");
    } else {
        gtk_label_set_text(GTK_LABEL(a->bt_sub), "No disponible");
    }

    {
        const char *modes[] = { "Off", "Contactos", "Everyone" };
        pill_active(a->ad_pill, a->airdrop_mode == AIRDROP_EVERYONE);
        gtk_label_set_text(GTK_LABEL(a->ad_sub), modes[a->airdrop_mode]);
    }

    {
        int playing = (g_strcmp0(s->p_status, "Playing") == 0);
        int has = (*s->p_title || *s->p_artist);
        gtk_label_set_text(GTK_LABEL(a->np_title),
                           has ? (*s->p_title ? s->p_title : "Desconocido") : "Not Playing");
        gtk_label_set_text(GTK_LABEL(a->np_artist), has ? s->p_artist : "");
        set_img(a->np_play_icon,
                playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic",
                playing ? "media-playback-pause" : "media-playback-start");
        pill_active(a->np_card, has);
    }

    {
        round_active(a->dnd_btn, s->dnd);
        set_img(a->dnd_icon,
                s->dnd ? "notifications-disabled-symbolic" : "alarm-symbolic",
                s->dnd ? "user-invisible" : "preferences-desktop-notifications");
        round_active(a->mute_btn, s->muted);
    }

    {
        pill_active(a->focus_pill, s->dnd);
        gtk_label_set_text(GTK_LABEL(a->focus_sub), s->dnd ? "On" : "Focus");
        set_img(a->focus_icon, "weather-clear-night-symbolic", "night-light-symbolic");
    }

    if (!a->slider_busy) {
        if (s->bright >= 0) {
            gtk_range_set_value(GTK_RANGE(a->display_scale), s->bright);
            if (a->dsp_scale)
                gtk_range_set_value(GTK_RANGE(a->dsp_scale), s->bright);
        }
        if (s->vol >= 0) {
            gtk_range_set_value(GTK_RANGE(a->sound_scale), s->vol);
            if (a->snd_scale)
                gtk_range_set_value(GTK_RANGE(a->snd_scale), s->vol);
        }
        if (s->muted)
            set_img(a->sound_mute_icon, "audio-volume-muted-symbolic", "audio-volume-muted");
        else
            set_img(a->sound_mute_icon, "audio-volume-high-symbolic", "audio-volume-high");
    }

    round_active(a->dark_btn, s->dark);
}

static void snap_free(Snap *s) {
    g_free(s->ssid);
    g_free(s->p_status);
    g_free(s->p_title);
    g_free(s->p_artist);
    g_free(s);
}

/* caché en disco: pinta estado instantáneo al abrir (luego el poll corrige) */
static char *cache_path(void) {
    char *dir = g_build_filename(g_get_user_cache_dir(), "control-center", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *p = g_build_filename(dir, "state.ini", NULL);
    g_free(dir);
    return p;
}

static void cache_save(App *a, Snap *s) {
    GKeyFile *k = g_key_file_new();
    g_key_file_set_integer(k, "cc", "wifi_sup", s->wifi_sup);
    g_key_file_set_integer(k, "cc", "wifi_on", s->wifi_on);
    g_key_file_set_string(k, "cc", "ssid", s->ssid ? s->ssid : "");
    g_key_file_set_integer(k, "cc", "sig", s->sig);
    g_key_file_set_integer(k, "cc", "bt_hw", s->bt_hw);
    g_key_file_set_integer(k, "cc", "bt_on", s->bt_on);
    g_key_file_set_string(k, "cc", "p_status", s->p_status ? s->p_status : "");
    g_key_file_set_string(k, "cc", "p_title", s->p_title ? s->p_title : "");
    g_key_file_set_string(k, "cc", "p_artist", s->p_artist ? s->p_artist : "");
    g_key_file_set_integer(k, "cc", "dnd", s->dnd);
    g_key_file_set_integer(k, "cc", "vol", s->vol);
    g_key_file_set_integer(k, "cc", "muted", s->muted);
    g_key_file_set_integer(k, "cc", "bright", s->bright);
    g_key_file_set_integer(k, "cc", "dark", s->dark);
    g_key_file_set_integer(k, "cc", "airdrop", a->airdrop_mode);
    char *p = cache_path();
    g_key_file_save_to_file(k, p, NULL);
    g_free(p);
    g_key_file_free(k);
}

static void cache_load(App *a) {
    char *p = cache_path();
    GKeyFile *k = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(k, p, G_KEY_FILE_NONE, &err)) {
        g_clear_error(&err);
        g_key_file_free(k);
        g_free(p);
        return;
    }
    Snap *s = g_new0(Snap, 1);
    s->wifi_sup = g_key_file_get_integer(k, "cc", "wifi_sup", NULL);
    s->wifi_on = g_key_file_get_integer(k, "cc", "wifi_on", NULL);
    s->ssid = g_key_file_get_string(k, "cc", "ssid", NULL);
    if (!s->ssid) s->ssid = g_strdup("");
    s->sig = g_key_file_get_integer(k, "cc", "sig", NULL);
    s->bt_hw = g_key_file_get_integer(k, "cc", "bt_hw", NULL);
    s->bt_on = g_key_file_get_integer(k, "cc", "bt_on", NULL);
    s->p_status = g_key_file_get_string(k, "cc", "p_status", NULL);
    if (!s->p_status) s->p_status = g_strdup("");
    s->p_title = g_key_file_get_string(k, "cc", "p_title", NULL);
    if (!s->p_title) s->p_title = g_strdup("");
    s->p_artist = g_key_file_get_string(k, "cc", "p_artist", NULL);
    if (!s->p_artist) s->p_artist = g_strdup("");
    s->dnd = g_key_file_get_integer(k, "cc", "dnd", NULL);
    s->vol = g_key_file_get_integer(k, "cc", "vol", NULL);
    s->muted = g_key_file_get_integer(k, "cc", "muted", NULL);
    s->bright = g_key_file_get_integer(k, "cc", "bright", NULL);
    s->dark = g_key_file_get_integer(k, "cc", "dark", NULL);
    a->airdrop_mode = g_key_file_get_integer(k, "cc", "airdrop", NULL);
    if (a->airdrop_mode < 0 || a->airdrop_mode > 2)
        a->airdrop_mode = AIRDROP_EVERYONE;
    paint_all(a, s);
    snap_free(s);
    g_key_file_free(k);
    g_free(p);
}

static gboolean apply_snap(gpointer d) {
    ApplyReq *r = (ApplyReq *)d;
    poll_busy = 0;
    paint_all(r->a, r->s);
    cache_save(r->a, r->s);
    snap_free(r->s);
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* ---------------- acciones ---------------- */

static void wifi_toggle(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    if (!sys_wifi_supported()) { show_banner(a, "NetworkManager no disponible"); return; }
    sys_wifi_set(!sys_wifi_enabled());
    tick(a);
}

static void bt_toggle(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    if (!sys_bt_hw()) { show_banner(a, "Sin radio Bluetooth"); return; }
    sys_bt_set(!sys_bt_powered());
    tick(a);
}

static void airdrop_cycle(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    a->airdrop_mode = (a->airdrop_mode + 1) % 3;
    tick(a);
}

static void stage_toggle(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    if (sys_overview_toggle() != 0)
        show_banner(a, "Vista Overview no disponible en este Hyprland");
}

static void dnd_toggle(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    if (!sys_dnd_supported()) { show_banner(a, "Instala dunst o swaync para Focus"); return; }
    sys_dnd_set(!sys_dnd_get());
    tick(a);
}

static void mute_toggle_btn(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    sys_mute_toggle();
    tick(a);
}

#define OPEN_PAGE(which) \
    static void open_##which(GtkButton *b, gpointer d) { (void)b; show_detail((App *)d, D_##which); }
OPEN_PAGE(WIFI)
OPEN_PAGE(BT)
OPEN_PAGE(AD)
OPEN_PAGE(SOUND)
OPEN_PAGE(DISPLAY)
OPEN_PAGE(FOCUS)
OPEN_PAGE(MIRROR)

static void on_dark(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    int dark = !sys_dark_get();
    sys_dark_set(dark);
    for (int i = 0; i < a->n_wins; i++) {
        GtkStyleContext *sc = gtk_widget_get_style_context(a->all_wins[i]);
        if (dark) {
            gtk_style_context_add_class(sc, "cc-dark");
            gtk_style_context_remove_class(sc, "cc-light");
        } else {
            gtk_style_context_add_class(sc, "cc-light");
            gtk_style_context_remove_class(sc, "cc-dark");
        }
    }
    tick(a);
}

static void on_calc(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    if (!sys_has("gnome-calculator") && !sys_has("qalculate-gtk") &&
        !sys_has("kcalc") && !sys_has("galculator"))
        show_banner(a, "Instala gnome-calculator");
    else
        sys_open_calculator();
}

static void on_shot(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    if (!sys_has("hyprshot")) { show_banner(a, "hyprshot no instalado"); return; }
    sys_screenshot();
    show_banner(a, "Screenshot recently");
}

static void on_lock(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    sys_lock();
}

static guint set_timer_vol = 0, set_timer_bri = 0;
static int pend_vol = -1, pend_bri = -1;

static gboolean do_vol_set(gpointer d) {
    set_timer_vol = 0;
    if (pend_vol >= 0) { sys_volume_set(pend_vol); pend_vol = -1; }
    (void)d;
    return G_SOURCE_REMOVE;
}

static gboolean do_bri_set(gpointer d) {
    set_timer_bri = 0;
    if (pend_bri >= 0) { sys_brightness_set(pend_bri); pend_bri = -1; }
    (void)d;
    return G_SOURCE_REMOVE;
}

static void flush_sets(App *a) {
    if (set_timer_vol) { g_source_remove(set_timer_vol); set_timer_vol = 0; }
    if (set_timer_bri) { g_source_remove(set_timer_bri); set_timer_bri = 0; }
    if (pend_vol >= 0) { sys_volume_set(pend_vol); pend_vol = -1; }
    if (pend_bri >= 0) { sys_brightness_set(pend_bri); pend_bri = -1; }
    (void)a;
}

static void on_display_changed(GtkRange *r, gpointer d) {
    App *a = (App *)d;
    if (a->slider_busy) {
        pend_bri = (int)gtk_range_get_value(r);
        if (!set_timer_bri) set_timer_bri = g_timeout_add(90, do_bri_set, a);
    }
    if (a->dsp_scale && GTK_WIDGET(r) != a->dsp_scale)
        gtk_range_set_value(GTK_RANGE(a->dsp_scale), gtk_range_get_value(r));
}

static void on_sound_changed(GtkRange *r, gpointer d) {
    App *a = (App *)d;
    if (a->slider_busy) {
        pend_vol = (int)gtk_range_get_value(r);
        if (!set_timer_vol) set_timer_vol = g_timeout_add(90, do_vol_set, a);
    }
    if (a->snd_scale && GTK_WIDGET(r) != a->snd_scale)
        gtk_range_set_value(GTK_RANGE(a->snd_scale), gtk_range_get_value(r));
}

static gboolean slider_press(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)w; (void)e;
    ((App *)d)->slider_busy = 1;
    return FALSE;
}
static gboolean slider_release(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)w; (void)e;
    App *a = (App *)d;
    a->slider_busy = 0;
    flush_sets(a);
    tick(a);
    return FALSE;
}

static void hook_slider(App *a, GtkWidget *s, GCallback cb) {
    g_signal_connect(s, "value-changed", cb, a);
    g_signal_connect(s, "button-press-event", G_CALLBACK(slider_press), a);
    g_signal_connect(s, "button-release-event", G_CALLBACK(slider_release), a);
}

/* ---------------- constructores ---------------- */

static GtkWidget *pill_card(App *a, const char *icon, const char *fallback,
                            const char *title, const char *sub,
                            GtkWidget **o_icon, GtkWidget **o_title,
                            GtkWidget **o_sub, GCallback on_icon,
                            GCallback on_text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkStyleContext *sc = gtk_widget_get_style_context(box);
    gtk_style_context_add_class(sc, "pill");

    GtkWidget *ib = gtk_button_new();
    GtkStyleContext *ic = gtk_widget_get_style_context(ib);
    gtk_style_context_add_class(ic, "icon");
    GtkWidget *img = sym(icon, fallback);
    gtk_button_set_image(GTK_BUTTON(ib), img);
    gtk_box_pack_start(GTK_BOX(box), ib, FALSE, FALSE, 0);
    g_signal_connect(ib, "clicked", on_icon, a);

    GtkWidget *tb = gtk_button_new();
    GtkStyleContext *tc = gtk_widget_get_style_context(tb);
    gtk_style_context_add_class(tc, "textarea");
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    GtkStyleContext *ts = gtk_widget_get_style_context(t);
    gtk_style_context_add_class(ts, "card-title");
    GtkWidget *s = gtk_label_new(sub);
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(s), 14);
    GtkStyleContext *ss = gtk_widget_get_style_context(s);
    gtk_style_context_add_class(ss, "card-sub");
    gtk_box_pack_start(GTK_BOX(txt), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), s, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(tb), txt);
    gtk_box_pack_start(GTK_BOX(box), tb, TRUE, TRUE, 0);
    g_signal_connect(tb, "clicked", on_text, a);

    if (o_icon) *o_icon = img;
    if (o_title) *o_title = t;
    if (o_sub) *o_sub = s;
    return box;
}

static GtkWidget *round_btn(App *a, const char *icon, const char *fallback,
                            const char *tip, GCallback cb, GtkWidget **o_icon) {
    GtkWidget *b = gtk_button_new();
    GtkStyleContext *sc = gtk_widget_get_style_context(b);
    gtk_style_context_add_class(sc, "round");
    GtkWidget *img = sym(icon, fallback);
    gtk_button_set_image(GTK_BUTTON(b), img);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, a);
    if (o_icon) *o_icon = img;
    return b;
}

static void on_net_clicked(GtkButton *row, gpointer u);
static void on_bt_dev_clicked(GtkButton *row, gpointer u);
static void on_mirror_clicked(GtkButton *row, gpointer u);
static gboolean tick_once(gpointer d);

typedef struct { App *a; char *raw; } ScanRes;

static gboolean wifi_rows_idle(gpointer d) {
    ScanRes *r = (ScanRes *)d;
    App *a = r->a;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(a->wifi_list));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    if (r->raw && *r->raw) {
        char **lines = g_strsplit(r->raw, "\n", -1);
        int n = 0;
        for (int i = 0; lines[i] && n < 14; i++) {
            char *ln = lines[i];
            if (!*ln) continue;
            int inuse = g_str_has_prefix(ln, "*:");
            char *p = strchr(ln, ':');
            if (!p) continue;
            char *rest = p + 1;
            char *last1 = strrchr(rest, ':');
            if (!last1) continue;
            *last1 = '\0';
            char *sec = last1 + 1;
            char *last2 = strrchr(rest, ':');
            if (!last2) continue;
            *last2 = '\0';
            int sig = atoi(last2 + 1);
            char *ssid = rest;
            if (!*ssid) continue;

            GtkWidget *row = gtk_button_new();
            GtkStyleContext *rc = gtk_widget_get_style_context(row);
            gtk_style_context_add_class(rc, "netrow");
            GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *dot = gtk_label_new(inuse ? "●" : "○");
            gtk_box_pack_start(GTK_BOX(hb), dot, FALSE, FALSE, 0);
            GtkWidget *name = gtk_label_new(ssid);
            gtk_widget_set_halign(name, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(name), 18);
            gtk_box_pack_start(GTK_BOX(hb), name, TRUE, TRUE, 0);
            char sigs[16];
            g_snprintf(sigs, sizeof sigs, "%d%%", sig);
            GtkWidget *sg = gtk_label_new(sigs);
            GtkStyleContext *sgc = gtk_widget_get_style_context(sg);
            gtk_style_context_add_class(sgc, "row-sub");
            gtk_box_pack_start(GTK_BOX(hb), sg, FALSE, FALSE, 0);
            if (*sec) {
                GtkWidget *lk = sym("system-lock-screen-symbolic", "dialog-password");
                gtk_box_pack_start(GTK_BOX(hb), lk, FALSE, FALSE, 0);
            }
            gtk_container_add(GTK_CONTAINER(row), hb);
            g_object_set_data_full(G_OBJECT(row), "ssid", g_strdup(ssid), g_free);
            g_object_set_data_full(G_OBJECT(row), "sec", g_strdup(sec), g_free);
            g_object_set_data(G_OBJECT(row), "app", a);
            g_object_set_data(G_OBJECT(row), "inuse", GINT_TO_POINTER(inuse));
            g_signal_connect(row, "clicked", G_CALLBACK(on_net_clicked), NULL);
            gtk_container_add(GTK_CONTAINER(a->wifi_list), row);
            n++;
        }
        g_strfreev(lines);
    }
    gtk_widget_show_all(a->wifi_list);
    gtk_button_set_label(GTK_BUTTON(a->wifi_scan_btn), "Actualizar");
    g_free(r->raw);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static gpointer wifi_scan_thread(gpointer d) {
    App *a = (App *)d;
    char *raw = sys_exec(
        "nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list --rescan yes");
    ScanRes *r = g_new0(ScanRes, 1);
    r->a = a; r->raw = raw;
    g_idle_add(wifi_rows_idle, r);
    return NULL;
}

static void wifi_refresh(GtkButton *b, gpointer d) {
    (void)b;
    App *a = (App *)d;
    gtk_button_set_label(GTK_BUTTON(a->wifi_scan_btn), "Buscando…");
    g_thread_new("wifi-scan", wifi_scan_thread, a);
}

static void on_wifi_switch(GObject *sw, GParamSpec *ps, gpointer d) {
    (void)ps;
    App *a = (App *)d;
    sys_wifi_set(gtk_switch_get_active(GTK_SWITCH(sw)));
    tick(a);
}

static void wifi_fill_cached(App *a) {
    ScanRes *r = g_new0(ScanRes, 1);
    r->a = a;
    r->raw = sys_wifi_scan_raw();
    wifi_rows_idle(r);
}

typedef struct { App *a; char *ssid; char *pw; } ConnReq;

static gboolean tick_once(gpointer d) {
    tick(d);
    return G_SOURCE_REMOVE;
}

static gpointer conn_thread(gpointer d) {
    ConnReq *c = (ConnReq *)d;
    sys_wifi_connect(c->ssid, c->pw);
    g_idle_add(tick_once, c->a);
    g_free(c->ssid); g_free(c->pw); g_free(c);
    return NULL;
}

static void on_net_clicked(GtkButton *row, gpointer u) {
    (void)u;
    App *a = (App *)g_object_get_data(G_OBJECT(row), "app");
    const char *ssid = (const char *)g_object_get_data(G_OBJECT(row), "ssid");
    const char *sec = (const char *)g_object_get_data(G_OBJECT(row), "sec");
    int inuse = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "inuse"));
    if (!ssid || inuse) return;

    char *pw = NULL;
    if (sec && *sec) {
        GtkWidget *dlg = gtk_dialog_new_with_buttons(
            "Contraseña Wi-Fi", GTK_WINDOW(a->catcher_win),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Cancelar", GTK_RESPONSE_CANCEL, "_Conectar", GTK_RESPONSE_OK, NULL);
        GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
        GtkWidget *lbl = gtk_label_new(ssid);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        gtk_box_pack_start(GTK_BOX(content), lbl, FALSE, FALSE, 6);
        gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 6);
        gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
        gtk_widget_show_all(dlg);
        a->modal_open = 1;
        gint res = gtk_dialog_run(GTK_DIALOG(dlg));
        if (res == GTK_RESPONSE_OK)
            pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
        gtk_widget_destroy(dlg);
        a->modal_open = 0;
        if (res != GTK_RESPONSE_OK) { g_free(pw); return; }
    }
    show_banner(a, "Conectando…");
    ConnReq *c = g_new0(ConnReq, 1);
    c->a = a; c->ssid = g_strdup(ssid); c->pw = pw;
    g_thread_new("wifi-conn", conn_thread, c);
}

static void on_bt_switch(GObject *sw, GParamSpec *ps, gpointer d) {
    (void)ps;
    App *a = (App *)d;
    sys_bt_set(gtk_switch_get_active(GTK_SWITCH(sw)));
    tick(a);
}

static void on_bt_dev_clicked(GtkButton *row, gpointer u) {
    (void)u;
    const char *cmd = (const char *)g_object_get_data(G_OBJECT(row), "cmd");
    if (cmd) sys_spawn(cmd);
}

static void bt_fill(App *a) {
    gtk_switch_set_active(GTK_SWITCH(a->bt_switch), sys_bt_powered());
    GList *kids = gtk_container_get_children(GTK_CONTAINER(a->bt_list));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    if (!sys_bt_ctl()) {
        gtk_label_set_text(GTK_LABEL(a->bt_hint),
            "Instala bluez-utils para ver dispositivos (bluetoothctl). "
            "El interruptor usa rfkill y sí funciona.");
        gtk_widget_show(a->bt_hint);
    } else {
        gtk_widget_hide(a->bt_hint);
        char *raw = sys_bt_devices_raw();
        if (raw && *raw) {
            char **lines = g_strsplit(raw, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                if (!g_str_has_prefix(lines[i], "Device ")) continue;
                char *mac = lines[i] + 7;
                char *sp = strchr(mac, ' ');
                if (!sp) continue;
                *sp = '\0';
                char *name = sp + 1;
                GtkWidget *row = gtk_button_new();
                GtkStyleContext *rc = gtk_widget_get_style_context(row);
                gtk_style_context_add_class(rc, "netrow");
                GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                GtkWidget *ic = sym("bluetooth-active-symbolic", "bluetooth");
                gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, FALSE, 0);
                GtkWidget *nl = gtk_label_new(name);
                gtk_widget_set_halign(nl, GTK_ALIGN_START);
                gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
                gtk_box_pack_start(GTK_BOX(hb), nl, TRUE, TRUE, 0);
                gtk_container_add(GTK_CONTAINER(row), hb);
                char *m = g_strdup_printf("bluetoothctl connect %s", mac);
                g_object_set_data_full(G_OBJECT(row), "cmd", m, g_free);
                g_signal_connect(row, "clicked", G_CALLBACK(
                    on_bt_dev_clicked), NULL);
                gtk_container_add(GTK_CONTAINER(a->bt_list), row);
            }
            g_strfreev(lines);
        }
        g_free(raw);
    }
    gtk_widget_show_all(a->bt_list);
}

static void on_snd_mute_switch(GObject *sw, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    sys_mute_set(gtk_switch_get_active(GTK_SWITCH(sw)));
}

static void on_sink_clicked(GtkButton *row, gpointer u) {
    (void)u;
    const char *id = (const char *)g_object_get_data(G_OBJECT(row), "sink");
    if (id) sys_sink_default(id);
}

static void snd_fill(App *a) {
    int muted = 0, v = sys_volume_get(&muted);
    if (v >= 0) gtk_range_set_value(GTK_RANGE(a->snd_scale), v);
    gtk_switch_set_active(GTK_SWITCH(a->snd_mute_switch), muted);

    GList *kids = gtk_container_get_children(GTK_CONTAINER(a->snd_list));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    char *def = sys_exec("pactl get-default-sink");
    if (!def || !*def) { g_free(def); def = sys_exec("pactl info | grep 'Default Sink' | cut -d: -f2"); }
    char *raw = sys_sinks_raw();
    if (raw && *raw) {
        char **lines = g_strsplit(raw, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i]) continue;
            char **cols = g_strsplit(lines[i], "\t", -1);
            if (!cols[0] || !cols[1]) { g_strfreev(cols); continue; }
            GtkWidget *row = gtk_button_new();
            GtkStyleContext *rc = gtk_widget_get_style_context(row);
            gtk_style_context_add_class(rc, "netrow");
            GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            int is_def = (def && strstr(def, cols[1]) != NULL);
            GtkWidget *dot = gtk_label_new(is_def ? "●" : "○");
            gtk_box_pack_start(GTK_BOX(hb), dot, FALSE, FALSE, 0);
            GtkWidget *nl = gtk_label_new(cols[1]);
            gtk_widget_set_halign(nl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(nl), 24);
            gtk_box_pack_start(GTK_BOX(hb), nl, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), hb);
            g_object_set_data_full(G_OBJECT(row), "sink", g_strdup(cols[1]), g_free);
            g_signal_connect(row, "clicked", G_CALLBACK(on_sink_clicked), NULL);
            gtk_container_add(GTK_CONTAINER(a->snd_list), row);
            g_strfreev(cols);
        }
        g_strfreev(lines);
    }
    g_free(def); g_free(raw);
    gtk_widget_show_all(a->snd_list);
}

static void on_dsp_dark(GObject *sw, GParamSpec *ps, gpointer d) {
    (void)ps;
    App *a = (App *)d;
    sys_dark_set(gtk_switch_get_active(GTK_SWITCH(sw)));
    tick(a);
}

static void on_night(GObject *sw, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    sys_nightlight_set(gtk_switch_get_active(GTK_SWITCH(sw)));
}

static void dsp_fill(App *a) {
    int b = sys_brightness_get();
    if (b >= 0) gtk_range_set_value(GTK_RANGE(a->dsp_scale), b);
    gtk_switch_set_active(GTK_SWITCH(a->dsp_dark_switch), sys_dark_get());
    if (sys_nightlight_supported()) {
        gtk_widget_show(a->dsp_night_row);
        gtk_switch_set_active(GTK_SWITCH(a->dsp_night_switch), sys_nightlight_get());
    } else {
        gtk_widget_hide(a->dsp_night_row);
    }
}

static void on_focus_switch(GObject *sw, GParamSpec *ps, gpointer d) {
    (void)ps;
    App *a = (App *)d;
    sys_dnd_set(gtk_switch_get_active(GTK_SWITCH(sw)));
    tick(a);
}

static void focus_fill(App *a) {
    gtk_switch_set_active(GTK_SWITCH(a->focus_switch), sys_dnd_get());
}

static void on_ad_radio(GtkButton *row, gpointer d) {
    App *a = (App *)d;
    a->airdrop_mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "mode"));
    tick(a);
    show_main(a);
}

static void on_mirror_clicked(GtkButton *row, gpointer u) {
    (void)u;
    App *a = (App *)g_object_get_data(G_OBJECT(row), "app");
    char *spec = (char *)g_object_get_data(G_OBJECT(row), "spec");
    char *sep = spec ? strchr(spec, '|') : NULL;
    if (!spec || !sep) return;
    *sep = '\0';
    sys_mirror(sep + 1, spec);
    show_banner(a, "Duplicando pantalla");
}

static void mirror_fill(App *a) {
    GtkWidget *list = a->mirror_list;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    char *raw = sys_monitors_raw();
    char *focused = sys_focused_monitor();
    if (raw && *raw) {
        char **mons = g_strsplit(raw, "\n", -1);
        for (int i = 0; mons[i]; i++) {
            if (!*mons[i]) continue;
            GtkWidget *row = gtk_button_new();
            GtkStyleContext *rc = gtk_widget_get_style_context(row);
            gtk_style_context_add_class(rc, "netrow");
            GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget *ic = sym("video-display-symbolic", "computer");
            gtk_box_pack_start(GTK_BOX(hb), ic, FALSE, FALSE, 0);
            char lbl[160];
            g_snprintf(lbl, sizeof lbl, "%s%s",
                       mons[i], (!strcmp(mons[i], focused ?: "")) ? " (principal)" : "");
            GtkWidget *nl = gtk_label_new(lbl);
            gtk_widget_set_halign(nl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(hb), nl, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), hb);
            char *spec = g_strdup_printf("%s|%s", mons[i], focused ?: "");
            g_object_set_data_full(G_OBJECT(row), "spec", spec, g_free);
            g_object_set_data(G_OBJECT(row), "app", a);
            g_signal_connect(row, "clicked", G_CALLBACK(on_mirror_clicked), NULL);
            gtk_container_add(GTK_CONTAINER(list), row);
        }
        g_strfreev(mons);
    }
    g_free(raw); g_free(focused);
    gtk_widget_show_all(list);
}

static GtkWidget *slider_card(App *a, const char *title, const char *lo,
                              const char *hi, GtkWidget **o_scale,
                              GCallback changed, const char *extra_icon,
                              GCallback extra_cb, GtkWidget **o_extra_icon) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkStyleContext *sc = gtk_widget_get_style_context(card);
    gtk_style_context_add_class(sc, "pill");
    gtk_style_context_add_class(sc, "slider-card");
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    GtkStyleContext *tc = gtk_widget_get_style_context(t);
    gtk_style_context_add_class(tc, "card-title");
    gtk_box_pack_start(GTK_BOX(card), t, FALSE, FALSE, 0);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *li = sym(lo, lo);
    gtk_box_pack_start(GTK_BOX(hb), li, FALSE, FALSE, 0);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);
    hook_slider(a, scale, changed);
    gtk_box_pack_start(GTK_BOX(hb), scale, TRUE, TRUE, 0);
    GtkWidget *hi_img = NULL;
    if (extra_icon) {
        GtkWidget *eb = gtk_button_new();
        GtkStyleContext *ec = gtk_widget_get_style_context(eb);
        gtk_style_context_add_class(ec, "mini");
        hi_img = sym(extra_icon, hi);
        gtk_button_set_image(GTK_BUTTON(eb), hi_img);
        g_signal_connect(eb, "clicked", extra_cb, a);
        gtk_box_pack_start(GTK_BOX(hb), eb, FALSE, FALSE, 0);
        if (o_extra_icon) *o_extra_icon = hi_img;
    } else {
        hi_img = sym(hi, hi);
        gtk_box_pack_start(GTK_BOX(hb), hi_img, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(card), hb, FALSE, FALSE, 0);
    if (o_scale) *o_scale = scale;
    return card;
}

static GtkWidget *scroll_wrap(GtkWidget *child, int height) {
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sw, -1, height);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

static GtkWidget *build_wifi(App *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(v), page_header(a, "Wi-Fi"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v),
        switch_row("Wi-Fi", "Activar o desactivar la radio", &a->wifi_switch),
        FALSE, FALSE, 0);
    g_signal_connect(a->wifi_switch, "notify::active", G_CALLBACK(on_wifi_switch), a);
    a->wifi_status = gtk_label_new("");
    gtk_widget_set_halign(a->wifi_status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), a->wifi_status, FALSE, FALSE, 0);
    a->wifi_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(a->wifi_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(v), scroll_wrap(a->wifi_list, 220), TRUE, TRUE, 0);
    a->wifi_scan_btn = gtk_button_new_with_label("Actualizar");
    GtkStyleContext *sbc = gtk_widget_get_style_context(a->wifi_scan_btn);
    gtk_style_context_add_class(sbc, "action-btn");
    g_signal_connect(a->wifi_scan_btn, "clicked", G_CALLBACK(wifi_refresh), a);
    gtk_box_pack_start(GTK_BOX(v), a->wifi_scan_btn, FALSE, FALSE, 0);
    return v;
}

static GtkWidget *build_bt(App *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(v), page_header(a, "Bluetooth"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v),
        switch_row("Bluetooth", "Activar o desactivar", &a->bt_switch),
        FALSE, FALSE, 0);
    g_signal_connect(a->bt_switch, "notify::active", G_CALLBACK(on_bt_switch), a);
    a->bt_hint = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(a->bt_hint), TRUE);
    gtk_widget_set_halign(a->bt_hint, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), a->bt_hint, FALSE, FALSE, 0);
    a->bt_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(a->bt_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(v), scroll_wrap(a->bt_list, 200), TRUE, TRUE, 0);
    return v;
}

static GtkWidget *build_sound(App *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(v), page_header(a, "Sound"), FALSE, FALSE, 0);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *li = sym("audio-volume-low-symbolic", "audio-volume-low");
    gtk_box_pack_start(GTK_BOX(hb), li, FALSE, FALSE, 0);
    a->snd_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(a->snd_scale), FALSE);
    gtk_widget_set_hexpand(a->snd_scale, TRUE);
    hook_slider(a, a->snd_scale, G_CALLBACK(on_sound_changed));
    gtk_box_pack_start(GTK_BOX(hb), a->snd_scale, TRUE, TRUE, 0);
    GtkWidget *hi = sym("audio-volume-high-symbolic", "audio-volume-high");
    gtk_box_pack_start(GTK_BOX(hb), hi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), hb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v),
        switch_row("Silenciar", "Silenciar la salida", &a->snd_mute_switch),
        FALSE, FALSE, 0);
    g_signal_connect(a->snd_mute_switch, "notify::active",
                     G_CALLBACK(on_snd_mute_switch), a);
    GtkWidget *t = gtk_label_new("Salida");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    a->snd_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(a->snd_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(v), scroll_wrap(a->snd_list, 160), TRUE, TRUE, 0);
    return v;
}

static GtkWidget *build_display(App *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(v), page_header(a, "Display"), FALSE, FALSE, 0);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(hb),
        sym("display-brightness-low-symbolic", "display-brightness"), FALSE, FALSE, 0);
    a->dsp_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(a->dsp_scale), FALSE);
    gtk_widget_set_hexpand(a->dsp_scale, TRUE);
    hook_slider(a, a->dsp_scale, G_CALLBACK(on_display_changed));
    gtk_box_pack_start(GTK_BOX(hb), a->dsp_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hb),
        sym("display-brightness-symbolic", "display-brightness"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), hb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v),
        switch_row("Modo oscuro", "Tema Adwaita dark", &a->dsp_dark_switch),
        FALSE, FALSE, 0);
    g_signal_connect(a->dsp_dark_switch, "notify::active", G_CALLBACK(on_dsp_dark), a);
    a->dsp_night_row = switch_row("Night Light", "Luz cálida (gammastep/wlsunset)",
                                  &a->dsp_night_switch);
    gtk_box_pack_start(GTK_BOX(v), a->dsp_night_row, FALSE, FALSE, 0);
    g_signal_connect(a->dsp_night_switch, "notify::active", G_CALLBACK(on_night), a);
    return v;
}

static GtkWidget *build_focus(App *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(v), page_header(a, "Focus"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v),
        switch_row("No molestar", "Pausar notificaciones (dunst)", &a->focus_switch),
        FALSE, FALSE, 0);
    g_signal_connect(a->focus_switch, "notify::active", G_CALLBACK(on_focus_switch), a);
    GtkWidget *note = gtk_label_new(
        "Equivale al modo Focus de macOS: silencia avisos hasta desactivarlo.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), note, FALSE, FALSE, 0);
    return v;
}

static GtkWidget *build_mirror(App *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(v), page_header(a, "Duplicar pantalla"), FALSE, FALSE, 0);
    GtkWidget *note = gtk_label_new(
        "Toca un monitor para duplicar en él la pantalla principal.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), note, FALSE, FALSE, 0);
    a->mirror_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(a->mirror_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(v), scroll_wrap(a->mirror_list, 180), TRUE, TRUE, 0);
    return v;
}

static GtkWidget *build_airdrop(App *a) {
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(v), page_header(a, "AirDrop"), FALSE, FALSE, 0);
    const char *opts[] = { "Off", "Solo contactos", "Everyone" };
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = gtk_button_new_with_label(opts[i]);
        GtkStyleContext *rc = gtk_widget_get_style_context(row);
        gtk_style_context_add_class(rc, "netrow");
        g_object_set_data(G_OBJECT(row), "mode", GINT_TO_POINTER(i));
        g_signal_connect(row, "clicked", G_CALLBACK(on_ad_radio), a);
        gtk_box_pack_start(GTK_BOX(v), row, FALSE, FALSE, 0);
    }
    GtkWidget *note = gtk_label_new(
        "En Linux no existe AirDrop. Alternativas: KDE Connect, "
        "Warpinator o LocalSend.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(v), note, FALSE, FALSE, 0);
    return v;
}

static void refresh_detail(App *a, int idx) {
    switch (idx) {
    case D_WIFI: {
        gtk_switch_set_active(GTK_SWITCH(a->wifi_switch), sys_wifi_enabled());
        char *ssid = sys_wifi_ssid();
        char st[192];
        g_snprintf(st, sizeof st, "Red: %s",
                   (ssid && *ssid) ? ssid : "desconectado");
        gtk_label_set_text(GTK_LABEL(a->wifi_status), st);
        g_free(ssid);
        wifi_fill_cached(a);
        break;
    }
    case D_BT: bt_fill(a); break;
    case D_SOUND: snd_fill(a); break;
    case D_DISPLAY: dsp_fill(a); break;
    case D_FOCUS: focus_fill(a); break;
    case D_MIRROR: mirror_fill(a); break;
    default: break;
    }
}

/* ---------------- ventanas layer-shell ---------------- */

static gboolean on_win_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
    (void)w; (void)d;
    if (e->keyval == GDK_KEY_Escape) quit_all();
    return FALSE;
}

static gboolean layer_focus_check(gpointer d) {
    App *a = (App *)d;
    a->focus_timer = 0;
    if (!a->ready || a->modal_open) return G_SOURCE_REMOVE;
    for (int i = 0; i < a->n_wins; i++) {
        if (gtk_widget_get_visible(a->all_wins[i]) &&
            gtk_window_has_toplevel_focus(GTK_WINDOW(a->all_wins[i])))
            return G_SOURCE_REMOVE;
    }
    quit_all();
    return G_SOURCE_REMOVE;
}

static gboolean on_win_focus_out(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)w; (void)e;
    App *a = (App *)d;
    if (!a->focus_timer) a->focus_timer = g_timeout_add(250, layer_focus_check, a);
    return FALSE;
}

static gboolean make_ready(gpointer d) {
    ((App *)d)->ready = 1;
    return G_SOURCE_REMOVE;
}

static int theme_dark_cached = -1;

/* Ventana layer-shell. Con full=1 ocupa toda la pantalla (catcher);
 * si no, se ajusta al contenido anclada arriba-derecha (botones/detalles)
 * con los márgenes dados: el quad de cristal queda ceñido al botón. */
static GtkWidget *layer_win_full(App *a, const char *title, const char *ns,
                                 int full, int kb_mode) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    gtk_widget_set_app_paintable(win, TRUE);
    GdkScreen *scr = gtk_widget_get_screen(win);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis) gtk_widget_set_visual(win, vis);

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win), ns);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, full);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, full);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, PANEL_MT);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, PANEL_MR);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, 0);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), 0);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), kb_mode);

    GtkStyleContext *wsc = gtk_widget_get_style_context(win);
    if (theme_dark_cached < 0) theme_dark_cached = sys_dark_get();
    if (theme_dark_cached) gtk_style_context_add_class(wsc, "cc-dark");
    else gtk_style_context_add_class(wsc, "cc-light");

    if (kb_mode != GTK_LAYER_SHELL_KEYBOARD_MODE_NONE) {
        g_signal_connect(win, "key-press-event", G_CALLBACK(on_win_key), a);
        g_signal_connect(win, "focus-out-event", G_CALLBACK(on_win_focus_out), a);
    }
    reg_win(a, win);
    return win;
}

static GtkWidget *layer_win(App *a, const char *title) {
    return layer_win_full(a, title, APP_ID, 0,
                          GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
}

/* todas parten apiladas FUERA de pantalla (margen der. enorme);
 * la animación arranca al mapear (ver on_first_map) */
static void stack_place(GtkWidget *win) {
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, PANEL_MT);
    gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, 2000);
}

static gboolean reveal_tick(gpointer d);

static gint64 t0_global = 0;
static long ms_now(void) {
    return (long)((g_get_monotonic_time() - t0_global) / 1000);
}

static void on_first_map(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)w; (void)e;
    App *a = (App *)d;
    if (a->awaiting_maps > 0) {
        a->awaiting_maps--;
        fprintf(stderr, "MAP left=%d t=%ldms\n", a->awaiting_maps, ms_now());
        if (a->awaiting_maps == 0) {
            /* todas mapeadas: gracia de 2-3 frames para el primer commit
             * y revelado conjunto + entrada (nada de pop-in escalonado) */
            g_timeout_add(120, reveal_tick, a);
        }
    }
}

static gboolean reveal_tick(gpointer d) {
    App *a = (App *)d;
    fprintf(stderr, "REVEAL t=%ldms\n", ms_now());
    for (int k = 0; k < NSLOT; k++) {
        gtk_widget_set_sensitive(a->btn_wins[k], TRUE);
        gtk_widget_set_opacity(a->btn_wins[k], 1.0);
    }
    start_entrance(a, a->reveal_quick ? 0 : 1);
    a->reveal_quick = 0;
    return G_SOURCE_REMOVE;
}

static gboolean map_fallback(gpointer d) {
    App *a = (App *)d;
    if (a->awaiting_maps > 0) {
        a->awaiting_maps = 0;
        reveal_tick(d);
    }
    return G_SOURCE_REMOVE;
}

/* ---------------- CSS ---------------- */

static char *css_path(void) {
    const char *home = g_get_home_dir();
    char *p = g_build_filename(home, ".config", "control-center", "style.css", NULL);
    if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
    g_free(p);
    if (g_file_test("./style.css", G_FILE_TEST_EXISTS)) return g_strdup("./style.css");
    if (g_file_test("../style.css", G_FILE_TEST_EXISTS)) return g_strdup("../style.css");
    p = g_build_filename(home, ".local", "share", "control-center", "style.css", NULL);
    if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
    g_free(p);
    return NULL;
}

static void load_css(void) {
    char *path = css_path();
    if (!path) {
        g_printerr("control-center: style.css no encontrado, uso estilo mínimo\n");
        return;
    }
    GtkCssProvider *prov = gtk_css_provider_new();
    GError *err = NULL;
    gtk_css_provider_load_from_path(prov, path, &err);
    if (err) {
        g_printerr("control-center: error CSS %s: %s\n", path, err->message);
        g_error_free(err);
    } else {
        GdkScreen *scr = gdk_screen_get_default();
        if (scr)
            gtk_style_context_add_provider_for_screen(
                scr, GTK_STYLE_PROVIDER(prov),
                GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    g_object_unref(prov);
    g_free(path);
}

static int run_check(const char *css_override) {
    int fails = 0;
    printf("=== control-center --check ===\n");

    GtkCssProvider *prov = gtk_css_provider_new();
    GError *err = NULL;
    char *path = css_override ? g_strdup(css_override) : css_path();
    if (path && gtk_css_provider_load_from_path(prov, path, &err)) {
        printf("[OK] CSS: %s\n", path);
    } else {
        printf("[FAIL] CSS: %s (%s)\n", path ?: "(no encontrado)",
               err ? err->message : "?");
        fails++;
    }
    g_clear_error(&err);
    g_object_unref(prov);
    g_free(path);

    printf("wifi: supported=%d enabled=%d\n", sys_wifi_supported(), sys_wifi_enabled());
    char *ssid = sys_wifi_ssid();
    printf("ssid: '%s' signal=%d\n", ssid, sys_wifi_signal());
    g_free(ssid);
    printf("bt: hw=%d powered=%d ctl=%d\n", sys_bt_hw(), sys_bt_powered(), sys_bt_ctl());
    int muted = 0, vol = sys_volume_get(&muted);
    printf("volume: %d muted=%d\n", vol, muted);
    printf("brightness: %d\n", sys_brightness_get());
    char *st = sys_player_status(), *meta = sys_player_meta(), *pn = sys_player_name();
    printf("player: name='%s' status='%s' meta='%s'\n", pn, st, meta);
    g_free(st); g_free(meta); g_free(pn);
    printf("dnd: supported=%d active=%d\n", sys_dnd_supported(), sys_dnd_get());
    printf("dark: %d nightlight=%d (sup=%d)\n",
           sys_dark_get(), sys_nightlight_get(), sys_nightlight_supported());
    char *mons = sys_monitors_raw(), *fm = sys_focused_monitor();
    printf("monitors: '%s' focused='%s'\n", mons, fm);
    g_free(mons); g_free(fm);
    printf("tools: calculator=%d shot=%d lock=%d\n",
           sys_has("gnome-calculator") || sys_has("qalculate-gtk") || sys_has("kcalc"),
           sys_has("hyprshot"), sys_has("hyprlock") || sys_has("swaylock"));

    printf(fails ? "RESULT: FAIL\n" : "RESULT: OK\n");
    return fails ? 1 : 0;
}

/* ---------------- main ---------------- */

typedef struct { App *a; char *name; } PageReq;

static gboolean show_page_idle(gpointer d) {
    PageReq *r = (PageReq *)d;
    if (!strcmp(r->name, "main")) show_main(r->a);
    else {
        const char *names[] = { "wifi", "bt", "sound", "display", "focus", "mirror", "airdrop" };
        for (int i = 0; i < NDETAIL; i++)
            if (!strcmp(r->name, names[i])) { show_detail(r->a, i); break; }
    }
    g_free(r->name);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void on_detail_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    (void)w;
    GtkWidget *frame = GTK_WIDGET(d);
    App *a = (App *)g_object_get_data(G_OBJECT(w), "app");
    if (a->modal_open) return;
    gint fx = 0, fy = 0;
    GtkWidget *win = gtk_widget_get_toplevel(frame);
    if (!gtk_widget_translate_coordinates(frame, win, 0, 0, &fx, &fy)) return;
    GtkAllocation al;
    gtk_widget_get_allocation(frame, &al);
    gint ox = 0, oy = 0;
    GdkWindow *ww = gtk_widget_get_window(win);
    if (ww) gdk_window_get_origin(ww, &ox, &oy);
    if (e->x_root >= ox + fx && e->x_root < ox + fx + al.width &&
        e->y_root >= oy + fy && e->y_root < oy + fy + al.height)
        return;
    quit_all();
}

static void on_catcher_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    (void)w; (void)e; (void)d;
    quit_all();
}

static void on_banner_press(GtkWidget *w, GdkEventButton *e, gpointer d) {
    (void)w; (void)e;
    App *a = (App *)d;
    if (a->banner_timer) { g_source_remove(a->banner_timer); a->banner_timer = 0; }
    gtk_widget_hide(a->banner_win);
}

/* Now Playing con controles */
static GtkWidget *build_np_card(App *a) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkStyleContext *sc = gtk_widget_get_style_context(card);
    gtk_style_context_add_class(sc, "pill");
    gtk_style_context_add_class(sc, "np");
    a->np_card = card;
    GtkWidget *art = sym("audio-x-generic-symbolic", "media-optical");
    gtk_widget_set_halign(art, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(card), art, FALSE, FALSE, 0);
    a->np_title = gtk_label_new("Not Playing");
    gtk_widget_set_halign(a->np_title, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(a->np_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(a->np_title), 16);
    GtkStyleContext *tc = gtk_widget_get_style_context(a->np_title);
    gtk_style_context_add_class(tc, "np-title");
    a->np_artist = gtk_label_new("");
    gtk_widget_set_halign(a->np_artist, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(a->np_artist), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(a->np_artist), 16);
    GtkStyleContext *ac = gtk_widget_get_style_context(a->np_artist);
    gtk_style_context_add_class(ac, "card-sub");
    gtk_box_pack_start(GTK_BOX(card), a->np_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), a->np_artist, FALSE, FALSE, 0);

    GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(ctl, GTK_ALIGN_CENTER);
    GtkWidget *prev = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(prev),
        sym("media-skip-backward-symbolic", "media-skip-backward"));
    GtkStyleContext *pvc = gtk_widget_get_style_context(prev);
    gtk_style_context_add_class(pvc, "media");
    g_signal_connect_swapped(prev, "clicked", G_CALLBACK(sys_player_cmd),
                             (gpointer)"previous");
    GtkWidget *play = gtk_button_new();
    a->np_play_icon = sym("media-playback-start-symbolic", "media-playback-start");
    gtk_button_set_image(GTK_BUTTON(play), a->np_play_icon);
    GtkStyleContext *plc = gtk_widget_get_style_context(play);
    gtk_style_context_add_class(plc, "media");
    gtk_style_context_add_class(plc, "play");
    g_signal_connect_swapped(play, "clicked", G_CALLBACK(sys_player_cmd),
                             (gpointer)"play-pause");
    GtkWidget *next = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(next),
        sym("media-skip-forward-symbolic", "media-skip-forward"));
    GtkStyleContext *nxc = gtk_widget_get_style_context(next);
    gtk_style_context_add_class(nxc, "media");
    g_signal_connect_swapped(next, "clicked", G_CALLBACK(sys_player_cmd),
                             (gpointer)"next");
    gtk_box_pack_start(GTK_BOX(ctl), prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl), play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl), next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), ctl, FALSE, FALSE, 0);
    return card;
}

int main(int argc, char **argv) {
    const char *page_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) {
            const char *css = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : NULL;
            gtk_init_check(&argc, &argv);
            return run_check(css);
        }
        if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            page_arg = argv[++i];
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Uso: control-center [--check [style.css]] [--page NOMBRE]\n");
            return 0;
        }
    }
    const char *dbg = g_getenv("CC_PAGE");
    if (dbg && *dbg) page_arg = dbg;

    gtk_init(&argc, &argv);
    load_css();
    t0_global = g_get_monotonic_time();
    fprintf(stderr, "START t=0ms\n");

    App app = { 0 };
    App *a = &app;
    a->airdrop_mode = AIRDROP_EVERYONE;

    /* catcher fullscreen sin cristal: come clics fuera y cierra */
    a->catcher_win = layer_win_full(a, "Control Center", CATCHER_ID, 1,
                                    GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    {
        GtkWidget *catcher = gtk_event_box_new();
        GtkStyleContext *csc = gtk_widget_get_style_context(catcher);
        gtk_style_context_add_class(csc, "cc-catcher");
        gtk_container_add(GTK_CONTAINER(a->catcher_win), catcher);
        g_signal_connect(catcher, "button-press-event",
                         G_CALLBACK(on_catcher_press), a);
        gtk_widget_show_all(a->catcher_win);
    }

    /* banner flotante (tira superior, input recortado a la píldora) */
    a->banner_win = layer_win_full(a, "Control Center", APP_ID, 0,
                                   GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor(GTK_WINDOW(a->banner_win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(a->banner_win), GTK_LAYER_SHELL_EDGE_TOP, 100);
    {
        GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_valign(hb, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(a->banner_win), hb);
        GtkWidget *sp1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp1, TRUE);
        gtk_box_pack_start(GTK_BOX(hb), sp1, TRUE, TRUE, 0);
        GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkStyleContext *bsc = gtk_widget_get_style_context(pill);
        gtk_style_context_add_class(bsc, "banner");
        GtkWidget *bdot = sym("video-display-symbolic", "computer");
        gtk_box_pack_start(GTK_BOX(pill), bdot, FALSE, FALSE, 0);
        a->banner_label = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(pill), GTK_WIDGET(a->banner_label), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hb), pill, FALSE, FALSE, 0);
        GtkWidget *sp2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(sp2, TRUE);
        gtk_box_pack_start(GTK_BOX(hb), sp2, TRUE, TRUE, 0);
        a->banner_pill = pill;
        g_object_set_data(G_OBJECT(a->banner_win), "app", a);
        g_signal_connect(a->banner_win, "button-press-event",
                         G_CALLBACK(on_banner_press), a);
    }

    /* 15 ventanas-botón ajustadas a su tarjeta */
    {
        GtkWidget *cards[NSLOT];
        cards[S_WIFI] = pill_card(a, "network-wireless-symbolic", "network-wireless",
                                  "Wi-Fi", "…", &a->wifi_icon, &a->wifi_title,
                                  &a->wifi_sub, G_CALLBACK(wifi_toggle),
                                  G_CALLBACK(open_WIFI));
        cards[S_NP] = build_np_card(a);
        cards[S_BT] = pill_card(a, "bluetooth-active-symbolic", "bluetooth",
                                "Bluetooth", "…", &a->bt_icon, &a->bt_title,
                                &a->bt_sub, G_CALLBACK(bt_toggle),
                                G_CALLBACK(open_BT));
        cards[S_AD] = pill_card(a, "document-send-symbolic", "network-transmit-receive",
                                "AirDrop", "…", &a->ad_icon, &a->ad_title,
                                &a->ad_sub, G_CALLBACK(airdrop_cycle),
                                G_CALLBACK(open_AD));
        cards[S_STAGE] = round_btn(a, "view-dual-symbolic", "view-grid",
                                   "Stage Manager (Overview)",
                                   G_CALLBACK(stage_toggle), NULL);
        cards[S_MIRROR] = round_btn(a, "video-display-symbolic", "computer",
                                    "Duplicar pantalla", G_CALLBACK(open_MIRROR), NULL);
        cards[S_DND] = round_btn(a, "alarm-symbolic", "preferences-desktop-notifications",
                                 "No molestar", G_CALLBACK(dnd_toggle), &a->dnd_icon);
        cards[S_MUTE] = round_btn(a, "audio-volume-muted-symbolic", "audio-input-microphone",
                                  "Silenciar", G_CALLBACK(mute_toggle_btn), &a->mute_icon);
        cards[S_FOCUS] = pill_card(a, "weather-clear-night-symbolic", "night-light",
                                   "Focus", "Focus", &a->focus_icon, NULL,
                                   &a->focus_sub, G_CALLBACK(dnd_toggle),
                                   G_CALLBACK(open_FOCUS));
        cards[S_DISPLAY] = slider_card(a, "Display",
            "display-brightness-low-symbolic", "display-brightness-symbolic",
            &a->display_scale, G_CALLBACK(on_display_changed),
            "display-brightness-symbolic", G_CALLBACK(open_DISPLAY), NULL);
        cards[S_SOUND] = slider_card(a, "Sound",
            "audio-volume-low-symbolic", "audio-volume-high-symbolic",
            &a->sound_scale, G_CALLBACK(on_sound_changed),
            "audio-speakers-symbolic", G_CALLBACK(open_SOUND), &a->sound_mute_icon);
        cards[S_DARK] = round_btn(a, "weather-clear-night-symbolic", "night-light",
                                  "Modo oscuro", G_CALLBACK(on_dark), &a->dark_icon);
        cards[S_CALC] = round_btn(a, "accessories-calculator-symbolic", "input-dialpad",
                                  "Calculadora", G_CALLBACK(on_calc), NULL);
        cards[S_SHOT] = round_btn(a, "camera-photo-symbolic", "applets-screenshooter",
                                  "Captura de pantalla", G_CALLBACK(on_shot), NULL);
        cards[S_LOCK] = round_btn(a, "system-lock-screen-symbolic", "changes-prevent",
                                  "Bloquear sesión", G_CALLBACK(on_lock), NULL);

        const char *titles[NSLOT] = {
            "Wi-Fi", "Now Playing", "Bluetooth", "AirDrop", "Stage Manager",
            "Duplicar pantalla", "No molestar", "Silenciar", "Focus", "Display",
            "Sound", "Modo oscuro", "Calculadora", "Captura", "Bloquear"
        };
        for (int k = 0; k < NSLOT; k++) {
            GtkWidget *win = layer_win(a, titles[k]);
            stack_place(win);
            gtk_widget_set_size_request(cards[k], GEOMS[k].w, GEOMS[k].h);
            gtk_container_add(GTK_CONTAINER(win), cards[k]);
            a->btn_wins[k] = win;
            g_signal_connect(win, "map-event", G_CALLBACK(on_first_map), a);
        }
        /* mostrar cuando el contador está armado: invisibles hasta estar
         * las 15 mapeadas; el revelado conjunto + entrada lo hace reveal_tick
         * (más fallback por si alguna tarda) */
        a->awaiting_maps = NSLOT;
        for (int k = 0; k < NSLOT; k++) {
            gtk_widget_set_opacity(a->btn_wins[k], 0.0);
            gtk_widget_set_sensitive(a->btn_wins[k], FALSE);
            gtk_widget_show_all(a->btn_wins[k]);
        }
        g_timeout_add(1500, map_fallback, a);
        /* botones cuyo estado visual se actualiza en el poll */
        a->dnd_btn = cards[S_DND];
        a->mute_btn = cards[S_MUTE];
        a->dark_btn = cards[S_DARK];
        a->wifi_pill = cards[S_WIFI];
        a->bt_pill = cards[S_BT];
        a->ad_pill = cards[S_AD];
        a->focus_pill = cards[S_FOCUS];
    }

    /* 7 ventanas-detalle ajustadas al contenido */
    {
        GtkWidget *contents[NDETAIL];
        contents[D_WIFI] = build_wifi(a);
        contents[D_BT] = build_bt(a);
        contents[D_SOUND] = build_sound(a);
        contents[D_DISPLAY] = build_display(a);
        contents[D_FOCUS] = build_focus(a);
        contents[D_MIRROR] = build_mirror(a);
        contents[D_AD] = build_airdrop(a);
        const char *titles[NDETAIL] = {
            "Wi-Fi", "Bluetooth", "Sound", "Display",
            "Focus", "Duplicar pantalla", "AirDrop"
        };
        for (int i = 0; i < NDETAIL; i++) {
            GtkWidget *win = layer_win(a, titles[i]);
            GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            GtkStyleContext *fsc = gtk_widget_get_style_context(frame);
            gtk_style_context_add_class(fsc, "cc-root");
            gtk_widget_set_size_request(frame, 396, -1);
            gtk_container_add(GTK_CONTAINER(win), frame);
            gtk_box_pack_start(GTK_BOX(frame), contents[i], TRUE, TRUE, 0);
            a->detail_wins[i] = win;
            a->detail_frames[i] = frame;
            g_object_set_data(G_OBJECT(win), "app", a);
            g_signal_connect(win, "button-press-event",
                             G_CALLBACK(on_detail_press), frame);
            gtk_widget_show_all(win);
            gtk_widget_hide(win);
        }
    }

    /* estado instantáneo desde caché; la entrada animada arranca al
     * mapear la primera ventana (on_first_map); el poll corrige después */
    cache_load(a);
    tick(a);
    g_timeout_add_seconds(2, tick, a);
    g_timeout_add(600, make_ready, a);
    if (page_arg) {
        PageReq *r = g_new0(PageReq, 1);
        r->a = a; r->name = g_strdup(page_arg);
        g_idle_add(show_page_idle, r);
    }

    gtk_main();
    return 0;
}
