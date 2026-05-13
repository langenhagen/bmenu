#define _GNU_SOURCE
/*
Minimal stdin-driven Wayland menu.

- reads newline-separated items from stdin
- filters matches while typing
- Up/Down/PgUp/PgDn changes selection, Enter prints selection, Esc cancels

author: andreasl
*/

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <xdg-shell-client-protocol.h>

enum {
    EXIT_OK = 0,
    EXIT_CANCEL = 1,
    EXIT_ERROR = 2,
};

#define BMENU_DEFAULT_WIDTH  1024
#define BMENU_DEFAULT_HEIGHT 640
#define BMENU_FONT_FILE      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#define BMENU_FONT_SIZE      16.0

struct app;

/* Shared-memory render buffer used for one frame. */
struct buffer {
    struct wl_buffer *wl_buffer;
    uint8_t *data;
    int width;
    int height;
    int stride;
    int size;
    bool busy;
};

/* One fuzzy match: index into items + ranking score. */
struct match {
    size_t index;
    int score;
};

/* One tracked wl_output used for scale-factor updates. */
struct output {
    struct app *app;
    struct wl_output *wl_output;
    uint32_t name;
    int32_t scale;
};

/* Runtime state for a single bmenu invocation. */
struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *wm_base;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    struct buffer buffers[2];

    int width;
    int height;
    int scale;
    bool running;

    char text[2048];
    size_t text_len;

    char **items;
    size_t item_count;

    struct match *matches;
    size_t match_count;

    size_t selected;
    size_t scroll;

    const char *result;

    int32_t repeat_rate;
    int32_t repeat_delay;
    xkb_keysym_t repeat_sym;
    struct timespec repeat_at;

    struct output *outputs;
    size_t output_count;

    FT_Library ft_library;
    FT_Face ft_face;
    cairo_font_face_t *cairo_face;
};

/* Print an error and terminate with EXIT_ERROR. */
static void die(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(EXIT_ERROR);
}

/* Load the menu font once into a cached cairo font face. */
static void ensure_font(struct app *app)
{
    if (app->cairo_face != NULL) {
        return;
    }
    if (FT_Init_FreeType(&app->ft_library) != 0) {
        die("failed to initialize freetype");
    }
    if (FT_New_Face(app->ft_library, BMENU_FONT_FILE, 0, &app->ft_face) != 0) {
        die("failed to load font: " BMENU_FONT_FILE);
    }
    app->cairo_face = cairo_ft_font_face_create_for_ft_face(app->ft_face, 0);
    if (app->cairo_face == NULL
        || cairo_font_face_status(app->cairo_face) != CAIRO_STATUS_SUCCESS) {
        die("failed to create cairo font face");
    }
}

