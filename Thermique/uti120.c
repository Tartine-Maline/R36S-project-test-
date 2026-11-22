# 1. Insère la carte SD dans ton PC.
# 2. Ouvre un terminal (Linux/macOS/WSL/PowerShell).
# 3. Adapte ce chemin vers le point de montage de ta partition ROMS/SHARE :
#    Linux:   /media/$USER/ROMS
#    macOS:   /Volumes/ROMS
#    WSL:     /mnt/d   (ou /mnt/e, etc.)
#    Windows PowerShell: D:  (lettre de ta carte SD)
# Puis lance ce bloc ENTIER (copie-colle + Entrée) :

set -e
SD_MOUNT="/media/$USER/ROMS"  # <--- MODIFIE CE CHEMIN SI BESOIN
PORT_DIR="$SD_MOUNT/ports/Thermique"

echo "📁 Création de l'arborescence dans $PORT_DIR ..."
mkdir -p "$PORT_DIR"
cd "$PORT_DIR"

# ============================================================
# 1. THERMIQUE.SH (Lanceur optimisé)
# ============================================================
cat > Thermique.sh << 'EOF_THERMIQUE_SH'
#!/bin/bash
# Camera thermique UNI-T UTi120Mobile - R36S / dArkOS (Version 3)
# Place ce script dans le dossier "ports" avec uti120.c (même dossier).
# Réglages, captures et journaux : dossier uti120_out/ (à côté de ce script).
# Rendu logiciel (si l'affichage montre des défauts) : créer un fichier vide
#   uti120_out/rendu_logiciel

set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/uti120_out"
BIN="/tmp/uti120"
mkdir -p "$OUT"

# Rotation logs
[ -f "$OUT/log_affichage.txt" ] && mv -f "$OUT/log_affichage.txt" "$OUT/log_precedent.txt"
rm -f "$OUT/build.txt" "$OUT/dmesg_usb.txt"

# 1. Compilation optimisée (quelques secondes)
# -Ofast + auto-vectorisation NEON (armv8-a+simd) + LTO
SDLF="$(pkg-config --cflags --libs sdl2 2>/dev/null || sdl2-config --cflags --libs 2>/dev/null)"
[ -z "$SDLF" ] && SDLF="-I/usr/include/SDL2 -lSDL2"
USBLIB="$(pkg-config --libs libusb-1.0 2>/dev/null || echo "-l:libusb-1.0.so.0")"

{
  echo "== compilation $(date) =="
  gcc -Ofast -march=armv8-a+simd -flto -pthread \
      -o "$BIN" "$DIR/uti120.c" \
      $SDLF $USBLIB -lm \
      -Wno-unused-parameter -Wno-unused-variable \
      2>&1
  echo "gcc exit: $?"
} > "$OUT/build.txt" 2>&1

[ -x "$BIN" ] || { cat "$OUT/build.txt"; exit 1; }

# 2. Droits d'accès à la camera USB (nécessite sudo NOPASSWD pour chmod/dmesg)
sudo chmod a+rw /dev/bus/usb/*/* 2>/dev/null

# 3. Application
OPT=""
[ -f "$OUT/rendu_logiciel" ] && OPT="-s"

"$BIN" -r 1 $OPT -d "$OUT" -l "$OUT/log_affichage.txt"
RC=$?

echo "code de retour: $RC" >> "$OUT/log_affichage.txt"
if [ "$RC" -ne 0 ]; then
  {
    echo "== messages noyau USB =="
    sudo dmesg -T 2>&1 | grep -i -E "usb|dwc2|otg|vbus|over-current" | tail -n 30
  } > "$OUT/dmesg_usb.txt" 2>&1
fi
sync
EOF_THERMIQUE_SH

chmod +x Thermique.sh

# ============================================================
# 2. UTI120.C (Application complète, C99 pur, sans lambdas, ordre correct)
# ============================================================
cat > uti120.c << 'END_OF_UTI120_C'
/*
 * uti120.c - UNI-T UTi120Mobile thermal camera for R36S / dArkOS
 * Version 3.4: Pure C99, no lambdas, correct declaration order, 60 FPS target.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

/* --- libusb minimal prototypes --- */
typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;
extern int libusb_init(libusb_context **ctx);
extern void libusb_exit(libusb_context *ctx);
extern libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *ctx, uint16_t vid, uint16_t pid);
extern void libusb_close(libusb_device_handle *h);
extern int libusb_claim_interface(libusb_device_handle *h, int n);
extern int libusb_release_interface(libusb_device_handle *h, int n);
extern int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *transferred, unsigned int timeout);
extern int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *transferred, unsigned int timeout);
extern int libusb_kernel_driver_active(libusb_device_handle *h, int n);
extern int libusb_detach_kernel_driver(libusb_device_handle *h, int n);
extern const char *libusb_strerror(int errcode);

/* ============ CONSTANTES ============ */
#define W 120
#define H 90
#define NPIX (W * H)
#define HQ 4
#define HW (W * HQ)
#define HH (H * HQ)
#define FRAME_BYTES 25600
#define MAGIC 0xAA55
#define IMG_OFFSET (W * 3)
#define NPAL 6
#define NACT_COUNT 25
#define NESSENTIAL 12
#define ID_AXIS 100
#define ID_HAT 200
#define NID 256
#define TREND_N 240
#define FONT_COLS 13
#define NGLYPH 65

/* ============ ENUMS ============ */
typedef enum {
    A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK, A_MENU, A_CAPTURE, A_PALETTE,
    A_ZOOM_IN, A_ZOOM_OUT, A_FREEZE, A_RECAL, A_HUD, A_RANGE, A_CENTER, A_REF,
    A_MEASURE, A_HELP, A_GALLERY, A_LEVEL_UP, A_LEVEL_DOWN, A_SPAN_UP,
    A_SPAN_DOWN, A_PAL_PREV
} Action;

typedef enum { S_VIEW, S_MENU, S_GALLERY, S_WIZARD, S_BIND, S_CALIB, S_INFO, S_HELP, S_PADTEST } State;
typedef enum { MT_ACTION, MT_CHOICE, MT_FLOAT, MT_INT, MT_SUB, MT_BIND } MenuType;
typedef enum { PG_MAIN, PG_IMAGE, PG_MEAS, PG_TEMP, PG_CAM, PG_CAPS, PG_UI, PG_PAD, PG_BTN, PG_COUNT } PageId;
typedef enum {
    M_RESUME = 1, M_CAPTURE, M_GALLERY, M_INFO, M_QUIT, M_RECAL, M_NUC,
    M_CAL1, M_CAL2, M_CALRESET, M_WIZARD, M_BTNDEF, M_HELP, M_PADTEST,
    M_DEFAULTS, M_RECONNECT, M_REF
} MenuAction;
typedef enum { CB_MAT = 1, CB_EMIS } CallbackId;
typedef enum { CMD_NONE = 0, CMD_RECAL, CMD_NUC, CMD_REGS } CamCmd;

/* ============ STRUCTURES ============ */
typedef struct {
    int palette, pal_inv, quality, range_mode;
    float man_min, man_max;
    int zoom, tfilter, mirror, flip, deadpix;
    int hud, ui_big, marks, crosshair, colorbar, show_clock, show_batt, graph;
    int meas_mode, zone;
    int alarm_mode;
    float alarm_hi, alarm_lo;
    int fahrenheit, cap_fmt, cap_csv, cap_txt;
    float emis, tamb, gain, bias;
    int material, tref_src, autocal, nuc_start;
    int ax_lx, ax_ly, ax_rx, ax_ry, deadzone, stick_speed, invert_y, rstick_mode, analog_cursor;
    int bind[NACT_COUNT];
} Cfg;

typedef struct { const char *key; void *p; int isf; } CE;
typedef struct {
    const char *label;
    int type;
    void *val;
    float mn, mx, st;
    const char *const *opts;
    int nopt;
    int arg;
} MItem;
typedef struct { const char *title; MItem *items; int n; } Page;
typedef struct { float v[NPIX]; uint32_t seq; uint16_t counter; float twcal[4]; long ts; } Shared;

/* ============ GLOBALES ============ */
static FILE *logf_ = NULL;
static const char *outdir = ".";
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t logmtx = PTHREAD_MUTEX_INITIALIZER;

static SDL_Window *win = NULL;
static SDL_Renderer *ren = NULL;
static SDL_Texture *tex_s = NULL, *tex_hd = NULL, *font_tex = NULL;
static int OW = 640, OH = 480;

static libusb_context *ctx = NULL;
static libusb_device_handle *dev = NULL;
static int pad_mode = 1;
static volatile int gone = 0;
static int rd_timeout = 300;
static int do_nuc = 1;
static int cmd_to = 500;
static uint8_t last_reply[64];
static int last_len = 0;
static int req_mode = 1;

static uint16_t raw16[FRAME_BYTES / 2];
static int dumped = 0, badmagic = 0, resync_count = 0;

static float off_[NPIX];
static uint8_t bad_[NPIX];
static int use_bad = 0;
static float vmap[NPIX];
static int iir_valid = 0;
static float tw_cal[4];

static float vui[NPIX];
static float tmap[NPIX];
static float tw_ui[4];
static uint32_t ui_seq = 0;
static uint16_t ui_counter = 0;
static long last_frame_ms = 0;
static float t_min = 0, t_max = 0, t_avg = 0;
static int i_min = 0, i_max = 0;

static float temp_K1 = 1.0f, temp_K2 = 0.0f;
static int temp_lut_ver = -1;

static uint8_t pal[NPAL][256][3];
static const char *const pal_names[NPAL] = {"IRONBOW", "ARC-EN-CIEL", "GRIS", "METAL CHAUD", "BLEU-ROUGE", "PLASMA"};
static const char *pal_opts[NPAL];

static float lo_t = 15, hi_t = 35, lock_lo = 15, lock_hi = 35;
static int range_init = 0, prev_range = 0;
static const char *const range_names[3] = {"AUTO", "MANUEL", "FIGE"};

static uint32_t argb_s[NPIX];
static uint32_t argb_hd[HW * HH];

static int spot_dx = W / 2, spot_dy = H / 2;
static float spot_fx = W / 2, spot_fy = H / 2;
static const int zooms[4] = {1, 2, 3, 4};

static float trend[TREND_N];
static int trend_n = 0;

static char capdir[600];
static int cap_index = 1, cap_pending = 0;
static uint32_t *cap_screen_buf = NULL;
static int cap_screen_w = 0, cap_screen_h = 0;

static char toast_msg[128];
static long toast_until = 0;

static State state = S_VIEW;

static char gal_name[512][48];
static int gal_n = 0, gal_i = 0, gal_confirm = 0;
static SDL_Texture *gal_tex = NULL;
static int gal_w = 0, gal_h = 0;

static MItem pg_main[15], pg_image[14], pg_meas[7], pg_temp[9], pg_cam[5], pg_caps[4], pg_ui[5], pg_pad[10], pg_btn[2 + NACT_COUNT];
static Page pages[PG_COUNT];
static int menu_page = PG_MAIN, menu_sel[PG_COUNT], menu_top[PG_COUNT];
static int wiz_idx = 0, bind_act = 0;
static float calib_ref = 33.0f;
static int calib_point = 1;

static int ref_on = 0;
static float ref_t = 0.0f;
static int cal_p1_valid = 0;
static float cal_p1_v = 0.0f, cal_p1_a = 0.0f;
static long last_render = 0;

static SDL_Joystick *joy = NULL;
static long held[NID], hold_next[NID];
static int held_act[NID];
static int axis_rest[8], axis_raw[8], axis_dir[8][2], hat_prev = 0;
static int pad_pressed[NID];
static char pad_last[48] = "---";
static long pad_back_t = 0;
static int axis_logged = 0;

static int fps_now = 0, batt_pct = -1;
static long batt_t = 0;

static volatile int cam_run = 1, cam_cmd = 0, cam_conn = 1, cal_active = 0, calibrated = 0;
static volatile float cal_prog = 0;
static const char *volatile cal_label = "CALIBRATION";
static volatile int regs_ok = 0, regs_ready = 0;
static uint32_t regs_val[4];
static char cam_msg[96];
static volatile int cam_msg_new = 0;
static volatile long frames_cam = 0, stalls = 0, reconnects = 0, max_gap = 0;

static const float tf_alpha[4] = {1.0f, 0.6f, 0.4f, 0.25f};
static volatile int cam_tfilter = 1, cam_deadpix = 1;

static int run = 1;
static int frozen = 0;

static Cfg C;
static int cfg_bound = 0;
static volatile int cfg_ver = 1;
static Shared sh;

static const uint8_t font5x7[NGLYPH][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x06,0x09,0x09,0x06}
};

static const char *act_name[NACT_COUNT] = {
    "HAUT", "BAS", "GAUCHE", "DROITE", "OK / VALIDER", "RETOUR", "MENU", "CAPTURE", "PALETTE +", "ZOOM +", "ZOOM -",
    "GELER IMAGE", "RECALIBRER", "AFFICHAGE", "PLAGE AUTO", "VISEUR CENTRE", "REFERENCE", "MESURE", "AIDE", "GALERIE",
    "NIVEAU +", "NIVEAU -", "CONTRASTE +", "CONTRASTE -", "PALETTE -"};
static const char *act_desc[NACT_COUNT] = {
    "DEPLACE LE VISEUR / NAVIGUE", "DEPLACE LE VISEUR / NAVIGUE", "DEPLACE LE VISEUR / REGLE", "DEPLACE LE VISEUR / REGLE",
    "VALIDE / OUVRE LE MENU", "ANNULE / RETOUR (2 FOIS = QUITTER)", "OUVRE LE MENU", "ENREGISTRE UNE CAPTURE",
    "PALETTE DE COULEURS SUIVANTE", "ZOOM AVANT SUR LE VISEUR", "ZOOM ARRIERE", "FIGE / REPREND L'IMAGE",
    "RECALIBRATION RAPIDE", "COMPLET / MINIMAL / AUCUN", "AUTO / MANUEL / FIGE", "REMET LE VISEUR AU CENTRE",
    "TEMPERATURE DE REFERENCE (DELTA)", "POINT / ZONE", "AFFICHE L'AIDE", "OUVRE LA GALERIE",
    "DECALE LA PLAGE VERS LE HAUT", "DECALE LA PLAGE VERS LE BAS", "PLAGE PLUS ETROITE", "PLAGE PLUS LARGE", "PALETTE PRECEDENTE"};