/* Create an anonymous shm-backed fd via memfd_create. */
static int create_shm_file(size_t size)
{
    int fd = memfd_create("bmenu-shm", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Mark a frame buffer as reusable once compositor releases it. */
static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    ((struct buffer *)data)->busy = false;
}

/* Buffer listener for wl_buffer release notifications. */
static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

/* Destroy one shm-backed wl_buffer and its mapping. */
static void destroy_buffer(struct buffer *buffer)
{
    if (buffer->wl_buffer != NULL) {
        wl_buffer_destroy(buffer->wl_buffer);
        buffer->wl_buffer = NULL;
    }
    if (buffer->data != NULL) {
        munmap(buffer->data, (size_t)buffer->size);
        buffer->data = NULL;
    }
    buffer->busy = false;
}

/* Allocate or resize a shm buffer to current surface dimensions. */
static void ensure_buffer(struct app *app, struct buffer *buffer)
{
    int pixel_width = app->width * app->scale;
    int pixel_height = app->height * app->scale;

    /* Reuse buffer when current size already matches surface size. */
    if (buffer->wl_buffer != NULL && buffer->width == pixel_width && buffer->height == pixel_height) {
        return;
    }

    destroy_buffer(buffer);

    buffer->width = pixel_width;
    buffer->height = pixel_height;
    buffer->stride = pixel_width * 4;
    buffer->size = buffer->stride * pixel_height;

    int fd = create_shm_file((size_t)buffer->size);
    if (fd < 0) {
        die("failed to create shared memory file");
    }

    buffer->data = mmap(NULL, (size_t)buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->data == MAP_FAILED) {
        close(fd);
        die("failed to map shared memory");
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, buffer->size);
    buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, pixel_width, pixel_height, buffer->stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
}

/* Return fuzzy score for subsequence match or -1 if no match. */
static int fuzzy_score(const char *needle, const char *haystack)
{
    /* Subsequence match with a cheap score favoring tighter spans. */
    if (needle[0] == '\0') {
        return 0;
    }

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    size_t hi = 0;
    size_t first = (size_t)-1;
    size_t last = 0;

    for (size_t ni = 0; ni < nlen; ni++) {
        unsigned char nc = (unsigned char)needle[ni];
        bool found = false;
        while (hi < hlen) {
            unsigned char hc = (unsigned char)haystack[hi];
            if (tolower(nc) == tolower(hc)) {
                if (first == (size_t)-1) {
                    first = hi;
                }
                last = hi;
                hi++;
                found = true;
                break;
            }
            hi++;
        }
        if (!found) {
            return -1;
        }
    }

    int span = (int)(last - first);
    int len_penalty = (int)(hlen - nlen);
    return span * 4 + len_penalty;
}

/* Sort matches by score, then preserve input order as tiebreaker. */
static int compare_matches(const void *a, const void *b)
{
    const struct match *ma = a;
    const struct match *mb = b;
    if (ma->score != mb->score) {
        return ma->score - mb->score;
    }
    if (ma->index < mb->index) {
        return -1;
    }
    if (ma->index > mb->index) {
        return 1;
    }
    return 0;
}

/* Rebuild and sort match list from current query text. */
static void recompute_matches(struct app *app)
{
    app->match_count = 0;
    for (size_t i = 0; i < app->item_count; i++) {
        int score = fuzzy_score(app->text, app->items[i]);
        if (score >= 0) {
            app->matches[app->match_count].index = i;
            app->matches[app->match_count].score = score;
            app->match_count++;
        }
    }

    qsort(app->matches, app->match_count, sizeof(app->matches[0]), compare_matches);

    if (app->selected >= app->match_count) {
        app->selected = app->match_count > 0 ? app->match_count - 1 : 0;
    }
    if (app->scroll > app->selected) {
        app->scroll = app->selected;
    }
}

/* Keep selected match row within current scroll window. */
static void ensure_visible(struct app *app, size_t visible_rows)
{
    if (visible_rows == 0 || app->match_count == 0) {
        return;
    }
    if (app->selected < app->scroll) {
        app->scroll = app->selected;
    } else if (app->selected >= app->scroll + visible_rows) {
        app->scroll = app->selected - visible_rows + 1;
    }
}

/* Render input line and visible matches, then commit frame. */
static void draw(struct app *app)
{
    struct buffer *buffer = NULL;
    for (size_t i = 0; i < 2; i++) {
        if (!app->buffers[i].busy) {
            buffer = &app->buffers[i];
            break;
        }
    }
    if (buffer == NULL) {
        return;
    }

    ensure_buffer(app, buffer);

    int pixel_width = app->width * app->scale;
    int pixel_height = app->height * app->scale;

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        buffer->data, CAIRO_FORMAT_ARGB32, pixel_width, pixel_height, buffer->stride);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, app->scale, app->scale);

    cairo_set_source_rgb(cr, 0.08, 0.09, 0.11);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.22, 0.24, 0.27);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, app->width - 1.0, app->height - 1.0);
    cairo_stroke(cr);

    ensure_font(app);

    cairo_font_options_t *font_options = cairo_font_options_create();
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(font_options, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(font_options, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(cr, font_options);
    cairo_font_options_destroy(font_options);

    cairo_set_font_face(cr, app->cairo_face);
    cairo_set_font_size(cr, BMENU_FONT_SIZE);

    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);

    const int input_y = 12;
    const int row_h = 26;
    const int list_y = 44;
    const int padding_x = 12;
    size_t visible_rows = (size_t)((app->height - list_y - 8) / row_h);
    ensure_visible(app, visible_rows);

    cairo_set_source_rgb(cr, 0.93, 0.94, 0.96);
    cairo_move_to(cr, padding_x, input_y + fe.ascent);
    cairo_show_text(cr, app->text);

    cairo_text_extents_t te;
    cairo_text_extents(cr, app->text, &te);
    int caret_x = padding_x + (int)(te.x_advance + 0.5) + 1;
    int caret_h = (int)(fe.height + 0.5);
    cairo_set_source_rgb(cr, 0.65, 0.69, 0.75);
    cairo_move_to(cr, caret_x + 0.5, input_y);
    cairo_line_to(cr, caret_x + 0.5, input_y + (caret_h > 0 ? caret_h : 18));
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.20, 0.22, 0.25);
    cairo_move_to(cr, 0, list_y - 5.5);
    cairo_line_to(cr, app->width, list_y - 5.5);
    cairo_stroke(cr);

    for (size_t row = 0; row < visible_rows; row++) {
        size_t mi = app->scroll + row;
        if (mi >= app->match_count) {
            break;
        }

        int y = list_y + (int)row * row_h;
        if (mi == app->selected) {
            cairo_set_source_rgb(cr, 0.16, 0.25, 0.36);
            cairo_rectangle(cr, 6, y - 2, app->width - 12, row_h);
            cairo_fill(cr);
        }

        cairo_set_source_rgb(cr, 0.93, 0.94, 0.96);
        cairo_move_to(cr, padding_x, y + 2 + fe.ascent);
        cairo_show_text(cr, app->items[app->matches[mi].index]);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);

    wl_surface_set_buffer_scale(app->surface, app->scale);
    wl_surface_attach(app->surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(app->surface, 0, 0, pixel_width, pixel_height);
    wl_surface_commit(app->surface);
    buffer->busy = true;
}