static const char *act_key[NACT_COUNT] = {"up", "down", "left", "right", "ok", "back", "menu", "capture", "palette", "zoom_in",
                                    "zoom_out", "freeze", "recal", "hud", "range", "center", "ref", "measure", "help",
                                    "gallery", "level_up", "level_down", "span_up", "span_down", "pal_prev"};

static const char *const o_onoff[] = {"NON", "OUI"};
static const char *const o_range[] = {"AUTO", "MANUEL", "FIGEE"};
static const char *const o_zoom[] = {"X1", "X2", "X3", "X4"};
static const char *const o_qual[] = {"PIXELS", "DOUX", "HD"};
static const char *const o_tf[] = {"AUCUN", "FAIBLE", "MOYEN", "FORT"};
static const char *const o_hud[] = {"COMPLET", "MINIMAL", "AUCUN"};
static const char *const o_unit[] = {"CELSIUS", "FAHRENHEIT"};
static const char *const o_auto[] = {"NON", "1 MIN", "3 MIN", "5 MIN", "10 MIN"};
static const char *const o_src[] = {"MOT 8", "MOT 9", "MOT 10", "MOT 11"};
static const char *const o_alarm[] = {"AUCUNE", "AU-DESSUS", "EN-DESSOUS", "ENTRE"};
static const char *const o_meas[] = {"POINT", "ZONE"};
static const char *const o_marks[] = {"AUCUN", "MARQUES", "ETIQUETTES"};
static const char *const o_cap[] = {"ECRAN", "IMAGE", "LES DEUX"};
static const char *const o_text[] = {"NORMAL", "GRAND"};
static const char *const o_rst[] = {"ZOOM/PALETTE", "NIVEAU/CONTR.", "AUCUN"};
static const char *const o_mat[] = {"PERSO", "PEAU", "EAU", "PEINTURE", "PLASTIQUE", "BOIS", "BETON", "VERRE", "CUIVRE OXY", "ALU OXYDE", "ACIER POLI"};
static const float mat_e[] = {0, 0.98f, 0.96f, 0.95f, 0.94f, 0.90f, 0.93f, 0.92f, 0.78f, 0.30f, 0.10f};

/* ============ FORWARD DECLARATIONS ============ */
static void zone_stats(float *mn, float *mx, float *avg);
static void find_bad(void);
static int cmp_float(const void *a, const void *b);
static int wait_ready(int max_ms, int need, float p0, float p1);
static void progress(float f);
static int calibrate(int with_nuc);
static int next_frame(void);
static void camera_reconnect(void);
static void *cam_thread(void *arg);
static void rebuild_temp_coeffs(void);
static void compute_temps(void);
static void update_range(void);
static void build_small(void);
static void build_hd(void);
static int grab_valid(void);
static int grab_frame(void);
static void drain_bulk(void);
static int read_reg(uint8_t func, uint8_t reg, uint32_t *val);
static int send_cmd(uint8_t func, uint8_t reg, const uint32_t *vals, int count);
static int send_request(void);
static int usb_open(void);
static void usb_close(void);
static int note(int r);
static void menu_do(int id);
static void menu_close(void);
static void ref_toggle(void);
static void capture_input(int id);
static void dispatch(int a);
static void view_input(int act);
static void calib_input(int act);
static void gallery_input(int act);
static void input_down(int id);
static void input_up(int id);
static void axis_event(int axis, int value);
static void hat_event(int value);
static void process_sticks(long now);
static void render(long now);
static void rebuild_image(void);
static void apply_scale_mode(void);
static int key_to_action(int sym);
static void font_init(void);
static void pal_init(void);
static void capture_init(void);
static void gallery_scan(void);
static void gallery_load(void);
static void gallery_delete(void);
static void menu_init(void);
static void cfg_save(void);
static void cfg_load(void);
static void cfg_defaults(void);
static void cfg_sanitize(void);
static void item_value(const MItem *it, char *buf, size_t n);
static void menu_cb(int id);
static void item_adjust(const MItem *it, int dir);
static int menu_rows(void);
static void menu_input(int act);
static int default_nav(int id);
static int repeat_interval(int act);
static int find_action(int id);
static void spot_set(int x, int y);
static float spot_temp(void);
static float spot_v(void);
static float current_value(void);
static void zone_rect(int *x0, int *y0, int *x1, int *y1);
static void trend_push(float v);
static void draw_trend(int x, int y, int w, int h);
static void battery_update(long now);
static void draw_hud(const SDL_Rect *src, const SDL_Rect *dst);
static void draw_toast(int y);
static void draw_toast_top(void);
static void draw_panel(int x, int y, int w, int h, const char *title);
static void draw_menu(void);
static void draw_calib(void);
static void draw_info(void);
static void draw_help(void);
static void draw_padtest(void);
static void draw_gallery(void);
static void draw_wizard(void);
static void draw_calib_overlay(void);
static void draw_image_layer(void);
static int save_bmp(const char *path, const uint32_t *px, int w, int h);
static void capture_init(void);
static void export_csv(const char *path);
static void export_txt(const char *path);
static void save_clean(const char *path);
static void do_capture(void);
static int cmp_name(const void *a, const void *b);
static void gallery_load(void);
static void gallery_scan(void);
static void gallery_delete(void);
static int cmp_float(const void *a, const void *b);

/* ============ INLINE HELPERS ============ */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int sc_s(void) { return C.ui_big ? 2 : 1; }
static inline int sc_b(void) { return C.ui_big ? 3 : 2; }
static inline int sc_n(void) { return C.ui_big ? 4 : 3; }
static inline int glyph(int c) { c = toupper((unsigned char)c); if (c == '~') return 64; if (c < 32 || c > 95) return 0; return c - 32; }
static inline int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc; }
static inline int disp2data(int dx, int dy) { return (C.flip ? H - 1 - dy : dy) * W + (C.mirror ? W - 1 - dx : dx); }
static inline int data2dispx(int i) { int x = i % W; return C.mirror ? W - 1 - x : x; }
static inline int data2dispy(int i) { int y = i / W; return C.flip ? H - 1 - y : y; }
static inline uint32_t color_of(float t, float sc) { int k = clampi((int)((t - lo_t) * sc), 0, 255); if (C.pal_inv) k = 255 - k; const uint8_t *c = pal[C.palette][k]; uint32_t col = 0xFF000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]; switch (C.alarm_mode) { case 1: if (t >= C.alarm_hi) col = 0xFFFF00FFu; break; case 2: if (t <= C.alarm_lo) col = 0xFF00FFFFu; break; case 3: if (t >= C.alarm_lo && t <= C.alarm_hi) col = 0xFF00FF00u; break; } return col; }
static inline SDL_Texture *cur_tex(void) { return C.quality == 2 ? tex_hd : tex_s; }
static long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; }
static void LOG(const char *fmt, ...) { if (!logf_) return; va_list ap; va_start(ap, fmt); pthread_mutex_lock(&logmtx); vfprintf(logf_, fmt, ap); fputc('\n', logf_); fflush(logf_); pthread_mutex_unlock(&logmtx); va_end(ap); }
static void toast(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(toast_msg, sizeof toast_msg, fmt, ap); va_end(ap); toast_until = now_ms() + 2400; }
static void cam_post(const char *m) { pthread_mutex_lock(&mtx); snprintf(cam_msg, sizeof cam_msg, "%s", m); cam_msg_new = 1; pthread_mutex_unlock(&mtx); }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void input_name(int id, char *b, size_t n) { static const char *hn[4] = {"HAUT", "DROITE", "BAS", "GAUCHE"}; if (id < 0) snprintf(b, n, "---"); else if (id < 64) snprintf(b, n, "BOUTON %d", id); else if (id >= ID_AXIS && id < ID_AXIS + 16) snprintf(b, n, "AXE %d %c", (id - ID_AXIS) / 2, ((id - ID_AXIS) & 1) ? '+' : '-'); else if (id >= ID_HAT && id < ID_HAT + 4) snprintf(b, n, "CROIX %s", hn[id - ID_HAT]); else snprintf(b, n, "?%d", id); }
static int find_action(int id) { for (int a = 0; a < NACT_COUNT; a++) if (C.bind[a] == id) return a; return -1; }
static float axis_norm(int a) { if (a < 0 || a > 7) return 0; float d = (float)(axis_raw[a] - axis_rest[a]) / 32767.0f; return clampf(d, -1.0f, 1.0f); }
static float stick_val(int a) { float d = axis_norm(a), dz = C.deadzone / 100.0f; if (fabsf(d) < dz) return 0.0f; float v = (fabsf(d) - dz) / (1.0f - dz); return d < 0 ? -v : v; }
static int default_nav(int id) { static const int m[4] = {A_UP, A_RIGHT, A_DOWN, A_LEFT}; if (id >= ID_HAT && id < ID_HAT + 4) return m[id - ID_HAT]; if (id >= ID_AXIS && id < ID_AXIS + 16 && state != S_VIEW) { int ax = (id - ID_AXIS) / 2, pos = (id - ID_AXIS) & 1; if (ax == C.ax_lx) return pos ? A_RIGHT : A_LEFT; if (ax == C.ax_ly) return (pos ^ C.invert_y) ? A_DOWN : A_UP; } return -1; }
static int repeat_interval(int act) { if (act >= A_UP && act <= A_RIGHT) return state == S_VIEW ? 60 : 130; if (act == A_LEVEL_UP || act == A_LEVEL_DOWN || act == A_SPAN_UP || act == A_SPAN_DOWN) return 60; if (act == A_ZOOM_IN || act == A_ZOOM_OUT) return 280; return 0; }
static void view_src(SDL_Rect *src) { int z = zooms[C.zoom], w = W / z, h = H / z; int x = clampi(spot_dx - w / 2, 0, W - w), y = clampi(spot_dy - h / 2, 0, H - h); *src = (SDL_Rect){x, y, w, h}; }
static void img2scr(int ix, int iy, const SDL_Rect *src, const SDL_Rect *dst, int *sx, int *sy, int *inside) { *inside = ix >= src->x && ix < src->x + src->w && iy >= src->y && iy < src->y + src->h; *sx = dst->x + (int)((ix - src->x + 0.5f) * dst->w / src->w); *sy = dst->y + (int)((iy - src->y + 0.5f) * dst->h / src->h); }
static float spot_temp(void) { double s = 0; int c = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int x = spot_dx + dx, y = spot_dy + dy; if (x < 0 || x >= W || y < 0 || y >= H) continue; s += tmap[disp2data(x, y)]; c++; } return c ? (float)(s / c) : 0.0f; }
static float spot_v(void) { double s = 0; int c = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int x = spot_dx + dx, y = spot_dy + dy; if (x < 0 || x >= W || y < 0 || y >= H) continue; s += vui[disp2data(x, y)]; c++; } return c ? (float)(s / c) : 0.0f; }
static float current_value(void) { if (C.meas_mode == 1) { float a, b, c; zone_stats(&a, &b, &c); return c; } return spot_temp(); }
static void zone_rect(int *x0, int *y0, int *x1, int *y1) { int hz = C.zone / 2; *x0 = clampi(spot_dx - hz, 0, W - 1); *x1 = clampi(spot_dx + hz, 0, W - 1); *y0 = clampi(spot_dy - hz, 0, H - 1); *y1 = clampi(spot_dy + hz, 0, H - 1); }
static void zone_stats(float *mn, float *mx, float *avg) { int x0, y0, x1, y1; zone_rect(&x0, &y0, &x1, &y1); double s = 0; int c = 0; *mn = 1e30f; *mx = -1e30f; for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { float t = tmap[disp2data(x, y)]; s += t; c++; if (t < *mn) *mn = t; if (t > *mx) *mx = t; } *avg = c ? (float)(s / c) : 0.0f; }
static void trend_push(float v) { if (trend_n < TREND_N) trend[trend_n++] = v; else { memmove(trend, trend + 1, (TREND_N - 1) * sizeof(float)); trend[TREND_N - 1] = v; } }

/* ============ FONT ============ */
static void font_init(void) {
    static uint32_t px[FONT_COLS * 6 * 5 * 8]; memset(px, 0, sizeof px);
    for (int g = 0; g < NGLYPH; g++) { int gx = (g % FONT_COLS) * 6, gy = (g / FONT_COLS) * 8; for (int c = 0; c < 5; c++) for (int r = 0; r < 7; r++) if ((font5x7[g][c] >> r) & 1) px[(gy + r) * (FONT_COLS * 6) + gx + c] = 0xFFFFFFFFu; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); font_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, FONT_COLS * 6, 5 * 8);
    if (font_tex) { SDL_UpdateTexture(font_tex, NULL, px, FONT_COLS * 6 * 4); SDL_SetTextureBlendMode(font_tex, SDL_BLENDMODE_BLEND); }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
}

static void fillr(int x, int y, int w, int h, int r, int g, int b, int a) { SDL_SetRenderDrawBlendMode(ren, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE); SDL_SetRenderDrawColor(ren, r, g, b, a); SDL_Rect q = {x, y, w, h}; SDL_RenderFillRect(ren, &q); }
static void rectr(int x, int y, int w, int h, int r, int g, int b) { SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE); SDL_SetRenderDrawColor(ren, r, g, b, 255); SDL_Rect q = {x, y, w, h}; SDL_RenderDrawRect(ren, &q); }
static void liner(int x0, int y0, int x1, int y1, int r, int g, int b) { SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE); SDL_SetRenderDrawColor(ren, r, g, b, 255); SDL_RenderDrawLine(ren, x0, y0, x1, y1); }
static void txt(int x, int y, int sc, const char *s, int r, int g, int b) { if (!font_tex) return; int o = sc > 1 ? sc / 2 + 1 : 1; SDL_SetTextureColorMod(font_tex, r, g, b); for (; *s; s++, x += 6 * sc) { int gi = glyph(*s); if (gi == 0) continue; SDL_Rect src = {(gi % FONT_COLS) * 6, (gi / FONT_COLS) * 8, 6, 8}; SDL_Rect dst = {x + o, y + o, 6 * sc, 8 * sc}; SDL_RenderCopy(ren, font_tex, &src, &dst); dst.x = x; dst.y = y; SDL_RenderCopy(ren, font_tex, &src, &dst); } }
static void txt_c(int y, int sc, const char *s, int r, int g, int b) { txt((OW - text_w(s, sc)) / 2, y, sc, s, r, g, b); }
static void txt_r(int xr, int y, int sc, const char *s, int r, int g, int b) { txt(xr - text_w(s, sc), y, sc, s, r, g, b); }