/* Recompute app scale from known outputs (pick highest scale). */
static void recompute_scale(struct app *app)
{
    int scale = 1;
    for (size_t i = 0; i < app->output_count; i++) {
        if (app->outputs[i].scale > scale) {
            scale = app->outputs[i].scale;
        }
    }
    app->scale = scale;
}

/* Required callback; output geometry is not used. */
static void output_geometry(void *data,
                            struct wl_output *wl_output,
                            int32_t x,
                            int32_t y,
                            int32_t physical_width,
                            int32_t physical_height,
                            int32_t subpixel,
                            const char *make,
                            const char *model,
                            int32_t transform)
{
    (void)data;
    (void)wl_output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

/* Required callback; output mode is not used. */
static void output_mode(void *data,
                        struct wl_output *wl_output,
                        uint32_t flags,
                        int32_t width,
                        int32_t height,
                        int32_t refresh)
{
    (void)data;
    (void)wl_output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

/* Required callback; output done is not used. */
static void output_done(void *data, struct wl_output *wl_output)
{
    (void)data;
    (void)wl_output;
}

/* Track output scale updates and redraw using new scale. */
static void output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct output *output = data;
    struct app *app = output->app;
    (void)wl_output;
    if (factor < 1) {
        factor = 1;
    }
    output->scale = factor;
    recompute_scale(app);
    if (app->surface != NULL) {
        draw(app);
    }
}

/* Output listener used to capture display scale changes. */
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = NULL,
    .description = NULL,
};

/* Reply to compositor ping to keep client responsive. */
static void wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

/* Listener set for xdg_wm_base ping events. */
static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

/* Ack surface configure and trigger redraw. */
static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    draw(app);
}

/* Listener set for xdg_surface configure events. */
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

/* Track compositor-provided toplevel size changes. */
static void toplevel_configure(void *data,
                               struct xdg_toplevel *xdg_toplevel,
                               int32_t width,
                               int32_t height,
                               struct wl_array *states)
{
    (void)xdg_toplevel;
    (void)states;
    struct app *app = data;
    if (width > 0) {
        app->width = width;
    }
    if (height > 0) {
        app->height = height;
    }
}

/* Stop main loop when compositor requests close. */
static void toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    (void)xdg_toplevel;
    ((struct app *)data)->running = false;
}

/* Listener set for xdg_toplevel resize/close events. */
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = NULL,
    .wm_capabilities = NULL,
};

/* Load xkb keymap and rebuild xkb state from compositor data. */
static void keyboard_keymap(void *data,
                            struct wl_keyboard *keyboard,
                            uint32_t format,
                            int fd,
                            uint32_t size)
{
    (void)keyboard;
    struct app *app = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *keymap_string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (keymap_string == MAP_FAILED) {
        close(fd);
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        app->xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(keymap_string, size);
    close(fd);
    if (keymap == NULL) {
        return;
    }

    struct xkb_state *state = xkb_state_new(keymap);
    if (state == NULL) {
        xkb_keymap_unref(keymap);
        return;
    }

    if (app->xkb_state != NULL) {
        xkb_state_unref(app->xkb_state);
    }
    if (app->xkb_keymap != NULL) {
        xkb_keymap_unref(app->xkb_keymap);
    }
    app->xkb_keymap = keymap;
    app->xkb_state = state;
}

/* Required callback; keyboard focus entered this surface. */
static void keyboard_enter(void *data,
                           struct wl_keyboard *keyboard,
                           uint32_t serial,
                           struct wl_surface *surface,
                           struct wl_array *keys)
{
    /* Required callback; no state needed. */
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

/* Required callback; keyboard focus left this surface. Disarm key repeat. */
static void keyboard_leave(void *data,
                           struct wl_keyboard *keyboard,
                           uint32_t serial,
                           struct wl_surface *surface)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    struct app *app = data;
    app->repeat_sym = XKB_KEY_NoSymbol;
}

/* Finalize current highlighted match as output selection. */
static void select_current(struct app *app)
{
    if (app->match_count == 0) {
        return;
    }
    app->result = app->items[app->matches[app->selected].index];
    app->running = false;
}

/* Copy selected row text into input field (source of truth). */
static void set_text_from_selected(struct app *app)
{
    if (app->match_count == 0) {
        return;
    }

    const char *value = app->items[app->matches[app->selected].index];
    size_t len = strlen(value);
    if (len >= sizeof(app->text)) {
        len = sizeof(app->text) - 1;
    }

    memcpy(app->text, value, len);
    app->text[len] = '\0';
    app->text_len = len;
}

/* Clear the entire query text (Ctrl+U). */
static void clear_text(struct app *app)
{
    if (app->text_len == 0) {
        return;
    }
    app->text_len = 0;
    app->text[0] = '\0';
    app->selected = 0;
    app->scroll = 0;
    recompute_matches(app);
    draw(app);
}

/* Delete the trailing word from query text (Ctrl+W).
   Boundary is ASCII whitespace; safe on UTF-8 since ASCII bytes
   never appear inside a multi-byte sequence. */
static void delete_word(struct app *app)
{
    if (app->text_len == 0) {
        return;
    }
    while (app->text_len > 0 && (app->text[app->text_len - 1] == ' '
                                 || app->text[app->text_len - 1] == '\t')) {
        app->text_len--;
    }
    while (app->text_len > 0 && app->text[app->text_len - 1] != ' '
                             && app->text[app->text_len - 1] != '\t') {
        app->text_len--;
    }
    app->text[app->text_len] = '\0';
    app->selected = 0;
    app->scroll = 0;
    recompute_matches(app);
    draw(app);
}

/* Apply a single key action (used for both initial press and held-key repeat). */
static void handle_key(struct app *app, xkb_keysym_t sym)
{
    if (sym == XKB_KEY_Escape) {
        app->running = false;
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        select_current(app);
        return;
    }

    bool ctrl = xkb_state_mod_name_is_active(
        app->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;

    if (ctrl) {
        if (sym == XKB_KEY_w || sym == XKB_KEY_W) {
            delete_word(app);
            return;
        }
        if (sym == XKB_KEY_u || sym == XKB_KEY_U) {
            clear_text(app);
            return;
        }
        /* Swallow other ctrl combos so they don't fall through to text input. */
        return;
    }

    if (sym == XKB_KEY_Down) {
        if (app->match_count > 0 && app->selected + 1 < app->match_count) {
            app->selected++;
            set_text_from_selected(app);
            draw(app);
        }
        return;
    }
    if (sym == XKB_KEY_Up) {
        if (app->match_count > 0 && app->selected > 0) {
            app->selected--;
            set_text_from_selected(app);
            draw(app);
        }
        return;
    }
    if (sym == XKB_KEY_Page_Down || sym == XKB_KEY_KP_Page_Down) {
        if (app->match_count > 0) {
            size_t last = app->match_count - 1;
            size_t next = app->selected + 10;
            if (next > last) next = last;
            if (next != app->selected) {
                app->selected = next;
                set_text_from_selected(app);
                draw(app);
            }
        }
        return;
    }
    if (sym == XKB_KEY_Page_Up || sym == XKB_KEY_KP_Page_Up) {
        if (app->match_count > 0) {
            size_t prev = app->selected > 10 ? app->selected - 10 : 0;
            if (prev != app->selected) {
                app->selected = prev;
                set_text_from_selected(app);
                draw(app);
            }
        }
        return;
    }
    if (sym == XKB_KEY_BackSpace) {
        if (app->text_len > 0) {
            /* Remove one UTF-8 codepoint from query text. */
            app->text_len--;
            while (app->text_len > 0 && ((app->text[app->text_len] & 0xC0) == 0x80)) {
                app->text_len--;
            }
            app->text[app->text_len] = '\0';
            app->selected = 0;
            app->scroll = 0;
            recompute_matches(app);
            draw(app);
        }
        return;
    }

    uint32_t codepoint = xkb_keysym_to_utf32(sym);
    if (codepoint < 0x20 || codepoint > 0x10FFFF || app->text_len + 4 >= sizeof(app->text)) {
        return;
    }

    if (codepoint < 0x80) {
        app->text[app->text_len++] = (char)codepoint;
    } else if (codepoint < 0x800) {
        app->text[app->text_len++] = (char)(0xC0 | (codepoint >> 6));
        app->text[app->text_len++] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        app->text[app->text_len++] = (char)(0xE0 | (codepoint >> 12));
        app->text[app->text_len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        app->text[app->text_len++] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        app->text[app->text_len++] = (char)(0xF0 | (codepoint >> 18));
        app->text[app->text_len++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        app->text[app->text_len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        app->text[app->text_len++] = (char)(0x80 | (codepoint & 0x3F));
    }

    app->text[app->text_len] = '\0';
    app->selected = 0;
    app->scroll = 0;
    recompute_matches(app);
    draw(app);
}

/* True if a sym should auto-repeat while held (Enter/Esc never repeat). */
static bool sym_repeats(xkb_keysym_t sym)
{
    if (sym == XKB_KEY_Escape) return false;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) return false;
    return sym != XKB_KEY_NoSymbol;
}

/* Compute target = now + ms, using CLOCK_MONOTONIC. */
static void timespec_after_ms(struct timespec *target, int32_t ms)
{
    clock_gettime(CLOCK_MONOTONIC, target);
    target->tv_sec += ms / 1000;
    target->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (target->tv_nsec >= 1000000000L) {
        target->tv_sec += 1;
        target->tv_nsec -= 1000000000L;
    }
}

/* Translate raw event key code to a keysym using current xkb state. */
static xkb_keysym_t key_to_sym(struct app *app, uint32_t key)
{
    if (app->xkb_state == NULL) {
        return XKB_KEY_NoSymbol;
    }
    return xkb_state_key_get_one_sym(app->xkb_state, key + 8);
}

/* Handle key press/release: dispatch action and arm/disarm key repeat. */
static void keyboard_key(void *data,
                         struct wl_keyboard *keyboard,
                         uint32_t serial,
                         uint32_t time,
                         uint32_t key,
                         uint32_t state)
{
    (void)keyboard;
    (void)serial;
    (void)time;

    struct app *app = data;
    if (app->xkb_state == NULL) {
        return;
    }

    xkb_keysym_t sym = key_to_sym(app, key);

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (sym == app->repeat_sym) {
            app->repeat_sym = XKB_KEY_NoSymbol;
        }
        return;
    }

    handle_key(app, sym);

    if (app->running && app->repeat_rate > 0 && sym_repeats(sym)) {
        app->repeat_sym = sym;
        timespec_after_ms(&app->repeat_at, app->repeat_delay);
    } else {
        app->repeat_sym = XKB_KEY_NoSymbol;
    }
}

/* Apply keyboard modifier state updates to xkb state. */
static void keyboard_modifiers(void *data,
                               struct wl_keyboard *keyboard,
                               uint32_t serial,
                               uint32_t mods_depressed,
                               uint32_t mods_latched,
                               uint32_t mods_locked,
                               uint32_t group)
{
    (void)keyboard;
    (void)serial;

    struct app *app = data;
    if (app->xkb_state == NULL) {
        return;
    }
    xkb_state_update_mask(app->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

/* Capture compositor key-repeat preferences; rate<=0 keeps current defaults. */
static void keyboard_repeat_info(void *data,
                                 struct wl_keyboard *keyboard,
                                 int32_t rate,
                                 int32_t delay)
{
    (void)keyboard;
    struct app *app = data;
    if (rate > 0) {
        app->repeat_rate = rate;
        app->repeat_delay = delay > 0 ? delay : app->repeat_delay;
    }
}

/* enter/leave are required by the listener but intentionally no-op. */
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* Bind or release keyboard object as seat capabilities change. */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct app *app = data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && app->keyboard == NULL) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && app->keyboard != NULL) {
        wl_keyboard_destroy(app->keyboard);
        app->keyboard = NULL;
    }
}

/* Required callback; seat name events are ignored. */
static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    /* Required callback; seat name not used. */
    (void)data;
    (void)seat;
    (void)name;
}

/* Listener set for wl_seat keyboard capability updates. */
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* Bind Wayland globals needed by bmenu. */
static void registry_global(void *data,
                            struct wl_registry *registry,
                            uint32_t name,
                            const char *interface,
                            uint32_t version)
{
    struct app *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        uint32_t output_version = version < 2 ? version : 2;
        struct wl_output *output_obj = wl_registry_bind(registry, name, &wl_output_interface, output_version);

        struct output *new_outputs = realloc(app->outputs, (app->output_count + 1) * sizeof(app->outputs[0]));
        if (new_outputs == NULL) {
            wl_output_destroy(output_obj);
            die("out of memory");
        }
        app->outputs = new_outputs;
        app->outputs[app->output_count].app = app;
        app->outputs[app->output_count].wl_output = output_obj;
        app->outputs[app->output_count].name = name;
        app->outputs[app->output_count].scale = 1;
        wl_output_add_listener(output_obj, &output_listener, &app->outputs[app->output_count]);
        app->output_count++;
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        uint32_t seat_version = version < 5 ? version : 5;
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, seat_version);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    }
}