/* ============ PALETTES ============ */
static void build_pal(int p, const float st[][4], int n) { for (int i = 0; i < 256; i++) { float t = i / 255.0f; int s = 0; while (s < n - 2 && t > st[s + 1][0]) s++; float a = clampf((t - st[s][0]) / (st[s + 1][0] - st[s][0]), 0, 1); for (int c = 0; c < 3; c++) pal[p][i][c] = (uint8_t)(st[s][c + 1] + a * (st[s + 1][c + 1] - st[s][c + 1])); } }
static void pal_init(void) { static const float st_iron[][4] = {{0.00f, 0, 0, 0}, {0.15f, 30, 0, 90}, {0.35f, 150, 0, 130}, {0.55f, 230, 50, 40}, {0.75f, 255, 150, 0}, {0.90f, 255, 230, 60}, {1.00f, 255, 255, 255}}; static const float st_rain[][4] = {{0.0f, 0, 0, 140}, {0.2f, 0, 110, 255}, {0.4f, 0, 230, 160}, {0.6f, 160, 255, 0}, {0.8f, 255, 160, 0}, {1.0f, 255, 0, 0}}; static const float st_grey[][4] = {{0.0f, 0, 0, 0}, {1.0f, 255, 255, 255}}; static const float st_hot[][4] = {{0.0f, 0, 0, 0}, {0.4f, 190, 0, 0}, {0.7f, 255, 200, 0}, {1.0f, 255, 255, 255}}; static const float st_br[][4] = {{0.0f, 0, 0, 160}, {0.25f, 0, 120, 255}, {0.5f, 255, 255, 255}, {0.75f, 255, 140, 0}, {1.0f, 200, 0, 0}}; static const float st_plasma[][4] = {{0.0f, 13, 8, 135}, {0.25f, 126, 3, 168}, {0.5f, 204, 71, 120}, {0.75f, 248, 149, 64}, {1.0f, 240, 249, 33}}; build_pal(0, st_iron, 7); build_pal(1, st_rain, 6); build_pal(2, st_grey, 2); build_pal(3, st_hot, 4); build_pal(4, st_br, 5); build_pal(5, st_plasma, 5); }

/* ============ TEMPERATURE MODEL ============ */
static void rebuild_temp_coeffs(void) { float emis_eff = C.emis < 0.1f ? 0.1f : C.emis; float tref = tw_ui[C.tref_src] / 100.0f; temp_K1 = C.gain / emis_eff; temp_K2 = C.tamb + (tref + C.bias - C.tamb) / emis_eff; temp_lut_ver = cfg_ver; }
static void compute_temps(void) { if (temp_lut_ver != cfg_ver) rebuild_temp_coeffs(); double sum = 0; t_min = 1e30f; t_max = -1e30f; int imin = 0, imax = 0; for (int i = 0; i < NPIX; i++) { float t = temp_K1 * vui[i] + temp_K2; tmap[i] = t; sum += t; if (t < t_min) { t_min = t; imin = i; } if (t > t_max) { t_max = t; imax = i; } } t_avg = (float)(sum / NPIX); i_min = imin; i_max = imax; }

/* ============ RANGE AUTO (HISTOGRAMME) ============ */
static void update_range(void) {
    if (C.range_mode == 2 && prev_range != 2) { lock_lo = lo_t; lock_hi = hi_t; }
    prev_range = C.range_mode;
    if (C.range_mode == 1) { lo_t = C.man_min; hi_t = C.man_max; if (hi_t - lo_t < 1.0f) hi_t = lo_t + 1.0f; return; }
    if (C.range_mode == 2) { lo_t = lock_lo; hi_t = lock_hi; return; }
    float mn = t_min, mx = t_max;
    if (mx - mn < 1.5f) { float mid = (mx + mn) / 2; mn = mid - 0.75f; mx = mid + 0.75f; }
    int bins[256] = {0}; float bin_w = (mx - mn) / 256.0f; if (bin_w <= 0) bin_w = 1.0f;
    for (int i = 0; i < NPIX; i++) { int b = (int)((tmap[i] - mn) / bin_w); if (b < 0) b = 0; else if (b > 255) b = 255; bins[b]++; }
    int total = NPIX; int target_lo = total / 100; int target_hi = total - total / 100;
    int acc = 0; float lo = mn, hi = mx;
    for (int b = 0; b < 256; b++) { acc += bins[b]; if (acc >= target_lo) { lo = mn + b * bin_w; break; } }
    acc = 0;
    for (int b = 255; b >= 0; b--) { acc += bins[b]; if (acc >= target_hi) { hi = mn + (b + 1) * bin_w; break; } }
    if (hi - lo < 1.5f) { float mid = (hi + lo) / 2; lo = mid - 0.75f; hi = mid + 0.75f; }
    if (!range_init) { lo_t = lo; hi_t = hi; range_init = 1; } else { lo_t += 0.15f * (lo - lo_t); hi_t += 0.15f * (hi - hi_t); }
}

/* ============ IMAGE BUILD ============ */
static void build_small(void) { float sc = 255.0f / (hi_t - lo_t); for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) argb_s[y * W + x] = color_of(tmap[disp2data(x, y)], sc); }
static void build_hd(void) {
    static float td[NPIX], hp[H * HW]; static float wt[4][4]; static const float fr[4] = {0.625f, 0.875f, 0.125f, 0.375f}; static const int bs[4] = {-1, -1, 0, 0}; static int winit = 0;
    if (!winit) { for (int r = 0; r < 4; r++) { float f = fr[r], f2 = f * f, f3 = f2 * f; wt[r][0] = (-f3 + 2 * f2 - f) / 2; wt[r][1] = (3 * f3 - 5 * f2 + 2) / 2; wt[r][2] = (-3 * f3 + 4 * f2 + f) / 2; wt[r][3] = (f3 - f2) / 2; } winit = 1; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) td[y * W + x] = tmap[disp2data(x, y)];
    for (int y = 0; y < H; y++) { const float *row = td + y * W; float *out = hp + y * HW; for (int ox = 0; ox < HW; ox++) { int r = ox & 3, i0 = (ox >> 2) + bs[r]; float s = 0; for (int j = 0; j < 4; j++) s += wt[r][j] * row[clampi(i0 - 1 + j, 0, W - 1)]; out[ox] = s; } }
    float sc = 255.0f / (hi_t - lo_t), tlo = t_min, thi = t_max;
    for (int oy = 0; oy < HH; oy++) { int r = oy & 3, i0 = (oy >> 2) + bs[r]; const float *rw[4]; for (int j = 0; j < 4; j++) rw[j] = hp + clampi(i0 - 1 + j, 0, H - 1) * HW; uint32_t *out = argb_hd + oy * HW; for (int ox = 0; ox < HW; ox++) { float t = wt[r][0] * rw[0][ox] + wt[r][1] * rw[1][ox] + wt[r][2] * rw[2][ox] + wt[r][3] * rw[3][ox]; t = clampf(t, tlo, thi); out[ox] = color_of(t, sc); } }
}

/* ============ USB ============ */
static int note(int r) { if (r == -4 && !gone) { gone = 1; LOG("CAMERA DECONNECTEE (erreur -4)"); } return r; }
static int send_request(void) { uint8_t pkt[64]; int sent = 0, r = 0; if (req_mode == 3) return 0; memset(pkt, 0, sizeof pkt); pkt[0] = 0x81; if (req_mode == 0) r = libusb_bulk_transfer(dev, 0x02, pkt, 1, &sent, 500); else r = libusb_interrupt_transfer(dev, 0x01, pkt, req_mode == 1 ? 1 : 64, &sent, 500); return note(r); }
static int usb_open(void) { int r = libusb_init(&ctx); if (r < 0) { LOG("libusb_init: %s", libusb_strerror(r)); return -1; } dev = libusb_open_device_with_vid_pid(ctx, 0x5656, 0x1201); if (!dev) { LOG("camera 5656:1201 introuvable ou acces refuse"); return -1; } for (int i = 0; i < 2; i++) { if (libusb_kernel_driver_active(dev, i) == 1) libusb_detach_kernel_driver(dev, i); r = libusb_claim_interface(dev, i); LOG("claim interface %d: %s", i, r < 0 ? libusb_strerror(r) : "ok"); } return 0; }
static void usb_close(void) { if (dev) { libusb_release_interface(dev, 0); libusb_release_interface(dev, 1); libusb_close(dev); dev = NULL; } if (ctx) { libusb_exit(ctx); ctx = NULL; } }
static int send_cmd(uint8_t func, uint8_t reg, const uint32_t *vals, int count) { uint8_t pkt[64], reply[64]; int len = 3 + 4 * count; for (int attempt = 0; attempt < 2 && !gone; attempt++) { int padded = attempt == 0 ? pad_mode : !pad_mode; memset(pkt, 0, sizeof pkt); pkt[0] = func; pkt[1] = reg; pkt[2] = (uint8_t)count; for (int i = 0; i < count; i++) { pkt[3 + 4 * i] = vals[i] >> 24; pkt[4 + 4 * i] = vals[i] >> 16; pkt[5 + 4 * i] = vals[i] >> 8; pkt[6 + 4 * i] = vals[i]; } int sent = 0, got = 0; int r = note(libusb_interrupt_transfer(dev, 0x01, pkt, padded ? 64 : len, &sent, cmd_to)); if (r < 0) { LOG("cmd %02x/%02x envoi: %s", func, reg, libusb_strerror(r)); continue; } r = note(libusb_interrupt_transfer(dev, 0x81, reply, 64, &got, cmd_to)); if (r == 0 && got > 0) { pad_mode = padded; memcpy(last_reply, reply, got); last_len = got; return 0; } LOG("cmd %02x/%02x pas de reponse (%s)", func, reg, libusb_strerror(r)); } return -1; }
static int read_reg(uint8_t func, uint8_t reg, uint32_t *val) { uint32_t zero = 0; cmd_to = 150; int r = send_cmd(func, reg, &zero, 1); cmd_to = 500; if (r || last_len < 6) return -1; *val = ((uint32_t)last_reply[2] << 24) | ((uint32_t)last_reply[3] << 16) | ((uint32_t)last_reply[4] << 8) | last_reply[5]; return 0; }
static void drain_bulk(void) { uint8_t t[512]; for (int i = 0; i < 200 && !gone; i++) { int n = 0; int r = note(libusb_bulk_transfer(dev, 0x82, t, sizeof t, &n, 5)); if (r < 0 && n == 0) break; } }
static int grab_frame(void) { uint8_t *raw = (uint8_t *)raw16; for (int tries = 0; tries < 2 && !gone; tries++) { drain_bulk(); int r = send_request(); if (r < 0) { LOG("demande image: %s", libusb_strerror(r)); continue; } int got = 0, empty = 0; while (got < FRAME_BYTES && empty < 8) { int n = 0; r = note(libusb_bulk_transfer(dev, 0x82, raw + got, FRAME_BYTES - got, &n, rd_timeout)); got += n; if (r < 0) break; if (n == 0) empty++; } if (got == FRAME_BYTES) return 0; } return -1; }
static int grab_valid(void) {
    if (grab_frame()) return -1;
    uint8_t *raw = (uint8_t *)raw16;
    if (raw16[0] != MAGIC) {
        for (int off = 64; off <= 256; off += 64) { const uint16_t *w = (const uint16_t *)(raw + off); if (w[0] == MAGIC && w[2] == 0x0078 && w[3] == 0x005C) { memmove(raw, raw + off, FRAME_BYTES - off); int got = 0; while (got < off) { int n = 0; int r = note(libusb_bulk_transfer(dev, 0x82, raw + FRAME_BYTES - off + got, off - got, &n, 300)); got += n; if (r < 0) break; } if (resync_count++ < 5) LOG("resynchronisation: decalage de %d octets (%s)", off, got == off ? "ok" : "queue manquante"); if (got != off) return -1; break; } }
    }
    if (!dumped && raw16[0] == MAGIC) { char path[600]; snprintf(path, sizeof path, "%s/frame_raw.bin", outdir); FILE *f = fopen(path, "wb"); if (f) { fwrite(raw16, 1, FRAME_BYTES, f); fclose(f); } LOG("premiere image valide: compteur=%u temperatures internes (0.01C) %u %u %u %u", raw16[1], raw16[8], raw16[9], raw16[10], raw16[11]); dumped = 1; }
    if (raw16[0] != MAGIC) { if (badmagic++ < 5) LOG("en-tete inattendu: %04x (attendu aa55)", raw16[0]); drain_bulk(); return -1; }
    return 0;
}
static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static void find_bad(void) {
    static float t[NPIX], d[NPIX]; memcpy(t, off_, sizeof t); qsort(t, NPIX, sizeof(float), cmp_float); float med = t[NPIX / 2];
    for (int i = 0; i < NPIX; i++) d[i] = fabsf(off_[i] - med); qsort(d, NPIX, sizeof(float), cmp_float); float thr = 6.0f * 1.4826f * d[NPIX / 2]; if (thr < 1.0f) thr = 1.0f;
    int cnt = 0; for (int i = 0; i < NPIX; i++) { bad_[i] = fabsf(off_[i] - med) > thr; cnt += bad_[i]; } use_bad = cnt * 20 < NPIX; LOG("pixels aberrants: %d (filtre %s)", cnt, use_bad ? "actif" : "desactive"); }
static void progress(float f) { cal_prog = f; }
static int wait_ready(int max_ms, int need, float p0, float p1) { long t0 = now_ms(), lastp = 0; int good = 0; while (!gone && cam_run) { long el = now_ms() - t0; if (el > max_ms) break; if (el - lastp > 250) { progress(p0 + (p1 - p0) * el / max_ms); lastp = el; } if (grab_valid() == 0) { if (++good >= need) return 0; } else { good = 0; usleep(50000); } } return -1; }
static int calibrate(int with_nuc) {
    uint32_t one = 1, zero = 0; static double acc[NPIX]; double tw[4] = {0, 0, 0, 0}; memset(acc, 0, sizeof acc);
    cal_label = with_nuc ? "CALIBRATION COMPLETE" : "RECALIBRATION"; cal_prog = 0.0f; cal_active = 1;
    if (with_nuc) { LOG("calibration: NUC (peut durer 10 a 20 s)"); progress(0.02f); send_cmd(0x0A, 4, &one, 1); usleep(2000000); int rc = wait_ready(40000, 20, 0.05f, 0.45f); LOG("NUC: %s", rc ? "delai depasse" : "termine (images valides)"); }
    LOG("calibration: obturateur ferme"); progress(0.46f); send_cmd(0x0A, 3, &one, 1); usleep(500000); wait_ready(8000, 3, 0.46f, 0.5f);
    int ok = 0; long t0 = now_ms(); while (ok < 16 && !gone && cam_run && now_ms() - t0 < 12000) { if (grab_valid() == 0) { const uint16_t *img = raw16 + IMG_OFFSET; for (int k = 0; k < NPIX; k++) acc[k] += img[k]; for (int k = 0; k < 4; k++) tw[k] += raw16[8 + k]; ok++; progress(0.5f + 0.45f * ok / 16); } }
    LOG("calibration: %d images valides", ok); send_cmd(0x0A, 3, &zero, 1); usleep(500000); wait_ready(8000, 3, 0.95f, 1.0f); cal_active = 0;
    if (ok < 8) return -1;
    for (int k = 0; k < NPIX; k++) off_[k] = (float)(acc[k] / ok); for (int k = 0; k < 4; k++) tw_cal[k] = (float)(tw[k] / ok);
    find_bad(); iir_valid = 0; pthread_mutex_lock(&mtx); for (int k = 0; k < 4; k++) sh.twcal[k] = tw_cal[k]; pthread_mutex_unlock(&mtx); calibrated = 1; return 0;
}
static int next_frame(void) { if (grab_valid()) return -1; const uint16_t *img = raw16 + IMG_OFFSET; static float cur[NPIX]; for (int i = 0; i < NPIX; i++) cur[i] = (float)img[i] - off_[i]; if (use_bad && cam_deadpix) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int i = y * W + x; if (!bad_[i]) continue; float s = 0; int c = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy; if ((dx || dy) && xx >= 0 && xx < W && yy >= 0 && yy < H && !bad_[yy * W + xx]) { s += cur[yy * W + xx]; c++; } } if (c) cur[i] = s / c; } } float a = tf_alpha[clampi(cam_tfilter, 0, 3)]; if (!iir_valid || a >= 0.999f) { memcpy(vmap, cur, sizeof vmap); iir_valid = 1; } else for (int i = 0; i < NPIX; i++) vmap[i] = (1.0f - a) * vmap[i] + a * cur[i]; return 0; }
static void camera_reconnect(void) { cam_conn = 0; reconnects++; LOG("reconnexion de la camera..."); usb_close(); while (cam_run) { usleep(1000000); system("sudo -n chmod a+rw /dev/bus/usb/*/* 2>/dev/null"); gone = 0; if (usb_open() == 0) { uint32_t two = 2; send_cmd(0x04, 0xF0, &two, 1); usleep(300000); if (!gone) { cam_conn = 1; calibrated = 0; cam_post("CAMERA RECONNECTEE"); LOG("camera reconnectee"); return; } } gone = 1; usb_close(); } }
static void *cam_thread(void *arg) { (void)arg; long last_ok = now_ms(), retry_at = 0; while (cam_run) { if (gone) { camera_reconnect(); last_ok = now_ms(); continue; } int cmd = cam_cmd; if (cmd) { cam_cmd = 0; if (cmd == 1 || cmd == 2) { int rc = calibrate(cmd == 2); cam_post(rc ? "ECHEC DE LA CALIBRATION" : cmd == 2 ? "CALIBRATION TERMINEE" : "RECALIBRE"); } else if (cmd == 3) { static const uint8_t rg[4] = {0x02, 0x03, 0x05, 0x06}; int ok = 1; for (int k = 0; k < 4; k++) if (read_reg(0x05, rg[k], &regs_val[k])) ok = 0; regs_ok = ok; regs_ready = 1; } last_ok = now_ms(); continue; } if (!calibrated) { if (now_ms() >= retry_at) { int rc = calibrate(0); if (rc) { retry_at = now_ms() + 3000; cam_post("CALIBRATION IMPOSSIBLE: NOUVEL ESSAI"); } last_ok = now_ms(); } else usleep(20000); continue; } if (next_frame() == 0) { long t = now_ms(); if (t - last_ok > 600) { stalls++; LOG("pause camera de %ld ms", t - last_ok); } if (t - last_ok > max_gap) max_gap = t - last_ok; last_ok = t; pthread_mutex_lock(&mtx); memcpy(sh.v, vmap, sizeof vmap); sh.seq++; sh.counter = raw16[1]; sh.ts = t; pthread_mutex_unlock(&mtx); frames_cam++; } else usleep(5000); } return NULL; }

/* ============ CAPTURE / GALLERY ============ */
static int save_bmp(const char *path, const uint32_t *px, int w, int h) { FILE *f = fopen(path, "wb"); if (!f) return -1; int rb = (w * 3 + 3) & ~3; uint8_t hdr[54]; memset(hdr, 0, sizeof hdr); hdr[0] = 'B'; hdr[1] = 'M'; put32(hdr + 2, 54 + rb * h); put32(hdr + 10, 54); put32(hdr + 14, 40); put32(hdr + 18, w); put32(hdr + 22, h); hdr[26] = 1; hdr[28] = 24; put32(hdr + 34, rb * h); fwrite(hdr, 1, 54, f); uint8_t *row = calloc(rb, 1); for (int y = h - 1; y >= 0; y--) { for (int x = 0; x < w; x++) { uint32_t c = px[y * w + x]; row[3 * x] = c; row[3 * x + 1] = c >> 8; row[3 * x + 2] = c >> 16; } fwrite(row, 1, rb, f); } free(row); fclose(f); return 0; }
static void capture_init(void) { snprintf(capdir, sizeof capdir, "%s/captures", outdir); mkdir(capdir, 0777); DIR *d = opendir(capdir); if (!d) return; struct dirent *e; while ((e = readdir(d))) { int n; if (sscanf(e->d_name, "IMG_%d.bmp", &n) == 1 && n >= cap_index) cap_index = n + 1; } closedir(d); }
static void export_csv(const char *path) { FILE *f = fopen(path, "w"); if (!f) return; for (int y = 0; y < H; y++) { for (int x = 0; x < W; x++) fprintf(f, x ? ",%.1f" : "%.1f", tmap[disp2data(x, y)]); fputc('\n', f); } fclose(f); }
static void export_txt(const char *path) { FILE *f = fopen(path, "w"); if (!f) return; time_t tt = time(NULL); char ts[64]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&tt)); float zmn, zmx, zav; zone_stats(&zmn, &zmx, &zav); fprintf(f, "Date: %s\nViseur: %.1f C\nMax: %.1f C\nMin: %.1f C\nMoyenne image: %.1f C\n", ts, spot_temp(), t_max, t_min, t_avg); fprintf(f, "Zone (%d px): min %.1f max %.1f moy %.1f C\n", C.zone, zmn, zmx, zav); fprintf(f, "Emissivite: %.2f\nTemperature ambiante: %.1f C\nSensibilite: %.4f C/unite\nDecalage: %.2f C\n", C.emis, C.tamb, C.gain, C.bias); fprintf(f, "Palette: %s\nPlage: %.1f a %.1f C (%s)\nZoom: x%d\n", pal_names[C.palette], lo_t, hi_t, range_names[C.range_mode], zooms[C.zoom]); fprintf(f, "Les temperatures sont approximatives si la camera n'est pas etalonnee.\n"); fclose(f); }
static void save_clean(const char *path) { if (C.quality == 2) { save_bmp(path, argb_hd, HW, HH); return; } static uint32_t big[HW * HH]; for (int y = 0; y < HH; y++) for (int x = 0; x < HW; x++) big[y * HW + x] = argb_s[(y / HQ) * W + x / HQ]; save_bmp(path, big, HW, HH); }
static void do_capture(void) { char p[700]; int want_screen = (C.cap_fmt == 0 || C.cap_fmt == 2); int want_img = (C.cap_fmt == 1 || C.cap_fmt == 2); if (want_img) { snprintf(p, sizeof p, "%s/IMG_%04d.bmp", capdir, cap_index); save_clean(p); } if (want_screen) { if (cap_screen_buf) { if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, cap_screen_buf, cap_screen_w * 4) == 0) { snprintf(p, sizeof p, "%s/IMG_%04d.bmp", capdir, cap_index); if (!want_img) save_bmp(p, cap_screen_buf, cap_screen_w, cap_screen_h); else { snprintf(p, sizeof p, "%s/IMG_%04d_screen.bmp", capdir, cap_index); save_bmp(p, cap_screen_buf, cap_screen_w, cap_screen_h); } } else LOG("Capture ecran echouee: %s", SDL_GetError()); } } if (C.cap_csv) { snprintf(p, sizeof p, "%s/IMG_%04d.csv", capdir, cap_index); export_csv(p); } if (C.cap_txt) { snprintf(p, sizeof p, "%s/IMG_%04d.txt", capdir, cap_index); export_txt(p); } toast("CAPTURE IMG_%04d ENREGISTREE", cap_index); LOG("capture IMG_%04d", cap_index); cap_index++; }
static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void gallery_load(void) { if (gal_tex) { SDL_DestroyTexture(gal_tex); gal_tex = NULL; } if (gal_n == 0) return; char p[700]; snprintf(p, sizeof p, "%s/%s", capdir, gal_name[gal_i]); SDL_Surface *s = SDL_LoadBMP(p); if (!s) return; gal_tex = SDL_CreateTextureFromSurface(ren, s); SDL_FreeSurface(s); if (gal_tex) SDL_QueryTexture(gal_tex, NULL, NULL, &gal_w, &gal_h); }
static void gallery_scan(void) { gal_n = 0; DIR *d = opendir(capdir); if (d) { struct dirent *e; while ((e = readdir(d)) && gal_n < 512) { size_t l = strlen(e->d_name); if (l > 4 && l < 47 && !strncmp(e->d_name, "IMG_", 4) && !strcmp(e->d_name + l - 4, ".bmp") && !strstr(e->d_name, "_img")) strcpy(gal_name[gal_n++], e->d_name); } closedir(d); } qsort(gal_name, gal_n, sizeof gal_name[0], cmp_name); if (gal_i >= gal_n) gal_i = gal_n - 1; if (gal_i < 0) gal_i = 0; gallery_load(); }
static void gallery_delete(void) { char p[700]; snprintf(p, sizeof p, "%s/%s", capdir, gal_name[gal_i]); remove(p); size_t l = strlen(p); strcpy(p + l - 4, ".csv"); remove(p); strcpy(p + l - 4, ".txt"); remove(p); snprintf(p, sizeof p, "%s/%.*s_img.bmp", capdir, (int)strlen(gal_name[gal_i]) - 4, gal_name[gal_i]); remove(p); toast("IMAGE SUPPRIMEE"); gallery_scan(); }