/* Required callback; globals are not dynamically removed here. */
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    struct app *app = data;
    (void)registry;

    for (size_t i = 0; i < app->output_count; i++) {
        if (app->outputs[i].name == name) {
            wl_output_destroy(app->outputs[i].wl_output);
            if (i + 1 < app->output_count) {
                memmove(&app->outputs[i], &app->outputs[i + 1], (app->output_count - i - 1) * sizeof(app->outputs[0]));
            }
            app->output_count--;
            recompute_scale(app);
            break;
        }
    }
}

/* Listener set for wl_registry global add/remove events. */
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* Free stdin item storage and match arrays. */
static void free_items(struct app *app)
{
    for (size_t i = 0; i < app->item_count; i++) {
        free(app->items[i]);
    }
    free(app->items);
    free(app->matches);
}

/* Release all Wayland, xkb and heap resources. */
static void cleanup(struct app *app)
{
    for (size_t i = 0; i < 2; i++) {
        destroy_buffer(&app->buffers[i]);
    }

    if (app->xkb_state != NULL) {
        xkb_state_unref(app->xkb_state);
    }
    if (app->xkb_keymap != NULL) {
        xkb_keymap_unref(app->xkb_keymap);
    }
    if (app->xkb_context != NULL) {
        xkb_context_unref(app->xkb_context);
    }
    if (app->keyboard != NULL) {
        wl_keyboard_destroy(app->keyboard);
    }
    if (app->seat != NULL) {
        wl_seat_destroy(app->seat);
    }
    for (size_t i = 0; i < app->output_count; i++) {
        wl_output_destroy(app->outputs[i].wl_output);
    }
    free(app->outputs);
    if (app->xdg_toplevel != NULL) {
        xdg_toplevel_destroy(app->xdg_toplevel);
    }
    if (app->xdg_surface != NULL) {
        xdg_surface_destroy(app->xdg_surface);
    }
    if (app->surface != NULL) {
        wl_surface_destroy(app->surface);
    }
    if (app->wm_base != NULL) {
        xdg_wm_base_destroy(app->wm_base);
    }
    if (app->shm != NULL) {
        wl_shm_destroy(app->shm);
    }
    if (app->compositor != NULL) {
        wl_compositor_destroy(app->compositor);
    }
    if (app->registry != NULL) {
        wl_registry_destroy(app->registry);
    }
    if (app->display != NULL) {
        wl_display_disconnect(app->display);
    }

    if (app->cairo_face != NULL) {
        cairo_font_face_destroy(app->cairo_face);
    }
    if (app->ft_face != NULL) {
        FT_Done_Face(app->ft_face);
    }
    if (app->ft_library != NULL) {
        FT_Done_FreeType(app->ft_library);
    }

    free_items(app);
}