/* ============ MENU / CONFIG ============ */
#define IT(l, t, v, mn, mx, st, o, n, a) {l, t, v, mn, mx, st, o, n, a}
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static void menu_init(void) { for (int i = 0; i < NPAL; i++) pal_opts[i] = pal_names[i]; pg_btn[0] = (MItem)IT("ASSISTANT", MT_ACTION, NULL, 0, 0, 0, NULL, 0, M_WIZARD); pg_btn[1] = (MItem)IT("EFFACER TOUT", MT_ACTION, NULL, 0, 0, 0, NULL, 0, M_BTNDEF); for (int a = 0; a < NACT_COUNT; a++) pg_btn[2 + a] = (MItem)IT(act_name[a], MT_BIND, NULL, 0, 0, 0, NULL, 0, a); pages[PG_MAIN] = (Page){"MENU", pg_main, COUNT(pg_main)}; pages[PG_IMAGE] = (Page){"IMAGE", pg_image, COUNT(pg_image)}; pages[PG_MEAS] = (Page){"MESURE", pg_meas, COUNT(pg_meas)}; pages[PG_TEMP] = (Page){"TEMPERATURE", pg_temp, COUNT(pg_temp)}; pages[PG_CAM] = (Page){"CAMERA", pg_cam, COUNT(pg_cam)}; pages[PG_CAPS] = (Page){"CAPTURES", pg_caps, COUNT(pg_caps)}; pages[PG_UI] = (Page){"INTERFACE", pg_ui, COUNT(pg_ui)}; pages[PG_PAD] = (Page){"MANETTE", pg_pad, COUNT(pg_pad)}; pages[PG_BTN] = (Page){"BOUTONS", pg_btn, 2 + NACT_COUNT}; }
static void item_value(const MItem *it, char *buf, size_t n) { buf[0] = 0; switch (it->type) { case MT_CHOICE: snprintf(buf, n, "< %s >", it->opts[clampi(*(int *)it->val, 0, it->nopt - 1)]); break; case MT_INT: snprintf(buf, n, "< %d >", *(int *)it->val); break; case MT_FLOAT: { float v = *(float *)it->val; snprintf(buf, n, it->st < 0.01f ? "< %.3f >" : it->st < 0.1f ? "< %.2f >" : "< %.1f >", v); break; } case MT_SUB: snprintf(buf, n, ">"); break; case MT_BIND: input_name(C.bind[it->arg], buf, n); break; default: break; } }
static void menu_cb(int id) { if (id == CB_MAT && C.material > 0) C.emis = mat_e[C.material]; else if (id == CB_EMIS) C.material = 0; }
static void item_adjust(const MItem *it, int dir) { if (it->type == MT_CHOICE) { int *p = (int *)it->val; *p = (*p + dir + it->nopt) % it->nopt; } else if (it->type == MT_INT) { int *p = (int *)it->val; *p = clampi(*p + dir * (int)it->st, (int)it->mn, (int)it->mx); } else if (it->type == MT_FLOAT) { float *p = (float *)it->val; *p = clampf(roundf((*p + dir * it->st) / it->st) * it->st, it->mn, it->mx); } else return; if (it->type != MT_ACTION && it->arg) menu_cb(it->arg); cfg_sanitize(); cfg_ver++; }
static void cfg_defaults(void) { memset(&C, 0, sizeof C); C.quality = 1; C.man_min = 15; C.man_max = 40; C.tfilter = 1; C.deadpix = 1; C.ui_big = 1; C.marks = 2; C.crosshair = 1; C.colorbar = 1; C.show_clock = 1; C.show_batt = 1; C.zone = 24; C.alarm_hi = 37.5f; C.alarm_lo = 15.0f; C.cap_fmt = 0; C.cap_csv = 1; C.cap_txt = 1; C.emis = 0.95f; C.tamb = 20.0f; C.gain = 0.04f; C.nuc_start = 1; C.ax_lx = 0; C.ax_ly = 1; C.ax_rx = 2; C.ax_ry = 3; C.deadzone = 20; C.stick_speed = 60; C.analog_cursor = 1; for (int i = 0; i < NACT_COUNT; i++) C.bind[i] = -1; }
static void cfg_sanitize(void) { C.palette = clampi(C.palette, 0, NPAL - 1); C.quality = clampi(C.quality, 0, 2); C.range_mode = clampi(C.range_mode, 0, 2); C.zoom = clampi(C.zoom, 0, 3); C.tfilter = clampi(C.tfilter, 0, 3); C.hud = clampi(C.hud, 0, 2); C.marks = clampi(C.marks, 0, 2); C.meas_mode = clampi(C.meas_mode, 0, 1); C.zone = clampi(C.zone, 6, 60); C.alarm_mode = clampi(C.alarm_mode, 0, 3); C.cap_fmt = clampi(C.cap_fmt, 0, 2); C.material = clampi(C.material, 0, 10); C.tref_src = clampi(C.tref_src, 0, 3); C.autocal = clampi(C.autocal, 0, 4); C.ax_lx = clampi(C.ax_lx, 0, 7); C.ax_ly = clampi(C.ax_ly, 0, 7); C.ax_rx = clampi(C.ax_rx, 0, 7); C.ax_ry = clampi(C.ax_ry, 0, 7); C.deadzone = clampi(C.deadzone, 5, 60); C.stick_speed = clampi(C.stick_speed, 20, 200); C.rstick_mode = clampi(C.rstick_mode, 0, 2); C.emis = clampf(C.emis, 0.1f, 1.0f); C.gain = clampf(C.gain, 0.005f, 0.5f); C.pal_inv = !!C.pal_inv; C.mirror = !!C.mirror; C.flip = !!C.flip; C.deadpix = !!C.deadpix; C.ui_big = !!C.ui_big; C.crosshair = !!C.crosshair; C.colorbar = !!C.colorbar; C.show_clock = !!C.show_clock; C.show_batt = !!C.show_batt; C.graph = !!C.graph; C.fahrenheit = !!C.fahrenheit; C.cap_csv = !!C.cap_csv; C.cap_txt = !!C.cap_txt; C.nuc_start = !!C.nuc_start; C.invert_y = !!C.invert_y; C.analog_cursor = !!C.analog_cursor; }
static void cfg_save(void) { char path[600]; snprintf(path, sizeof path, "%s/settings.cfg", outdir); FILE *f = fopen(path, "w"); if (!f) return; for (unsigned i = 0; i < sizeof ce / sizeof ce[0]; i++) { if (ce[i].isf) fprintf(f, "%s=%.4f\n", ce[i].key, *(float *)ce[i].p); else fprintf(f, "%s=%d\n", ce[i].key, *(int *)ce[i].p); } for (int a = 0; a < NACT_COUNT; a++) fprintf(f, "btn_%s=%d\n", act_key[a], C.bind[a]); fclose(f); }
static void cfg_load(void) { char path[600], line[128]; snprintf(path, sizeof path, "%s/settings.cfg", outdir); FILE *f = fopen(path, "r"); if (!f) return; while (fgets(line, sizeof line, f)) { char *eq = strchr(line, '='); if (!eq) continue; *eq = 0; const char *k = line, *v = eq + 1; if (!strncmp(k, "btn_", 4)) { for (int a = 0; a < NACT_COUNT; a++) if (!strcmp(k + 4, act_key[a])) { C.bind[a] = atoi(v); if (C.bind[a] >= 0) cfg_bound = 1; } continue; } for (unsigned i = 0; i < sizeof ce / sizeof ce[0]; i++) if (!strcmp(k, ce[i].key)) { if (ce[i].isf) *(float *)ce[i].p = (float)atof(v); else *(int *)ce[i].p = atoi(v); } } fclose(f); cfg_sanitize(); }
static int menu_rows(void) { int rowh = 8 * sc_b() + 6; return clampi((OH - 16 - 56 - 34) / rowh, 4, 30); }
static void menu_input(int act) { Page *pg = &pages[menu_page]; int *sel = &menu_sel[menu_page]; MItem *it = &pg->items[*sel]; switch (act) { case A_UP: *sel = (*sel + pg->n - 1) % pg->n; break; case A_DOWN: *sel = (*sel + 1) % pg->n; break; case A_LEFT: item_adjust(it, -1); break; case A_RIGHT: item_adjust(it, 1); break; case A_OK: if (it->type == MT_ACTION) menu_do(it->arg); else if (it->type == MT_SUB) menu_page = it->arg; else if (it->type == MT_BIND) { bind_act = it->arg; state = S_BIND; } else item_adjust(it, 1); break; case A_BACK: if (menu_page == PG_MAIN) menu_close(); else menu_page = PG_MAIN; break; case A_MENU: menu_close(); break; default: break; } int *top = &menu_top[menu_page], rows = menu_rows(); if (*sel < *top) *top = *sel; if (*sel >= *top + rows) *top = *sel - rows + 1; if (*top < 0) *top = 0; }
static void ref_toggle(void) { if (ref_on) { ref_on = 0; toast("REFERENCE EFFACEE"); } else { ref_t = current_value(); ref_on = 1; toast("REFERENCE FIXEE: DELTA ACTIF"); } }
static void menu_do(int id) { switch (id) { case M_RESUME: menu_close(); break; case M_CAPTURE: menu_close(); if (cal_active) toast("CALIBRATION EN COURS"); else cap_pending = 1; break; case M_GALLERY: gal_confirm = 0; gal_i = 1000000; gallery_scan(); state = S_GALLERY; break; case M_INFO: regs_ready = 0; cam_cmd = 3; state = S_INFO; break; case M_QUIT: cfg_save(); run = 0; break; case M_RECAL: menu_close(); if (cal_active) toast("CALIBRATION EN COURS"); else cam_cmd = 1; break; case M_NUC: menu_close(); if (cal_active) toast("CALIBRATION EN COURS"); else cam_cmd = 2; break; case M_CAL1: calib_point = 1; calib_ref = roundf(current_value() * 10.0f) / 10.0f; state = S_CALIB; break; case M_CAL2: if (!cal_p1_valid) toast("FAITES D'ABORD LE POINT 1"); else { calib_point = 2; calib_ref = roundf(current_value() * 10.0f) / 10.0f; state = S_CALIB; } break; case M_CALRESET: C.bias = 0; C.gain = 0.04f; cal_p1_valid = 0; cfg_ver++; toast("ETALONNAGE REINITIALISE"); break; case M_WIZARD: wiz_idx = 0; for (int a = 0; a < NESSENTIAL; a++) C.bind[a] = -1; state = S_WIZARD; break; case M_BTNDEF: for (int a = 0; a < NACT_COUNT; a++) C.bind[a] = -1; toast("BOUTONS EFFACES (CLAVIER ACTIF)"); break; case M_HELP: state = S_HELP; break; case M_PADTEST: state = S_PADTEST; break; case M_RECONNECT: menu_close(); gone = 1; toast("RECONNEXION..."); break; case M_REF: ref_toggle(); break; case M_DEFAULTS: { int keep[NACT_COUNT]; memcpy(keep, C.bind, sizeof keep); cfg_defaults(); memcpy(C.bind, keep, sizeof keep); cfg_ver++; toast("REGLAGES PAR DEFAUT"); break; } default: break; } }
static void menu_close(void) { state = S_VIEW; cfg_save(); }
static void capture_input(int id) { char nm[32]; if (state == S_WIZARD) { if (wiz_idx > A_BACK && id == C.bind[A_BACK]) wiz_idx++; else { for (int j = 0; j < wiz_idx; j++) if (C.bind[j] == id) { toast("DEJA UTILISE"); return; } for (int j = NESSENTIAL; j < NACT_COUNT; j++) if (C.bind[j] == id) C.bind[j] = -1; C.bind[wiz_idx++] = id; } if (wiz_idx >= NESSENTIAL) { cfg_save(); cfg_bound = 1; state = S_HELP; toast("CONFIGURATION ENREGISTREE"); } } else if (state == S_BIND) { if (bind_act != A_BACK && id == C.bind[A_BACK] && C.bind[A_BACK] >= 0) { state = S_MENU; return; } for (int j = 0; j < NACT_COUNT; j++) if (j != bind_act && C.bind[j] == id) C.bind[j] = -1; C.bind[bind_act] = id; cfg_save(); state = S_MENU; input_name(id, nm, sizeof nm); toast("%s ASSIGNE", nm); } }
static void dispatch(int a) { switch (state) { case S_VIEW: view_input(a); break; case S_MENU: menu_input(a); break; case S_CALIB: calib_input(a); break; case S_INFO: if (a == A_BACK || a == A_OK || a == A_MENU) state = S_MENU; break; case S_HELP: if (a == A_BACK || a == A_OK || a == A_MENU) state = S_VIEW; break; case S_GALLERY: gallery_input(a); break; default: break; } }
static void view_input(int act) { switch (act) { case A_UP: spot_set(spot_dx, spot_dy - move_step); break; case A_DOWN: spot_set(spot_dx, spot_dy + move_step); break; case A_LEFT: spot_set(spot_dx - move_step, spot_dy); break; case A_RIGHT: spot_set(spot_dx + move_step, spot_dy); break; case A_OK: case A_MENU: state = S_MENU; menu_page = PG_MAIN; break; case A_BACK: if (now_ms() < quit_arm) { cfg_save(); run = 0; } else { quit_arm = now_ms() + 2000; toast("REAPPUYEZ SUR RETOUR POUR QUITTER"); } break; case A_CAPTURE: if (cal_active) toast("CALIBRATION EN COURS"); else cap_pending = 1; break; case A_PALETTE: C.palette = (C.palette + 1) % NPAL; cfg_ver++; toast("PALETTE: %s", pal_names[C.palette]); break; case A_PAL_PREV: C.palette = (C.palette + NPAL - 1) % NPAL; cfg_ver++; toast("PALETTE: %s", pal_names[C.palette]); break; case A_ZOOM_IN: C.zoom = clampi(C.zoom + 1, 0, 3); cfg_ver++; toast("ZOOM X%d", zooms[C.zoom]); break; case A_ZOOM_OUT: C.zoom = clampi(C.zoom - 1, 0, 3); cfg_ver++; toast("ZOOM X%d", zooms[C.zoom]); break; case A_RECAL: if (cal_active) toast("CALIBRATION EN COURS"); else cam_cmd = 1; break; case A_FREEZE: frozen = !frozen; toast(frozen ? "IMAGE GELEE" : "IMAGE EN DIRECT"); break; case A_HUD: C.hud = (C.hud + 1) % 3; cfg_ver++; break; case A_RANGE: C.range_mode = (C.range_mode + 1) % 3; cfg_ver++; toast("PLAGE: %s", range_names[C.range_mode]); break; case A_CENTER: spot_set(W / 2, H / 2); break; case A_REF: ref_toggle(); break; case A_MEASURE: C.meas_mode = !C.meas_mode; cfg_ver++; toast("MESURE: %s", o_meas[C.meas_mode]); break; case A_HELP: state = S_HELP; break; case A_GALLERY: gal_confirm = 0; gal_i = 1000000; gallery_scan(); state = S_GALLERY; break; case A_LEVEL_UP: range_adjust(0.04f, 1.0f); break; case A_LEVEL_DOWN: range_adjust(-0.04f, 1.0f); break; case A_SPAN_UP: range_adjust(0.0f, 0.93f); break; case A_SPAN_DOWN: range_adjust(0.0f, 1.0f / 0.93f); break; default: break; } }
static inline void spot_set(int x, int y) { spot_dx = clampi(x, 0, W - 1); spot_dy = clampi(y, 0, H - 1); spot_fx = (float)spot_dx; spot_fy = (float)spot_dy; }
static void calib_input(int act) { switch (act) { case A_UP: calib_ref += 1.0f; break; case A_DOWN: calib_ref -= 1.0f; break; case A_RIGHT: calib_ref += 0.1f; break; case A_LEFT: calib_ref -= 0.1f; break; case A_BACK: case A_MENU: state = S_MENU; break; case A_OK: { float v = spot_v(); float emis_eff = C.emis < 0.1f ? 0.1f : C.emis; float t_app = C.tamb + (calib_ref - C.tamb) * emis_eff; float tref = tw_ui[C.tref_src] / 100.0f; if (calib_point == 1) { C.bias = t_app - tref - C.gain * v; cal_p1_valid = 1; cal_p1_v = v; cal_p1_a = t_app; toast("POINT 1 ENREGISTRE"); } else { if (fabsf(v - cal_p1_v) < 20.0f) { toast("POINTS TROP PROCHES: CHANGEZ DE CIBLE"); break; } C.gain = clampf((t_app - cal_p1_a) / (v - cal_p1_v), 0.005f, 0.5f); C.bias = cal_p1_a - tref - C.gain * cal_p1_v; toast("SENSIBILITE: %.4f C PAR UNITE", C.gain); } C.material = 0; cfg_ver++; cfg_save(); state = S_MENU; break; } default: break; } calib_ref = clampf(calib_ref, -40, 500); }
static void gallery_input(int act) { if (gal_confirm) { if (act == A_OK) gallery_delete(); gal_confirm = 0; return; } switch (act) { case A_LEFT: case A_UP: if (gal_n) { gal_i = (gal_i + gal_n - 1) % gal_n; gallery_load(); } break; case A_RIGHT: case A_DOWN: if (gal_n) { gal_i = (gal_i + 1) % gal_n; gallery_load(); } break; case A_OK: if (gal_n) gal_confirm = 1; break; case A_BACK: case A_MENU: if (gal_tex) { SDL_DestroyTexture(gal_tex); gal_tex = NULL; } state = S_VIEW; break; default: break; } }
static void input_down(int id) { long now = now_ms(); char nm[32]; if (id < 0 || id >= NID) return; input_name(id, nm, sizeof nm); snprintf(pad_last, sizeof pad_last, "%s", nm); pad_pressed[id] = 1; held[id] = now; held_act[id] = -1; hold_next[id] = now + 400; LOG("entree %s", nm); if (state == S_WIZARD || state == S_BIND) { capture_input(id); return; } if (state == S_PADTEST) return; int a = find_action(id); if (a < 0) a = default_nav(id); held_act[id] = a; if (a >= 0) dispatch(a); else if (id < 64) { if (C.bind[A_MENU] < 0 && C.bind[A_OK] < 0) { toast("AUCUN BOUTON MENU: ASSISTANT"); wiz_idx = 0; state = S_WIZARD; } else toast("BOUTON %d NON ASSIGNE", id); } }
static void input_up(int id) { if (id < 0 || id >= NID) return; held[id] = 0; pad_pressed[id] = 0; }
static void axis_event(int axis, int value) { if (axis < 0 || axis > 7) return; if (axis_logged < 200) { LOG("axe %d = %d (repos %d)", axis, value, axis_rest[axis]); axis_logged++; } axis_raw[axis] = value; float d = axis_norm(axis); for (int pos = 0; pos < 2; pos++) { float v = pos ? d : -d; int id = ID_AXIS + 2 * axis + pos; if (!axis_dir[axis][pos] && v > 0.65f) { axis_dir[axis][pos] = 1; input_down(id); } else if (axis_dir[axis][pos] && v < 0.40f) { axis_dir[axis][pos] = 0; input_up(id); } } }
static void hat_event(int value) { static const int bits[4] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT}; for (int i = 0; i < 4; i++) { int was = hat_prev & bits[i], is = value & bits[i]; if (is && !was) input_down(ID_HAT + i); else if (!is && was) input_up(ID_HAT + i); } hat_prev = value; }
static void process_sticks(long now) { float dt = stick_t ? (now - stick_t) / 1000.0f : 0.0f; stick_t = now; if (dt > 0.2f) dt = 0.2f; if (state != S_VIEW || cal_active) return; if (C.analog_cursor) { int bound = 0; for (int p = 0; p < 2; p++) if (find_action(ID_AXIS + 2 * C.ax_lx + p) >= 0 || find_action(ID_AXIS + 2 * C.ax_ly + p) >= 0) bound = 1; if (!bound) { float sx = stick_val(C.ax_lx), sy = stick_val(C.ax_ly) * (C.invert_y ? -1.0f : 1.0f); if (sx != 0.0f || sy != 0.0f) { float k = C.stick_speed * dt; spot_fx = clampf(spot_fx + sx * fabsf(sx) * k, 0, W - 1); spot_fy = clampf(spot_fy + sy * fabsf(sy) * k, 0, H - 1); spot_dx = (int)(spot_fx + 0.5f); spot_dy = (int)(spot_fy + 0.5f); } } } float rx = stick_val(C.ax_rx), ry = stick_val(C.ax_ry) * (C.invert_y ? -1.0f : 1.0f); if (C.rstick_mode == 1) { if (rx != 0.0f || ry != 0.0f) range_adjust(-ry * 0.5f * dt, expf(-rx * 0.8f * dt)); } else if (C.rstick_mode == 0) { for (int d = 0; d < 4; d++) { float v = d == 0 ? -ry : d == 1 ? ry : d == 2 ? rx : -rx; if (v < 0.6f) { rst_next[d] = 0; continue; } if (now < rst_next[d]) continue; rst_next[d] = now + (rst_next[d] ? 350 : 600); if (d == 0) view_input(A_ZOOM_IN); else if (d == 1) view_input(A_ZOOM_OUT); else if (d == 2) view_input(A_PALETTE); else view_input(A_PAL_PREV); } } }

/* ============ DRAW HELPERS ============ */
static void draw_trend(int x, int y, int w, int h) { if (trend_n < 2) return; fillr(x, y, w, h, 0, 0, 0, 150); rectr(x, y, w, h, 120, 120, 120); float mn = trend[0], mx = trend[0]; for (int i = 1; i < trend_n; i++) { if (trend[i] < mn) mn = trend[i]; if (trend[i] > mx) mx = trend[i]; } if (mx - mn < 1.0f) { float m = (mx + mn) / 2; mn = m - 0.5f; mx = m + 0.5f; } for (int i = 1; i < trend_n; i++) { int x0 = x + 2 + (i - 1) * (w - 4) / (TREND_N - 1), x1 = x + 2 + i * (w - 4) / (TREND_N - 1); int y0 = y + h - 3 - (int)((trend[i - 1] - mn) / (mx - mn) * (h - 6)); int y1 = y + h - 3 - (int)((trend[i] - mn) / (mx - mn) * (h - 6)); liner(x0, y0, x1, y1, 255, 230, 80); } }
static void battery_update(long now) { if (now - batt_t < 15000 && batt_t) return; batt_t = now; FILE *f = fopen("/sys/class/power_supply/battery/capacity", "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } batt_pct = v; }
static void draw_hud(const SDL_Rect *src, const SDL_Rect *dst) { char b[96], c[32]; int sx, sy, in, sb = sc_s(), sn = sc_n(); float ts = current_value(); if (C.hud == 2) goto marks_only; if (C.marks && C.hud != 2) { int pairs[2] = {i_max, i_min}, spx, spy, spin; img2scr(spot_dx, spot_dy, src, dst, &spx, &spy, &spin); for (int k = 0; k < 2; k++) { img2scr(data2dispx(pairs[k]), data2dispy(pairs[k]), src, dst, &sx, &sy, &in); if (!in) continue; int near_spot = abs(sx - spx) < 90 && abs(sy - spy) < 30; if (k == 0) rectr(sx - 5, sy - 5, 10, 10, 255, 120, 0); else rectr(sx - 5, sy - 5, 10, 10, 0, 200, 255); if (C.marks == 2 && !near_spot) { fmt_temp(c, sizeof c, k == 0 ? t_max : t_min, 0); int lx = sx + 9, ly = sy - 18; if (lx + text_w(c, 2) > OW - 30) lx = sx - 9 - text_w(c, 2); if (ly < 70) ly = sy + 8; fillr(lx - 2, ly - 1, text_w(c, 2) + 4, 18, 0, 0, 0, 140); txt(lx, ly, 2, c, k == 0 ? 255 : 140, k == 0 ? 170 : 220, k == 0 ? 90 : 255); } } } marks_only: if (C.meas_mode == 1 && C.hud != 2) { int x0, y0, x1, y1, ax, ay, bx, by, i1, i2; zone_rect(&x0, &y0, &x1, &y1); img2scr(x0, y0, src, dst, &ax, &ay, &i1); img2scr(x1, y1, src, dst, &bx, &by, &i2); rectr(ax - dst->w / src->w / 2, ay - dst->h / src->h / 2, bx - ax + dst->w / src->w, by - ay + dst->h / src->h, 255, 255, 255); } if (C.crosshair && C.hud != 2) { img2scr(spot_dx, spot_dy, src, dst, &sx, &sy, &in); for (int pass = 0; pass < 2; pass++) { int o = pass == 0 ? 1 : 0, col = pass == 0 ? 0 : 255; liner(sx - 14, sy + o, sx - 5, sy + o, col, col, col); liner(sx + 5, sy + o, sx + 14, sy + o, col, col, col); liner(sx + o, sy - 14, sx + o, sy - 5, col, col, col); liner(sx + o, sy + 5, sx + o, sy + 14, col, col, col); } if (C.meas_mode == 0) { fmt_temp(b, sizeof b, ts, 0); int lx = sx + 12, ly = sy - 22; if (lx + text_w(b, 2) > OW - 40) lx = sx - 12 - text_w(b, 2); if (ly < 70) ly = sy + 10; fillr(lx - 3, ly - 2, text_w(b, 2) + 6, 20, 0, 0, 0, 150); txt(lx, ly, 2, b, 255, 255, 255); } } if (C.hud == 2) return; if (C.hud == 1) { fmt_temp(b, sizeof b, ts, 1); fillr(0, 0, text_w(b, sn) + 16, 8 * sn + 8, 0, 0, 0, 150); txt(8, 4, sn, b, 255, 255, 255); return; } int th = 8 * sb + 8 * sn + 10, colw = (OW - 30) / 3; float zmn = t_min, zmx = t_max, zav = t_avg; if (C.meas_mode == 1) zone_stats(&zmn, &zmx, &zav); fillr(0, 0, OW, th, 0, 0, 0, 160); txt(8, 3, sb, C.meas_mode ? "MOY ZONE" : "ACTUEL", 230, 230, 120); fmt_temp(b, sizeof b, ts, 1); txt(8, 3 + 8 * sb + 3, sn, b, 255, 255, 255); txt(8 + colw, 3, sb, C.meas_mode ? "MAX ZONE" : "MAX", 255, 140, 60); fmt_temp(b, sizeof b, zmx, 1); txt(8 + colw, 3 + 8 * sb + 3, sn, b, 255, 170, 90); txt(8 + 2 * colw, 3, sb, C.meas_mode ? "MIN ZONE" : "MIN", 110, 200, 255); fmt_temp(b, sizeof b, zmn, 1); txt(8 + 2 * colw, 3 + 8 * sb + 3, sn, b, 140, 220, 255); fmt_temp(c, sizeof c, C.meas_mode ? zav : t_avg, 1); snprintf(b, sizeof b, "MOY %s", c); if (ref_on) { char d[32]; fmt_delta(d, sizeof d, ts - ref_t, 1); snprintf(b + strlen(b), sizeof b - strlen(b), "   DELTA %s", d); } fillr(0, th, text_w(b, 2) + 14, 20, 0, 0, 0, 130); txt(6, th + 3, 2, b, 220, 220, 220); int bh = 22; fillr(0, OH - bh, OW, bh, 0, 0, 0, 160); snprintf(b, sizeof b, "%s Z%d E%.2f %s", pal_names[C.palette], zooms[C.zoom], C.emis, range_names[C.range_mode]); txt(6, OH - 18, 2, b, 230, 230, 230); char r[64] = ""; time_t tt = time(NULL); struct tm *lt = localtime(&tt); if (C.show_batt && batt_pct >= 0) snprintf(r + strlen(r), sizeof r - strlen(r), "%d%% ", batt_pct); if (C.show_clock && lt) snprintf(r + strlen(r), sizeof r - strlen(r), "%02d:%02d ", lt->tm_hour, lt->tm_min); snprintf(r + strlen(r), sizeof r - strlen(r), "%s%dFPS", frozen ? "GEL " : "", fps_now); txt_r(OW - 6, OH - 18, 2, r, frozen ? 255 : 170, frozen ? 90 : 255, frozen ? 90 : 170); if (C.graph) draw_trend(8, OH - bh - 52, 170, 46); if (C.colorbar) { int bx = OW - 24, by = th + 8, bhh = OH - bh - by - 8; for (int y = 0; y < bhh; y += 2) { int k = 255 - 255 * y / bhh; if (C.pal_inv) k = 255 - k; fillr(bx, by + y, 12, 2, pal[C.palette][k][0], pal[C.palette][k][1], pal[C.palette][k][2], 255); } rectr(bx - 1, by - 1, 14, bhh + 2, 255, 255, 255); fmt_temp(b, sizeof b, hi_t, 0); txt_r(bx - 4, by, 2, b, 255, 255, 255); fmt_temp(b, sizeof b, lo_t, 0); txt_r(bx - 4, by + bhh - 14, 2, b, 255, 255, 255); float tv = ts; int ty = by + bhh - (int)((clampf(tv, lo_t, hi_t) - lo_t) / (hi_t - lo_t) * bhh); liner(bx - 8, ty, bx - 1, ty, 255, 255, 255); liner(bx - 8, ty + 1, bx - 1, ty + 1, 0, 0, 0); if (C.alarm_mode) { float al[2] = {C.alarm_hi, C.alarm_lo}; for (int k = 0; k < 2; k++) { if ((C.alarm_mode == 1 && k == 1) || (C.alarm_mode == 2 && k == 0)) continue; if (al[k] < lo_t || al[k] > hi_t) continue; int ay = by + bhh - (int)((al[k] - lo_t) / (hi_t - lo_t) * bhh); liner(bx - 14, ay, bx + 14, ay, 255, 0, 255); } } } }
static void draw_toast(int y) { if (now_ms() >= toast_until) return; int sc = C.ui_big ? 3 : 2; int w = text_w(toast_msg, sc) + 24; if (w > OW - 8) { sc = 2; w = text_w(toast_msg, sc) + 24; } fillr((OW - w) / 2, y, w, 8 * sc + 14, 0, 0, 0, 215); rectr((OW - w) / 2, y, w, 8 * sc + 14, 255, 230, 120); txt_c(y + 7, sc, toast_msg, 255, 255, 120); }
static void draw_toast_top(void) { if (now_ms() >= toast_until) return; int sc = 2, w = text_w(toast_msg, sc) + 16; if (w > OW - 190) w = OW - 190; int x = OW - 96 - w; if (x < 180) x = 180; fillr(x, 8, w, 28, 40, 30, 0, 255); rectr(x, 8, w, 28, 255, 230, 120); txt(x + 8, 14, sc, toast_msg, 255, 255, 120); }
static void draw_panel(int x, int y, int w, int h, const char *title) { fillr(x, y, w, h, 14, 18, 32, 255); rectr(x, y, w, h, 120, 160, 255); txt(x + 12, y + 8, 3, title, 255, 255, 255); fillr(x + 8, y + 40, w - 16, 2, 120, 160, 255, 255); }
static void draw_menu(void) { Page *pg = &pages[menu_page]; int px = 8, py = 8, pw = OW - 16, ph = OH - 16, sb = sc_b(), rowh = 8 * sb + 6, rows = menu_rows(); char v[64], cnt[32]; draw_panel(px, py, pw, ph, pg->title); snprintf(cnt, sizeof cnt, "%d/%d", menu_sel[menu_page] + 1, pg->n); txt_r(px + pw - 14, py + 14, 2, cnt, 170, 190, 230); int top = menu_top[menu_page]; for (int r = 0; r < rows && top + r < pg->n; r++) { int idx = top + r, ry = py + 52 + r * rowh; MItem *it = &pg->items[idx]; int sel = idx == menu_sel[menu_page]; if (sel) fillr(px + 8, ry - 3, pw - 32, rowh, 50, 90, 170, 255); int g = sel ? 255 : 205; txt(px + 16, ry, sb, it->label, g, g, g); item_value(it, v, sizeof v); if (v[0]) txt_r(px + pw - 26, ry, sb, v, 255, 230, 120); } if (pg->n > rows) { int tx = px + pw - 14, ty = py + 48, th = rows * rowh; fillr(tx, ty, 6, th, 50, 60, 90, 255); int bh = th * rows / pg->n, by = ty + (th - bh) * top / (pg->n - rows); fillr(tx, by, 6, bh, 190, 210, 255, 255); } txt(px + 12, py + ph - 20, 1 + (C.ui_big ? 1 : 0), C.ui_big ? "HAUT/BAS  GAUCHE/DROITE  OK  RETOUR" : "HAUT/BAS CHOISIR  GAUCHE/DROITE REGLER  OK VALIDER  RETOUR", 170, 190, 230); }
static void draw_calib(void) { char b[160], c[64]; int sb = sc_b(); fillr(0, OH - 150, OW, 130, 0, 0, 0, 215); snprintf(b, sizeof b, "ETALONNAGE POINT %d", calib_point); txt_c(OH - 146, 2, b, 255, 255, 120); txt_c(OH - 124, 1, "VISEZ UNE CIBLE DE TEMPERATURE CONNUE AVEC LE VISEUR", 230, 230, 230); fmt_temp(c, sizeof c, calib_ref, 1); snprintf(b, sizeof b, "REELLE: %s", c); txt_c(OH - 106, sb, b, 255, 255, 255); fmt_temp(c, sizeof c, current_value(), 1); snprintf(b, sizeof b, "MESURE: %s", c); txt_c(OH - 106 + 8 * sb + 6, sb, b, 140, 220, 255); txt_c(OH - 32, 1, "HAUT/BAS +-1  GAUCHE/DROITE +-0.1  OK VALIDER  RETOUR ANNULER", 200, 200, 200); }
static void draw_info(void) { char b[128]; int px = 8, py = 8, pw = OW - 16, ph = OH - 16, y = py + 50, lh = 19; SDL_RendererInfo ri; draw_panel(px, py, pw, ph, "INFOS CAMERA"); txt(px + 14, y, 2, "UNI-T UTI120MOBILE 5656:1201", 255, 255, 255); y += lh + 2; snprintf(b, sizeof b, "IMAGE N %u  AFFICHEES %d FPS", ui_counter, fps_now); txt(px + 14, y, 2, b, 210, 210, 210); y += lh; snprintf(b, sizeof b, "CAMERA %ld IMAGES %ld PAUSES", (long)frames_cam, (long)stalls); txt(px + 14, y, 2, b, 210, 210, 210); y += lh; snprintf(b, sizeof b, "ECART MAX %ld MS  RECONNEXIONS %ld", (long)max_gap, (long)reconnects); txt(px + 14, y, 2, b, 210, 210, 210); y += lh; snprintf(b, sizeof b, "T INTERNES %.1f %.1f %.1f %.1f", tw_ui[0] / 100, tw_ui[1] / 100, tw_ui[2] / 100, tw_ui[3] / 100); txt(px + 14, y, 2, b, 210, 210, 210); y += lh; if (!regs_ready) snprintf(b, sizeof b, "REGISTRES: LECTURE..."); else if (regs_ok) snprintf(b, sizeof b, "REG %08X %08X", regs_val[0], regs_val[1]); else snprintf(b, sizeof b, "REGISTRES: LECTURE IMPOSSIBLE"); txt(px + 14, y, 2, b, 170, 200, 240); y += lh; if (regs_ready && regs_ok) { snprintf(b, sizeof b, "REG %08X %08X", regs_val[2], regs_val[3]); txt(px + 14, y, 2, b, 170, 200, 240); y += lh; } snprintf(b, sizeof b, "SENSIBILITE %.4f DECALAGE %.2f", C.gain, C.bias); txt(px + 14, y, 2, b, 170, 200, 240); y += lh; snprintf(b, sizeof b, "EMISSIVITE %.2f AMBIANTE %.1f", C.emis, C.tamb); txt(px + 14, y, 2, b, 170, 200, 240); y += lh; snprintf(b, sizeof b, "T MOY %.1f  T OBT %.1f  QUALITE %s", t_avg, tw_ui[C.tref_src] / 100.0f, o_qual[C.quality]); txt(px + 14, y, 2, b, 170, 200, 240); y += lh; if (SDL_GetRendererInfo(ren, &ri) == 0) { snprintf(b, sizeof b, "RENDU SDL: %s", ri.name); txt(px + 14, y, 2, b, 170, 200, 240); } y += lh + 6; txt(px + 14, y, 2, "TEMPERATURES APPROXIMATIVES.", 255, 200, 120); y += lh; txt(px + 14, y, 2, "ETALONNEZ SUR UNE CIBLE CONNUE", 255, 200, 120); y += lh; txt(px + 14, y, 2, "(EAU GLACEE = 0 C).", 255, 200, 120); txt(px + 12, py + ph - 22, 2, "OK / RETOUR", 170, 190, 230); }
static void draw_help(void) { char b[96], nm[32]; int px = 8, py = 8, pw = OW - 16, ph = OH - 16; draw_panel(px, py, pw, ph, "AIDE"); int y = py + 48, colx[2] = {px + 14, px + pw / 2 + 4}; static const int order[NACT_COUNT] = {A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK, A_MENU, A_CAPTURE, A_PALETTE, A_PAL_PREV, A_ZOOM_IN, A_ZOOM_OUT, A_FREEZE, A_RECAL, A_HUD, A_RANGE, A_CENTER, A_REF, A_MEASURE, A_HELP, A_GALLERY, A_LEVEL_UP, A_LEVEL_DOWN, A_SPAN_UP, A_SPAN_DOWN}; int n = 0; for (int i = 0; i < NACT_COUNT; i++) { int a = order[i]; if (C.bind[a] < 0) continue; input_name(C.bind[a], nm, sizeof nm); char *sp = strstr(nm, "BOUTON "); if (sp) { memmove(nm, "B", 1); memmove(nm + 1, nm + 7, strlen(nm + 7) + 1); } snprintf(b, sizeof b, "%s %s", nm, act_name[a]); int col = n >= 13, row = n % 13; if (n >= 26) break; txt(colx[col], y + row * 20, 2, b, n % 2 ? 210 : 235, n % 2 ? 210 : 235, 255); n++; } if (n == 0) txt_c(py + 120, 2, "AUCUN BOUTON ASSIGNE", 255, 200, 120); int yb = py + ph - 100; txt(px + 14, yb, 2, "STICK G: VISEUR  STICK D: ZOOM/PALETTE", 150, 220, 150); txt(px + 14, yb + 20, 2, "MENU > MANETTE: AXES ET TEST ENTREES", 150, 220, 150); txt(px + 14, yb + 40, 2, "MENU > TEMPERATURE > ETALON PT 1/2", 150, 220, 150); txt(px + 14, yb + 60, 2, "= DEGRES PLUS PRECIS", 150, 220, 150); txt_r(px + pw - 14, py + ph - 22, 2, "OK / RETOUR", 170, 190, 230); }
static void draw_padtest(void) { char b[64]; fillr(0, 0, OW, OH, 14, 18, 32, 255); txt_c(8, 3, "TEST MANETTE", 255, 255, 255); for (int a = 0; a < 6; a++) { int y = 54 + a * 26; snprintf(b, sizeof b, "AXE %d", a); txt(16, y, 2, b, 210, 210, 210); rectr(110, y, 260, 18, 120, 120, 160); float d = axis_norm(a); int w = (int)(fabsf(d) * 128); fillr(d < 0 ? 240 - w : 240, y + 2, w, 14, 120, 220, 120, 255); liner(240, y, 240, y + 18, 255, 255, 255); snprintf(b, sizeof b, "%d", axis_raw[a]); txt(384, y, 2, b, 255, 230, 120); } for (int i = 0; i < 24; i++) { int x = 16 + (i % 12) * 50, y = 226 + (i / 12) * 44; int on = pad_pressed[i]; fillr(x, y, 44, 36, on ? 80 : 30, on ? 200 : 40, on ? 120 : 60, 255); rectr(x, y, 44, 36, 120, 120, 160); snprintf(b, sizeof b, "%d", i); txt(x + 22 - text_w(b, 2) / 2, y + 10, 2, b, 255, 255, 255); } snprintf(b, sizeof b, "DERNIER: %s", pad_last); txt(16, 330, 2, b, 255, 230, 120); txt(16, 358, 1, "BOUTONS 0-23 EN HAUT. LES DIRECTIONS DES STICKS (AXE N +/-) ET LA CROIX", 190, 200, 230); txt(16, 372, 1, "SONT AUSSI DES ENTREES. NOTEZ LES NUMEROS D'AXES POUR MENU > MANETTE.", 190, 200, 230); txt(16, 400, 1, "MAINTENEZ LE BOUTON RETOUR 2 SECONDES POUR QUITTER (OU ECHAP AU CLAVIER)", 255, 200, 120); if (pad_back_t) fillr(16, 420, (int)(now_ms() - pad_back_t) * 300 / 2000, 10, 255, 200, 120, 255); }
static void draw_gallery(void) { char b[96]; fillr(0, 0, OW, OH, 0, 0, 0, 255); if (gal_tex) { float s = fminf((OW) / (float)gal_w, (OH - 56) / (float)gal_h); SDL_Rect d = {(int)((OW - gal_w * s) / 2), 28, (int)(gal_w * s), (int)(gal_h * s)}; SDL_RenderCopy(ren, gal_tex, NULL, &d); } else txt_c(OH / 2 - 10, 2, gal_n ? "IMAGE ILLISIBLE" : "AUCUNE CAPTURE", 255, 255, 255); fillr(0, 0, OW, 26, 0, 0, 0, 230); snprintf(b, sizeof b, "GALERIE %d/%d  %s", gal_n ? gal_i + 1 : 0, gal_n, gal_n ? gal_name[gal_i] : ""); txt(8, 5, 2, b, 255, 255, 255); fillr(0, OH - 26, OW, 26, 0, 0, 0, 230); if (gal_confirm) txt_c(OH - 21, 2, "SUPPRIMER ? OK = OUI  AUTRE = NON", 255, 120, 120); else txt_c(OH - 19, 1, "GAUCHE/DROITE PARCOURIR   OK SUPPRIMER   RETOUR QUITTER", 200, 200, 200); }
static void draw_wizard(void) { char b[96], nm[32]; fillr(0, 0, OW, OH, 14, 18, 32, 255); txt_c(20, 3, "CONFIGURATION DES BOUTONS", 255, 255, 255); if (state == S_WIZARD) { snprintf(b, sizeof b, "%d / %d", wiz_idx + 1, NESSENTIAL); txt_c(62, 2, b, 180, 200, 255); txt_c(108, 2, "APPUYEZ SUR LE BOUTON POUR :", 230, 230, 230); txt_c(144, 4, act_name[wiz_idx], 255, 230, 120); txt_c(196, 2, act_desc[wiz_idx], 190, 210, 255); txt_c(232, 1, "BOUTONS, CROIX ET DIRECTIONS DES STICKS SONT ACCEPTES", 160, 170, 200); if (wiz_idx > A_BACK) txt_c(250, 2, "(BOUTON RETOUR = PASSER)", 200, 200, 200); for (int a = 0; a < wiz_idx && a < NACT_COUNT; a++) { input_name(C.bind[a], nm, sizeof nm); snprintf(b, sizeof b, "%s=%s", act_key[a], nm); txt(16 + (a % 3) * 210, 290 + (a / 3) * 16, 1, b, 160, 220, 160); } } else { txt_c(108, 2, "APPUYEZ SUR LE NOUVEAU BOUTON POUR :", 230, 230, 230); txt_c(144, 4, act_name[bind_act], 255, 230, 120); txt_c(196, 2, act_desc[bind_act], 190, 210, 255); txt_c(250, 2, "(BOUTON RETOUR = ANNULER)", 200, 200, 200); } if (cal_active) { snprintf(b, sizeof b, "CALIBRATION CAMERA %d%%", (int)(cal_prog * 100)); txt_c(OH - 40, 2, b, 120, 200, 255); } }
static void draw_calib_overlay(void) { fillr(0, 0, OW, OH, 0, 0, 0, 170); txt_c(OH / 2 - 56, 3, cal_label, 255, 255, 255); txt_c(OH / 2 - 24, 1, "NE DEBRANCHEZ PAS LA CAMERA", 180, 180, 180); rectr(OW / 8, OH / 2, OW * 3 / 4, 24, 255, 255, 255); fillr(OW / 8 + 3, OH / 2 + 3, (int)((OW * 3 / 4 - 6) * clampf(cal_prog, 0, 1)), 18, 255, 255, 255, 255); }
static void draw_image_layer(void) { float sc = fminf(OW / 4.0f, OH / 3.0f); SDL_Rect dst = {(int)((OW - 4 * sc) / 2), (int)((OH - 3 * sc) / 2), (int)(4 * sc), (int)(3 * sc)}; SDL_Rect src, st; view_src(&src); int q = C.quality == 2 ? HQ : 1; st = (SDL_Rect){src.x * q, src.y * q, src.w * q, src.h * q}; if (cur_tex() && ui_seq) SDL_RenderCopy(ren, cur_tex(), &st, &dst); }

/* ============ RENDER / REBUILD ============ */
static void render(long now) { SDL_GetRendererOutputSize(ren, &OW, &OH); SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE); SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); switch (state) { case S_GALLERY: draw_gallery(); draw_toast(OH - 70); SDL_RenderPresent(ren); return; case S_WIZARD: case S_BIND: draw_wizard(); draw_toast(OH - 70); SDL_RenderPresent(ren); return; case S_PADTEST: draw_padtest(); SDL_RenderPresent(ren); return; case S_HELP: draw_image_layer(); fillr(0, 0, OW, OH, 0, 0, 0, 200); draw_help(); draw_toast_top(); SDL_RenderPresent(ren); return; case S_MENU: draw_image_layer(); fillr(0, 0, OW, OH, 0, 0, 0, 200); draw_menu(); draw_toast_top(); SDL_RenderPresent(ren); return; case S_INFO: draw_image_layer(); fillr(0, 0, OW, OH, 0, 0, 0, 200); draw_info(); draw_toast_top(); SDL_RenderPresent(ren); return; default: break; } float s = fminf(OW / 4.0f, OH / 3.0f); SDL_Rect dst = {(int)((OW - 4 * s) / 2), (int)((OH - 3 * s) / 2), (int)(4 * s), (int)(3 * s)}; SDL_Rect src; view_src(&src); draw_image_layer(); draw_hud(&src, &dst); if (now - last_frame_ms > 1500 && ui_seq && !cal_active) { int blink = (now / 500) & 1; const char *m = cam_conn ? "SIGNAL CAMERA PERDU" : "CAMERA DECONNECTEE - RECONNEXION..."; fillr(0, OH / 2 - 24, OW, 44, 0, 0, 0, 200); txt_c(OH / 2 - 14, 2, m, 255, blink ? 90 : 255, blink ? 90 : 120); } if (state == S_VIEW && !cap_pending) draw_toast(OH - 70); if (cap_pending && state == S_VIEW) { cap_pending = 0; do_capture(); } if (state == S_CALIB) { draw_calib(); draw_toast(48); } if (cal_active) draw_calib_overlay(); SDL_RenderPresent(ren); }
static long build_ms_sum = 0; static int build_n = 0, build_fallback = 0;
static void rebuild_image(void) { compute_temps(); update_range(); long t0 = now_ms(); if (C.quality == 2) { build_hd(); SDL_UpdateTexture(tex_hd, NULL, argb_hd, HW * 4); build_ms_sum += now_ms() - t0; if (++build_n == 40) { if (build_ms_sum / 40 > 30 && !build_fallback) { C.quality = 1; build_fallback = 1; cfg_ver++; LOG("qualite HD trop lente (%ld ms): retour a DOUX", build_ms_sum / 40); toast("QUALITE HD TROP LENTE: DOUX"); } build_ms_sum = 0; build_n = 0; } } else { build_small(); SDL_UpdateTexture(tex_s, NULL, argb_s, W * 4); } apply_scale_mode(); }
static void apply_scale_mode(void) { if (tex_s) SDL_SetTextureScaleMode(tex_s, C.quality == 0 ? SDL_ScaleModeNearest : SDL_ScaleModeLinear); if (tex_hd) SDL_SetTextureScaleMode(tex_hd, SDL_ScaleModeLinear); }