/* Read newline-separated menu items from stdin. */
static void read_stdin_items(struct app *app)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;

    while ((nread = getline(&line, &cap, stdin)) != -1) {
        while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
            line[--nread] = '\0';
        }

        char *copy = strdup(line);
        if (copy == NULL) {
            free(line);
            die("out of memory");
        }

        char **new_items = realloc(app->items, (app->item_count + 1) * sizeof(app->items[0]));
        if (new_items == NULL) {
            free(copy);
            free(line);
            die("out of memory");
        }

        app->items = new_items;
        app->items[app->item_count++] = copy;
    }

    free(line);

    app->matches = calloc(app->item_count > 0 ? app->item_count : 1, sizeof(app->matches[0]));
    if (app->matches == NULL) {
        die("out of memory");
    }
}

/* Program entry: load items, run Wayland loop, print selection. */
int main(void)
{
    struct app app = {
        .width = BMENU_DEFAULT_WIDTH,
        .height = BMENU_DEFAULT_HEIGHT,
        .scale = 1,
        .running = true,
        .repeat_rate = 25,
        .repeat_delay = 600,
        .repeat_sym = XKB_KEY_NoSymbol,
    };

    read_stdin_items(&app);
    recompute_matches(&app);

    app.display = wl_display_connect(NULL);
    if (app.display == NULL) {
        cleanup(&app);
        die("failed to connect to wayland display");
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);
    recompute_scale(&app);

    if (app.compositor == NULL || app.shm == NULL || app.wm_base == NULL) {
        cleanup(&app);
        die("missing required wayland globals");
    }

    xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, &app);

    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);

    app.xdg_toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.xdg_toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_app_id(app.xdg_toplevel, "bmenu");
    xdg_toplevel_set_title(app.xdg_toplevel, "bmenu");

    wl_surface_commit(app.surface);

    app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (app.xkb_context == NULL) {
        cleanup(&app);
        die("failed to initialize xkb context");
    }

    int display_fd = wl_display_get_fd(app.display);
    while (app.running) {
        /* Drain any already-queued events before sleeping. */
        while (wl_display_prepare_read(app.display) != 0) {
            if (wl_display_dispatch_pending(app.display) == -1) {
                app.running = false;
                break;
            }
        }
        if (!app.running) {
            break;
        }
        while (wl_display_flush(app.display) == -1) {
            if (errno == EAGAIN) {
                break;
            }
            wl_display_cancel_read(app.display);
            app.running = false;
            break;
        }
        if (!app.running) {
            break;
        }

        int timeout = -1;
        if (app.repeat_sym != XKB_KEY_NoSymbol && app.repeat_rate > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (app.repeat_at.tv_sec - now.tv_sec) * 1000L
                   + (app.repeat_at.tv_nsec - now.tv_nsec) / 1000000L;
            timeout = ms < 0 ? 0 : (int)ms;
        }

        struct pollfd pfd = { .fd = display_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0) {
            wl_display_cancel_read(app.display);
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pr > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(app.display) == -1) {
                break;
            }
        } else {
            wl_display_cancel_read(app.display);
        }

        if (wl_display_dispatch_pending(app.display) == -1) {
            break;
        }

        if (pr == 0 && app.repeat_sym != XKB_KEY_NoSymbol && app.repeat_rate > 0) {
            xkb_keysym_t sym = app.repeat_sym;
            handle_key(&app, sym);
            if (app.running && app.repeat_sym == sym) {
                int32_t period = 1000 / app.repeat_rate;
                if (period < 1) {
                    period = 1;
                }
                timespec_after_ms(&app.repeat_at, period);
            }
        }
    }

    int exit_code = EXIT_CANCEL;
    if (app.result != NULL) {
        printf("%s\n", app.result);
        fflush(stdout);
        exit_code = EXIT_OK;
    }

    cleanup(&app);
    return exit_code;
}