/* ============ KEY MAP ============ */
static int key_to_action(int sym) { switch (sym) { case SDLK_UP: return A_UP; case SDLK_DOWN: return A_DOWN; case SDLK_LEFT: return A_LEFT; case SDLK_RIGHT: return A_RIGHT; case SDLK_RETURN: return A_OK; case SDLK_ESCAPE: case SDLK_BACKSPACE: return A_BACK; case SDLK_m: case SDLK_TAB: return A_MENU; case SDLK_c: return A_CAPTURE; case SDLK_p: return A_PALETTE; case SDLK_EQUALS: case SDLK_PLUS: return A_ZOOM_IN; case SDLK_MINUS: return A_ZOOM_OUT; case SDLK_r: return A_RECAL; case SDLK_f: return A_FREEZE; case SDLK_h: return A_HUD; case SDLK_g: return A_RANGE; case SDLK_SPACE: return A_CENTER; } return -1; }

/* ============ MAIN ============ */
static void range_adjust(float dlevel_frac, float span_mul) { if (C.range_mode != 1) { C.man_min = lo_t; C.man_max = hi_t; C.range_mode = 1; } float span = C.man_max - C.man_min, mid = (C.man_max + C.man_min) / 2; mid += dlevel_frac * span; span = clampf(span * span_mul, 1.0f, 400.0f); C.man_min = mid - span / 2; C.man_max = mid + span / 2; cfg_ver++; }

int main(int argc, char **argv) {
    int info_only = 0, force_sw = 0; const char *logpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) logpath = argv[++i];
        else if (!strcmp(argv[i], "-i")) info_only = 1;
        else if (!strcmp(argv[i], "-n")) do_nuc = 0;
        else if (!strcmp(argv[i], "-s")) force_sw = 1;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) req_mode = atoi(argv[++i]);
    }
    if (logpath) logf_ = fopen(logpath, "w");
    cfg_defaults(); cfg_load(); pal_init();
    if (usb_open()) { usb_close(); return 2; }
    uint32_t two = 2; if (send_cmd(0x04, 0xF0, &two, 1)) LOG("demarrage: pas de reponse, on essaie quand meme"); usleep(300000);
    if (info_only) { if (calibrate(do_nuc)) { LOG("ECHEC: calibration impossible"); usb_close(); return 3; } memcpy(tw_ui, tw_cal, sizeof tw_ui); int ok = 0; for (int i = 0; i < 60 && ok < 30; i++) if (next_frame() == 0) { memcpy(vui, vmap, sizeof vui); compute_temps(); update_range(); ok++; } LOG("test: %d images, T min %.1f max %.1f moyenne %.1f", ok, t_min, t_max, t_avg); build_small(); static uint32_t big[HW * HH]; for (int y = 0; y < HH; y++) for (int x = 0; x < HW; x++) big[y * HW + x] = argb_s[(y / HQ) * W + x / HQ]; char p[700]; snprintf(p, sizeof p, "%s/snapshot.bmp", outdir); save_bmp(p, big, HW, HH); LOG("snapshot.bmp ecrit"); usb_close(); return ok >= 10 ? 0 : 4; }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) { LOG("SDL_Init: %s", SDL_GetError()); usb_close(); return 5; }
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    for (int i = 0; i < SDL_NumJoysticks(); i++) { SDL_Joystick *j = SDL_JoystickOpen(i); if (!joy && j) joy = j; LOG("manette %d: %s", i, j ? SDL_JoystickName(j) : "echec"); }
    if (joy) for (int a = 0; a < 8; a++) { axis_rest[a] = SDL_JoystickGetAxis(joy, a); axis_raw[a] = axis_rest[a]; }
    win = SDL_CreateWindow("uti120", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_FULLSCREEN);
    if (!win) { LOG("SDL_CreateWindow: %s", SDL_GetError()); usb_close(); return 6; }
    ren = force_sw ? NULL : SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { LOG("SDL_CreateRenderer: %s", SDL_GetError()); usb_close(); return 7; }
    { SDL_RendererInfo ri; if (SDL_GetRendererInfo(ren, &ri) == 0) LOG("rendu SDL: %s (flags %u)", ri.name, ri.flags); }
    SDL_ShowCursor(0); font_init(); SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    tex_s = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    tex_hd = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, HW, HH);
    if (!tex_s || !tex_hd) { LOG("SDL_CreateTexture: %s", SDL_GetError()); usb_close(); return 8; }
    apply_scale_mode(); menu_init(); capture_init();
    cap_screen_w = 1280; cap_screen_h = 720; cap_screen_buf = (uint32_t *)malloc(cap_screen_w * cap_screen_h * 4);
    cam_tfilter = C.tfilter; cam_deadpix = C.deadpix; cam_cmd = (do_nuc && C.nuc_start) ? 2 : 1;
    pthread_t th; if (pthread_create(&th, NULL, cam_thread, NULL) != 0) { LOG("pthread_create a echoue"); usb_close(); return 9; }
    if (!cfg_bound) { wiz_idx = 0; state = S_WIZARD; }
    long t0 = now_ms(), fps_t = t0, last_trend = 0, last_cal_end = t0; int fps_cnt = 0, ui_ver = 0, frames_ui = 0; memset(held, 0, sizeof held); last_render = 0; LOG("affichage demarre");
    while (run) {
        long now = now_ms(); int dirty = 0; SDL_Event e;
        while (SDL_PollEvent(&e)) {
            dirty = 1;
            switch (e.type) {
                case SDL_QUIT: run = 0; break;
                case SDL_KEYDOWN: {
                    int a = key_to_action(e.key.keysym.sym);
                    if (a < 0) break;
                    if (e.key.repeat && a > A_RIGHT) break;
                    if (state == S_WIZARD || state == S_BIND) { if (a == A_BACK && state == S_BIND) state = S_MENU; break; }
                    if (state == S_PADTEST) { if (a == A_BACK) state = S_MENU; break; }
                    dispatch(a); break;
                }
                case SDL_JOYBUTTONDOWN: if (e.jbutton.button < 64) input_down(e.jbutton.button); break;
                case SDL_JOYBUTTONUP: if (e.jbutton.button < 64) input_up(e.jbutton.button); break;
                case SDL_JOYHATMOTION: hat_event(e.jhat.value); break;
                case SDL_JOYAXISMOTION: axis_event(e.jaxis.axis, e.jaxis.value); break;
            }
        }
        pad_back_t = 0;
        for (int id = 0; id < NID; id++) {
            if (!held[id]) continue;
            if (state == S_PADTEST && find_action(id) == A_BACK) { pad_back_t = held[id]; if (now - held[id] > 2000) { state = S_MENU; held[id] = 0; pad_back_t = 0; } continue; }
            int ri = held_act[id] >= 0 ? repeat_interval(held_act[id]) : 0;
            if (ri && now >= hold_next[id] && state != S_WIZARD && state != S_BIND) {
                if (held_act[id] >= A_UP && held_act[id] <= A_RIGHT && state == S_VIEW) move_step = clampi(1 + (int)((now - held[id]) / 700), 1, 4);
                dispatch(held_act[id]); move_step = 1; hold_next[id] = now + ri; dirty = 1;
            }
            if (id < 64 && now - held[id] > 5000) { LOG("appui tres long entree %d: arret", id); run = 0; }
        }
        process_sticks(now);
        if (state == S_VIEW && (spot_dx != (int)(spot_fx + 0.5f) || spot_dy != (int)(spot_fy + 0.5f))) dirty = 1;
        if (cam_msg_new) { pthread_mutex_lock(&mtx); toast("%s", cam_msg); cam_msg_new = 0; pthread_mutex_unlock(&mtx); dirty = 1; }
        cam_tfilter = C.tfilter; cam_deadpix = C.deadpix;
        if (C.autocal && state == S_VIEW && !cal_active && cam_cmd == 0 && calibrated && now - last_cal_end > (long[]){0, 60000, 180000, 300000, 600000}[C.autocal]) { cam_cmd = 1; last_cal_end = now; }
        if (cal_active) last_cal_end = now;
        int got_new = 0;
        pthread_mutex_lock(&mtx);
        if (sh.seq != ui_seq && !frozen) { memcpy(vui, sh.v, sizeof vui); memcpy(tw_ui, sh.twcal, sizeof tw_ui); ui_seq = sh.seq; ui_counter = sh.counter; got_new = 1; } else if (sh.seq != ui_seq && frozen) { memcpy(tw_ui, sh.twcal, sizeof tw_ui); }
        pthread_mutex_unlock(&mtx);
        if (got_new) { last_frame_ms = now; fps_cnt++; frames_ui++; }
        if (ui_seq && (got_new || ui_ver != cfg_ver)) { ui_ver = cfg_ver; rebuild_image(); dirty = 1; }
        if (got_new && now - last_trend >= 250) { trend_push(current_value()); last_trend = now; }
        if (now - fps_t >= 1000) { fps_now = fps_cnt; fps_cnt = 0; fps_t = now; battery_update(now); dirty = 1; }
        if ((now < toast_until || cal_active) && now - last_render > 33) dirty = 1;
        if (dirty) { render(now); last_render = now; }
        SDL_WaitEventTimeout(NULL, dirty ? 1 : 16);
    }
    cfg_save();
    LOG("fin: %d images affichees en %.1f s; camera: %ld images, %ld pauses, ecart max %ld ms, %ld reconnexions", frames_ui, (now_ms() - t0) / 1000.0, (long)frames_cam, (long)stalls, (long)max_gap, (long)reconnects);
    cam_run = 0; pthread_join(th, NULL);
    if (gal_tex) SDL_DestroyTexture(gal_tex); if (cap_screen_buf) free(cap_screen_buf);
    SDL_DestroyTexture(tex_s); SDL_DestroyTexture(tex_hd); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit(); usb_close();
    return 0;
}
END_OF_UTI120_C

echo ""
echo "✅ Fichiers créés dans $PORT_DIR :"
ls -la "$PORT_DIR"
echo ""
echo "🎯 Éjecte la carte SD proprement, remets-la dans la R36S, lance 'Thermique' dans Ports."
EOF