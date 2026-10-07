#define _POSIX_C_SOURCE 200809L

#include <gtk/gtk.h>
#include <cairo.h>
#include <mupdf/fitz.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define APP_ID "com.leafturn.Leafturn"
#define APP_NAME "Leafturn"
#define WINDOW_W 1120
#define WINDOW_H 780
#define TURN_DURATION_MS 230
#define DEFAULT_DPI 144.0f

typedef struct {
    char *path;
    char *title;
    int page;
    int page_count;
    time_t last_open;
} Book;

typedef struct {
    GPtrArray *books;
    char *state_dir;
    char *state_file;
} Library;

typedef struct {
    fz_context *ctx;
    fz_document *doc;
    char *path;
    int page_count;
    int page;
    float zoom;
    cairo_surface_t *current_surface;
    cairo_surface_t *next_surface;
    int current_w;
    int current_h;
    int next_w;
    int next_h;
} PdfView;

typedef enum {
    TURN_NONE = 0,
    TURN_FORWARD,
    TURN_BACKWARD
} TurnDirection;

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *header;
    GtkWidget *stack;
    GtkWidget *library_page;
    GtkWidget *reader_page;
    GtkWidget *library_list;
    GtkWidget *status;
    GtkWidget *canvas;
    GtkWidget *page_label;
    GtkWidget *progress;
    GtkWidget *zoom_label;
    GtkWidget *back_button;
    GtkWidget *prev_button;
    GtkWidget *next_button;
    GtkWidget *zoom_out_button;
    GtkWidget *zoom_in_button;
    GtkAccelGroup *accels;
    Library library;
    PdfView pdf;
    guint animation_source;
    gint64 animation_start_us;
    TurnDirection turn_direction;
} App;

static void book_free(gpointer data) {
    Book *b = data;
    if (!b) return;
    g_free(b->path);
    g_free(b->title);
    g_free(b);
}

static void library_free(Library *lib) {
    if (!lib) return;
    if (lib->books) g_ptr_array_free(lib->books, TRUE);
    g_free(lib->state_dir);
    g_free(lib->state_file);
    memset(lib, 0, sizeof(*lib));
}

static char *dup_base_title(const char *path) {
    char *base = g_path_get_basename(path);
    char *dot = strrchr(base, '.');
    if (dot && dot != base && g_ascii_strcasecmp(dot, ".pdf") == 0) {
        *dot = '\0';
    }
    return base;
}

static char *escape_field(const char *s) {
    GString *out = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '%' || *p == '|' || *p == '\n' || *p == '\r') {
            g_string_append_printf(out, "%%%02X", (unsigned int)*p);
        } else {
            g_string_append_c(out, (char)*p);
        }
    }
    return g_string_free(out, FALSE);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static char *unescape_field(const char *s) {
    GString *out = g_string_new(NULL);
    for (size_t i = 0; s[i] != '\0'; ++i) {
        if (s[i] == '%' && s[i + 1] && s[i + 2]) {
            int hi = hex_value(s[i + 1]);
            int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                g_string_append_c(out, (char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        g_string_append_c(out, s[i]);
    }
    return g_string_free(out, FALSE);
}

static char **split_pipe_line(const char *line) {
    return g_strsplit(line, "|", 0);
}

static Book *library_find(Library *lib, const char *path) {
    if (!path || !lib || !lib->books) return NULL;
    for (guint i = 0; i < lib->books->len; ++i) {
        Book *b = g_ptr_array_index(lib->books, i);
        if (g_strcmp0(b->path, path) == 0) return b;
    }
    return NULL;
}

static bool ensure_state_paths(Library *lib) {
    const char *state_home = g_get_user_state_dir();
    lib->state_dir = g_build_filename(state_home, "leafturn", NULL);
    lib->state_file = g_build_filename(lib->state_dir, "library.txt", NULL);
    if (g_mkdir_with_parents(lib->state_dir, 0700) != 0) {
        g_warning("Cannot create state directory %s: %s", lib->state_dir, g_strerror(errno));
        return false;
    }
    return true;
}

static void library_load(Library *lib) {
    lib->books = g_ptr_array_new_with_free_func(book_free);
    if (!ensure_state_paths(lib)) return;

    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(lib->state_file, &contents, &length, &error)) {
        if (error) {
            if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
                g_warning("Could not read %s: %s", lib->state_file, error->message);
            }
            g_error_free(error);
        }
        return;
    }

    char **lines = g_strsplit(contents, "\n", -1);
    for (char **line = lines; *line; ++line) {
        if ((*line)[0] == '\0' || (*line)[0] == '#') continue;
        char **parts = split_pipe_line(*line);
        if (!parts[0] || !parts[1] || !parts[2] || !parts[3] || !parts[4]) {
            g_strfreev(parts);
            continue;
        }
        Book *b = g_new0(Book, 1);
        b->path = unescape_field(parts[0]);
        b->title = unescape_field(parts[1]);
        b->page = (int)g_ascii_strtoll(parts[2], NULL, 10);
        b->page_count = (int)g_ascii_strtoll(parts[3], NULL, 10);
        b->last_open = (time_t)g_ascii_strtoll(parts[4], NULL, 10);
        if (b->page < 0) b->page = 0;
        if (b->page_count < 0) b->page_count = 0;
        g_ptr_array_add(lib->books, b);
        g_strfreev(parts);
    }
    g_strfreev(lines);
    g_free(contents);
    (void)length;
}

static void library_save(Library *lib) {
    if (!lib || !lib->books || !lib->state_file) return;
    GString *out = g_string_new("# Leafturn library v1\n");
    for (guint i = 0; i < lib->books->len; ++i) {
        Book *b = g_ptr_array_index(lib->books, i);
        char *ep = escape_field(b->path);
        char *et = escape_field(b->title);
        g_string_append_printf(out, "%s|%s|%d|%d|%lld\n",
                               ep, et, b->page, b->page_count, (long long)b->last_open);
        g_free(ep);
        g_free(et);
    }
    GError *error = NULL;
    if (!g_file_set_contents(lib->state_file, out->str, (gssize)out->len, &error)) {
        g_warning("Could not save library: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    g_string_free(out, TRUE);
}

static void pdf_drop(PdfView *pdf) {
    if (pdf->current_surface) cairo_surface_destroy(pdf->current_surface);
    if (pdf->next_surface) cairo_surface_destroy(pdf->next_surface);
    pdf->current_surface = NULL;
    pdf->next_surface = NULL;
    if (pdf->doc && pdf->ctx) fz_drop_document(pdf->ctx, pdf->doc);
    pdf->doc = NULL;
    g_clear_pointer(&pdf->path, g_free);
    pdf->page_count = 0;
    pdf->page = 0;
    pdf->current_w = pdf->current_h = 0;
    pdf->next_w = pdf->next_h = 0;
}

static bool pdf_init(PdfView *pdf) {
    memset(pdf, 0, sizeof(*pdf));
    pdf->zoom = 1.0f;
    pdf->ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!pdf->ctx) return false;
    fz_try(pdf->ctx) {
        fz_register_document_handlers(pdf->ctx);
    } fz_catch(pdf->ctx) {
        g_warning("MuPDF: cannot register document handlers: %s", fz_caught_message(pdf->ctx));
        fz_drop_context(pdf->ctx);
        pdf->ctx = NULL;
        return false;
    }
    return true;
}

static bool cairo_from_mupdf_pixmap(PdfView *pdf, fz_pixmap *pix,
                                     cairo_surface_t **out, int *out_w, int *out_h) {
    int w = fz_pixmap_width(pdf->ctx, pix);
    int h = fz_pixmap_height(pdf->ctx, pix);
    if (w <= 0 || h <= 0) return false;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_surface_flush(surface);
    unsigned char *dst = cairo_image_surface_get_data(surface);
    const int dst_stride = cairo_image_surface_get_stride(surface);
    unsigned char *src = fz_pixmap_samples(pdf->ctx, pix);
    const int src_stride = fz_pixmap_stride(pdf->ctx, pix);
    const int n = fz_pixmap_components(pdf->ctx, pix);
    const int has_alpha = fz_pixmap_alpha(pdf->ctx, pix);

    for (int y = 0; y < h; ++y) {
        unsigned char *d = dst + (size_t)y * (size_t)dst_stride;
        const unsigned char *s = src + (size_t)y * (size_t)src_stride;
        for (int x = 0; x < w; ++x) {
            unsigned char r = s[0];
            unsigned char g = n > 1 ? s[1] : r;
            unsigned char b = n > 2 ? s[2] : r;
            unsigned char a = has_alpha && n > 3 ? s[n - 1] : 255;
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = a;
            s += n;
            d += 4;
        }
    }
    cairo_surface_mark_dirty(surface);
    *out = surface;
    *out_w = w;
    *out_h = h;
    return true;
}

static cairo_surface_t *pdf_render_page(PdfView *pdf, int page_index, int *out_w, int *out_h) {
    if (!pdf->doc || page_index < 0 || page_index >= pdf->page_count) return NULL;

    const float dpi = DEFAULT_DPI * pdf->zoom;
    const fz_matrix ctm = fz_scale(dpi / 72.0f, dpi / 72.0f);
    fz_pixmap *pix = NULL;
    cairo_surface_t *surface = NULL;
    int w = 0;
    int h = 0;

    fz_try(pdf->ctx) {
        pix = fz_new_pixmap_from_page_number(pdf->ctx, pdf->doc, page_index,
                                              ctm, fz_device_rgb(pdf->ctx), 0);
    } fz_catch(pdf->ctx) {
        g_warning("MuPDF render failed for page %d: %s", page_index + 1, fz_caught_message(pdf->ctx));
        return NULL;
    }

    const bool ok = cairo_from_mupdf_pixmap(pdf, pix, &surface, &w, &h);
    fz_drop_pixmap(pdf->ctx, pix);
    if (!ok) return NULL;
    *out_w = w;
    *out_h = h;
    return surface;
}

static void pdf_clear_surfaces(PdfView *pdf) {
    if (pdf->current_surface) cairo_surface_destroy(pdf->current_surface);
    if (pdf->next_surface) cairo_surface_destroy(pdf->next_surface);
    pdf->current_surface = NULL;
    pdf->next_surface = NULL;
    pdf->current_w = pdf->current_h = 0;
    pdf->next_w = pdf->next_h = 0;
}

static bool pdf_open(PdfView *pdf, const char *path, int start_page) {
    if (!pdf->ctx) return false;
    fz_document *doc = NULL;
    fz_try(pdf->ctx) {
        doc = fz_open_document(pdf->ctx, path);
        const int pages = fz_count_pages(pdf->ctx, doc);
        if (pages <= 0) fz_throw(pdf->ctx, FZ_ERROR_FORMAT, "document has no pages");
        pdf_drop(pdf);
        pdf->doc = doc;
        doc = NULL;
        pdf->page_count = pages;
        pdf->page = CLAMP(start_page, 0, pages - 1);
        pdf->path = g_strdup(path);
        pdf->zoom = 1.0f;
    } fz_catch(pdf->ctx) {
        if (doc) fz_drop_document(pdf->ctx, doc);
        g_warning("MuPDF open failed: %s", fz_caught_message(pdf->ctx));
        return false;
    }
    return true;
}

static void pdf_drop_context(PdfView *pdf) {
    pdf_drop(pdf);
    if (pdf->ctx) fz_drop_context(pdf->ctx);
    pdf->ctx = NULL;
}

static void show_error_dialog(App *app, const char *primary, const char *secondary) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                               "%s", primary);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void update_reader_ui(App *app) {
    if (!app->pdf.doc) return;
    const int one_based = app->pdf.page + 1;
    const int pct = app->pdf.page_count > 0
        ? (int)llround(((double)one_based / (double)app->pdf.page_count) * 100.0)
        : 0;
    char buf[128];
    g_snprintf(buf, sizeof(buf), "Page %d of %d  ·  %d%%", one_based, app->pdf.page_count, pct);
    gtk_label_set_text(GTK_LABEL(app->page_label), buf);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), pct / 100.0);
    g_snprintf(buf, sizeof(buf), "%d%%", (int)llround((double)app->pdf.zoom * 100.0));
    gtk_label_set_text(GTK_LABEL(app->zoom_label), buf);
    gtk_widget_set_sensitive(app->back_button, app->pdf.page > 0);
    gtk_widget_set_sensitive(app->next_button, app->pdf.page < app->pdf.page_count - 1);

    Book *b = library_find(&app->library, app->pdf.path);
    if (b) {
        b->page = app->pdf.page;
        b->page_count = app->pdf.page_count;
        b->last_open = time(NULL);
    }
}

static bool render_current(App *app) {
    if (!app->pdf.doc) return false;
    pdf_clear_surfaces(&app->pdf);
    int w = 0;
    int h = 0;
    app->pdf.current_surface = pdf_render_page(&app->pdf, app->pdf.page, &w, &h);
    app->pdf.current_w = w;
    app->pdf.current_h = h;
    update_reader_ui(app);
    gtk_widget_queue_draw(app->canvas);
    return app->pdf.current_surface != NULL;
}

static bool prepare_turn(App *app, TurnDirection direction) {
    const int next_page = app->pdf.page + (direction == TURN_FORWARD ? 1 : -1);
    if (!app->pdf.doc || next_page < 0 || next_page >= app->pdf.page_count) return false;
    if (!app->pdf.current_surface && !render_current(app)) return false;

    if (app->pdf.next_surface) {
        cairo_surface_destroy(app->pdf.next_surface);
        app->pdf.next_surface = NULL;
    }

    int w = 0;
    int h = 0;
    app->pdf.next_surface = pdf_render_page(&app->pdf, next_page, &w, &h);
    app->pdf.next_w = w;
    app->pdf.next_h = h;
    if (!app->pdf.next_surface) return false;

    app->animation_start_us = g_get_monotonic_time();
    app->turn_direction = direction;
    return true;
}

static void finish_turn(App *app) {
    if (app->turn_direction == TURN_NONE) return;
    const int next_page = app->pdf.page + (app->turn_direction == TURN_FORWARD ? 1 : -1);
    app->pdf.page = next_page;

    if (app->pdf.current_surface) cairo_surface_destroy(app->pdf.current_surface);
    app->pdf.current_surface = app->pdf.next_surface;
    app->pdf.next_surface = NULL;
    app->pdf.current_w = app->pdf.next_w;
    app->pdf.current_h = app->pdf.next_h;
    app->pdf.next_w = 0;
    app->pdf.next_h = 0;
    app->turn_direction = TURN_NONE;

    update_reader_ui(app);
    library_save(&app->library);
    gtk_widget_queue_draw(app->canvas);
}

static gboolean animate_turn(gpointer user_data) {
    App *app = user_data;
    if (app->turn_direction == TURN_NONE) return G_SOURCE_REMOVE;
    const gint64 now = g_get_monotonic_time();
    const double elapsed_ms = (double)(now - app->animation_start_us) / 1000.0;
    if (elapsed_ms >= (double)TURN_DURATION_MS) {
        finish_turn(app);
        app->animation_source = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw(app->canvas);
    return G_SOURCE_CONTINUE;
}

static void begin_turn(App *app, TurnDirection direction) {
    if (app->turn_direction != TURN_NONE) return;
    if (!prepare_turn(app, direction)) return;
    app->animation_source = g_timeout_add(16, animate_turn, app);
}

static double ease_in_out(double x) {
    return x < 0.5 ? 4.0 * x * x * x : 1.0 - pow(-2.0 * x + 2.0, 3.0) / 2.0;
}

static void draw_page_surface(cairo_t *cr, cairo_surface_t *surface,
                              int page_w, int page_h,
                              double x, double y, double draw_w, double draw_h) {
    if (!surface || page_w <= 0 || page_h <= 0) return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, draw_w / (double)page_w, draw_h / (double)page_h);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_rectangle(cr, 0, 0, page_w, page_h);
    cairo_fill(cr);
    cairo_restore(cr);
}

static gboolean on_canvas_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    App *app = user_data;
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    const double W = a.width;
    const double H = a.height;

    cairo_set_source_rgb(cr, 0.09, 0.08, 0.07);
    cairo_paint(cr);

    if (!app->pdf.current_surface) {
        cairo_set_source_rgb(cr, 0.72, 0.69, 0.62);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 18.0);
        const char *hint = "Open a PDF to start reading";
        cairo_text_extents_t ext;
        cairo_text_extents(cr, hint, &ext);
        cairo_move_to(cr, (W - ext.width) / 2.0, (H - ext.height) / 2.0);
        cairo_show_text(cr, hint);
        return FALSE;
    }

    const double page_aspect = (double)app->pdf.current_w / (double)app->pdf.current_h;
    const double max_w = W * 0.78;
    const double max_h = H * 0.88;
    double draw_w = max_w;
    double draw_h = draw_w / page_aspect;
    if (draw_h > max_h) {
        draw_h = max_h;
        draw_w = draw_h * page_aspect;
    }
    const double x = (W - draw_w) / 2.0;
    const double y = (H - draw_h) / 2.0;

    cairo_set_source_rgba(cr, 0, 0, 0, 0.32);
    cairo_rectangle(cr, x + 8, y + 10, draw_w, draw_h);
    cairo_fill(cr);

    if (app->turn_direction == TURN_NONE || !app->pdf.next_surface) {
        draw_page_surface(cr, app->pdf.current_surface, app->pdf.current_w, app->pdf.current_h,
                          x, y, draw_w, draw_h);
        return FALSE;
    }

    const gint64 now = g_get_monotonic_time();
    double t = ((double)(now - app->animation_start_us) / 1000.0) / (double)TURN_DURATION_MS;
    t = CLAMP(t, 0.0, 1.0);
    const double e = ease_in_out(t);
    const bool forward = app->turn_direction == TURN_FORWARD;

    /* Destination page under the turning sheet. */
    draw_page_surface(cr, app->pdf.next_surface, app->pdf.next_w, app->pdf.next_h,
                      x, y, draw_w, draw_h);

    const double shrink = fabs(1.0 - 2.0 * e);
    const double center = forward ? x + draw_w : x;
    const double left = forward ? center - draw_w * shrink : center;
    if (shrink > 0.005) {
        draw_page_surface(cr, app->pdf.current_surface, app->pdf.current_w, app->pdf.current_h,
                          left, y, draw_w * shrink, draw_h);
    }

    const double fold_x = forward ? center - 14.0 : center + 14.0;
    cairo_pattern_t *grad = cairo_pattern_create_linear(fold_x - 28.0, 0, fold_x + 28.0, 0);
    cairo_pattern_add_color_stop_rgba(grad, 0.0, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(grad, 0.5, 0, 0, 0, 0.24);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, 0, 0, 0, 0.0);
    cairo_set_source(cr, grad);
    cairo_rectangle(cr, fold_x - 28.0, y, 56.0, draw_h);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);

    return FALSE;
}

static void rebuild_library_list(App *app);

static void remove_book_clicked(GtkButton *button, gpointer user_data) {
    App *app = user_data;
    Book *book = g_object_get_data(G_OBJECT(button), "book");
    if (!book) return;

    if (g_strcmp0(book->path, app->pdf.path) == 0) {
        pdf_drop(&app->pdf);
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "library");
        gtk_header_bar_set_title(GTK_HEADER_BAR(app->header), APP_NAME);
    }

    for (guint i = 0; i < app->library.books->len; ++i) {
        if (g_ptr_array_index(app->library.books, i) == book) {
            g_ptr_array_remove_index(app->library.books, i);
            break;
        }
    }
    library_save(&app->library);
    rebuild_library_list(app);
}

static GtkWidget *make_book_row(App *app, Book *b) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new(b->title);
    GtkWidget *detail = gtk_label_new(NULL);
    GtkWidget *bar = gtk_progress_bar_new();
    GtkWidget *remove = gtk_button_new_with_label("Remove");
    const int pct = b->page_count > 0
        ? (int)llround(((double)(b->page + 1) / (double)b->page_count) * 100.0)
        : 0;
    char buf[128];

    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "book-title");

    g_snprintf(buf, sizeof(buf), "%d%%  ·  Page %d of %d", pct, b->page + 1, b->page_count);
    gtk_label_set_text(GTK_LABEL(detail), buf);
    gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
    gtk_widget_set_hexpand(detail, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(detail), "dim-label");

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), pct / 100.0);
    gtk_widget_set_hexpand(bar, TRUE);

    gtk_button_set_relief(GTK_BUTTON(remove), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(remove), "destructive-action");
    g_object_set_data(G_OBJECT(remove), "book", b);
    g_signal_connect(remove, "clicked", G_CALLBACK(remove_book_clicked), app);

    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), bar, FALSE, FALSE, 0);

    gtk_widget_set_margin_start(outer, 16);
    gtk_widget_set_margin_end(outer, 16);
    gtk_widget_set_margin_top(outer, 12);
    gtk_widget_set_margin_bottom(outer, 12);
    gtk_container_add(GTK_CONTAINER(row), outer);
    g_object_set_data(G_OBJECT(row), "book", b);
    (void)app;
    return row;
}

static void rebuild_library_list(App *app) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->library_list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    for (guint i = 0; i < app->library.books->len; ++i) {
        Book *b = g_ptr_array_index(app->library.books, i);
        GtkWidget *row = make_book_row(app, b);
        gtk_container_add(GTK_CONTAINER(app->library_list), row);
        gtk_widget_show_all(row);
    }
}

static void open_book(App *app, Book *b);
static void on_app_activate(GtkApplication *gtk_app, gpointer user_data);

static void row_activate(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    App *app = user_data;
    Book *b = g_object_get_data(G_OBJECT(row), "book");
    if (b) open_book(app, b);
}

static bool path_is_pdf(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && g_ascii_strcasecmp(dot, ".pdf") == 0;
}

static void import_path(App *app, const char *path) {
    char *canon = realpath(path, NULL);
    if (!canon) canon = g_strdup(path);
    if (!path_is_pdf(canon)) {
        show_error_dialog(app, "That file is not a PDF.", canon);
        g_free(canon);
        return;
    }

    Book *existing = library_find(&app->library, canon);
    if (!existing) {
        Book *b = g_new0(Book, 1);
        b->path = g_strdup(canon);
        b->title = dup_base_title(canon);
        b->page = 0;
        b->page_count = 0;
        b->last_open = time(NULL);
        g_ptr_array_insert(app->library.books, 0, b);
        existing = b;
    }

    if (!pdf_open(&app->pdf, canon, existing->page)) {
        show_error_dialog(app, "Leafturn could not open that PDF.",
                          "The file may be damaged, encrypted, or unsupported.");
        if (existing->page_count == 0) {
            for (guint i = 0; i < app->library.books->len; ++i) {
                if (g_ptr_array_index(app->library.books, i) == existing) {
                    g_ptr_array_remove_index(app->library.books, i);
                    break;
                }
            }
        }
        library_save(&app->library);
        g_free(canon);
        return;
    }

    existing->page_count = app->pdf.page_count;
    existing->page = app->pdf.page;
    existing->last_open = time(NULL);
    library_save(&app->library);
    rebuild_library_list(app);
    open_book(app, existing);
    g_free(canon);
}

static void choose_pdf_response(GtkWidget *dialog, gint response_id, gpointer user_data) {
    App *app = user_data;
    if (response_id == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) {
            import_path(app, path);
            g_free(path);
        }
    }
    gtk_widget_destroy(dialog);
}

static void choose_pdf(App *app) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open PDF", GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Open", GTK_RESPONSE_ACCEPT,
        NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF documents");
    gtk_file_filter_add_mime_type(filter, "application/pdf");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    g_signal_connect(dialog, "response", G_CALLBACK(choose_pdf_response), app);
    gtk_widget_show(dialog);
}

static void open_book(App *app, Book *b) {
    if (!b) return;
    if (app->animation_source) {
        g_source_remove(app->animation_source);
        app->animation_source = 0;
    }
    app->turn_direction = TURN_NONE;
    if (!pdf_open(&app->pdf, b->path, b->page)) {
        show_error_dialog(app, "Cannot open PDF", b->path);
        return;
    }

    b->page_count = app->pdf.page_count;
    b->page = app->pdf.page;
    b->last_open = time(NULL);
    library_save(&app->library);

    char *base = g_path_get_basename(b->path);
    gtk_header_bar_set_title(GTK_HEADER_BAR(app->header), base);
    g_free(base);
    if (app->reader_page) {
        gtk_stack_set_visible_child(GTK_STACK(app->stack), app->reader_page);
    }

    gtk_widget_grab_focus(app->canvas);
    if (!render_current(app)) {
        gtk_label_set_text(GTK_LABEL(app->status), "Could not render the page.");
    } else {
        gtk_label_set_text(GTK_LABEL(app->status), "");
    }
}

static void show_library(App *app) {
    if (!app || !app->stack || !app->library_page) return;

    /* Stop any page-turn animation and discard its temporary destination page. */
    app->turn_direction = TURN_NONE;
    if (app->animation_source) {
        g_source_remove(app->animation_source);
        app->animation_source = 0;
    }
    if (app->pdf.next_surface) {
        cairo_surface_destroy(app->pdf.next_surface);
        app->pdf.next_surface = NULL;
    }
    app->pdf.next_w = app->pdf.next_h = 0;

    /* Persist the current book before changing views. */
    library_save(&app->library);
    rebuild_library_list(app);

    /* Home is an immediate view change. Temporarily disable the stack
       transition so a click can never get lost behind an in-flight fade. */
    GtkWidget *library_page = gtk_stack_get_child_by_name(GTK_STACK(app->stack), "library");
    if (!library_page) {
        g_warning("Leafturn: library stack page is missing");
        return;
    }

    gtk_widget_show(library_page);
    gtk_stack_set_transition_type(GTK_STACK(app->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_visible_child(GTK_STACK(app->stack), library_page);
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "library");
    gtk_widget_queue_draw(app->stack);
    gtk_stack_set_transition_type(GTK_STACK(app->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(app->header), APP_NAME);
    gtk_widget_grab_focus(app->library_list);
}

static void back_to_library(App *app) {
    show_library(app);
}

static bool reader_is_visible(App *app) {
    return app && app->stack && app->reader_page &&
           gtk_stack_get_visible_child(GTK_STACK(app->stack)) == app->reader_page;
}

static void zoom_in(App *app) {
    if (!app || !app->pdf.doc || !reader_is_visible(app) || app->turn_direction != TURN_NONE) return;
    app->pdf.zoom = MIN(app->pdf.zoom * 1.15f, 3.0f);
    render_current(app);
}

static void zoom_out(App *app) {
    if (!app || !app->pdf.doc || !reader_is_visible(app) || app->turn_direction != TURN_NONE) return;
    app->pdf.zoom = MAX(app->pdf.zoom / 1.15f, 0.65f);
    render_current(app);
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    App *app = user_data;
    if (event->type != GDK_KEY_PRESS) return FALSE;
    if (!app->pdf.doc || !reader_is_visible(app)) return FALSE;

    /* Home/End and numeric zoom reset remain window-level fallbacks. The
       directional controls are installed as GTK accelerators below so they
       work regardless of which reader control currently has focus. */
    switch (event->keyval) {
        case GDK_KEY_Home:
            if (app->turn_direction == TURN_NONE) {
                app->pdf.page = 0;
                render_current(app);
                library_save(&app->library);
            }
            return TRUE;
        case GDK_KEY_End:
            if (app->turn_direction == TURN_NONE) {
                app->pdf.page = app->pdf.page_count - 1;
                render_current(app);
                library_save(&app->library);
            }
            return TRUE;
        case GDK_KEY_0:
            if (app->turn_direction == TURN_NONE) {
                app->pdf.zoom = 1.0f;
                render_current(app);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)widget;
    App *app = user_data;
    if (event->type != GDK_BUTTON_PRESS || !app->pdf.doc || app->turn_direction != TURN_NONE) return FALSE;
    if (event->button == 1) {
        GtkAllocation a;
        gtk_widget_get_allocation(app->canvas, &a);
        if (event->x > (double)a.width * 0.55) begin_turn(app, TURN_FORWARD);
        else if (event->x < (double)a.width * 0.45) begin_turn(app, TURN_BACKWARD);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data) {
    (void)widget;
    App *app = user_data;
    if (!app->pdf.doc || app->turn_direction != TURN_NONE) return FALSE;
    if (event->direction == GDK_SCROLL_DOWN) {
        begin_turn(app, TURN_FORWARD);
        return TRUE;
    }
    if (event->direction == GDK_SCROLL_UP) {
        begin_turn(app, TURN_BACKWARD);
        return TRUE;
    }
    return FALSE;
}

static void add_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    choose_pdf(user_data);
}

static void back_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    back_to_library(user_data);
}

static void next_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    App *app = user_data;
    if (reader_is_visible(app)) begin_turn(app, TURN_FORWARD);
}

static void prev_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    App *app = user_data;
    if (reader_is_visible(app)) begin_turn(app, TURN_BACKWARD);
}

static void zoom_in_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    zoom_in(user_data);
}

static void zoom_out_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    zoom_out(user_data);
}

static void install_css(void) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background: #171512; }"
        ".library-heading { font-size: 28px; font-weight: 700; }"
        ".book-title { font-size: 17px; font-weight: 600; }"
        ".dim-label { opacity: 0.70; }"
        ".page-count { font-weight: 600; }"
        ".library-list { background: transparent; }"
        "list row { border-radius: 12px; margin: 4px 18px; }"
        "list row:hover { background: rgba(255,255,255,0.06); }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static GtkWidget *make_library_page(App *app) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *heading = gtk_label_new("Your Library");
    GtkWidget *sub = gtk_label_new("Local PDFs. No server, no database, no tracking.");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "library-heading");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "dim-label");
    gtk_widget_set_margin_start(intro, 24);
    gtk_widget_set_margin_top(intro, 24);
    gtk_widget_set_margin_end(intro, 24);
    gtk_widget_set_margin_bottom(intro, 18);
    gtk_box_pack_start(GTK_BOX(intro), heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(intro), sub, FALSE, FALSE, 0);

    app->library_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->library_list), GTK_SELECTION_SINGLE);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->library_list), "library-list");
    gtk_container_add(GTK_CONTAINER(scroll), app->library_list);
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    g_signal_connect(app->library_list, "row-activated", G_CALLBACK(row_activate), app);
    return page;
}

static GtkWidget *make_reader_page(App *app) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *prev = gtk_button_new_with_label("‹");
    GtkWidget *next = gtk_button_new_with_label("›");
    GtkWidget *minus = gtk_button_new_with_label("−");
    GtkWidget *plus = gtk_button_new_with_label("+");
    GtkWidget *spacer = gtk_label_new("");
    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    app->canvas = gtk_drawing_area_new();
    gtk_widget_set_vexpand(app->canvas, TRUE);
    gtk_widget_set_hexpand(app->canvas, TRUE);
    gtk_widget_set_size_request(app->canvas, 800, 600);
    gtk_widget_set_can_focus(app->canvas, TRUE);
    gtk_widget_add_events(app->canvas, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK);
    g_signal_connect(app->canvas, "draw", G_CALLBACK(on_canvas_draw), app);
    g_signal_connect(app->canvas, "button-press-event", G_CALLBACK(on_button_press), app);
    g_signal_connect(app->canvas, "scroll-event", G_CALLBACK(on_scroll), app);

    app->back_button = gtk_button_new_with_label("⌂ Home");
    gtk_widget_set_tooltip_text(app->back_button, "Return to your PDF library (Esc)");
    app->prev_button = prev;
    app->next_button = next;
    app->zoom_out_button = minus;
    app->zoom_in_button = plus;
    app->page_label = gtk_label_new("Page 1 of 1 · 0%");
    app->progress = gtk_progress_bar_new();
    app->zoom_label = gtk_label_new("100%");

    gtk_style_context_add_class(gtk_widget_get_style_context(app->page_label), "page-count");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->zoom_label), "dim-label");
    gtk_widget_set_size_request(app->progress, 220, -1);
    gtk_widget_set_valign(app->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(spacer, TRUE);

    gtk_box_pack_start(GTK_BOX(controls), app->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), spacer, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls), center, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), minus, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), app->zoom_label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(controls), plus, FALSE, FALSE, 0);
    gtk_box_set_homogeneous(GTK_BOX(center), FALSE);
    gtk_box_pack_start(GTK_BOX(center), app->page_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center), app->progress, FALSE, FALSE, 0);

    gtk_widget_set_margin_start(controls, 12);
    gtk_widget_set_margin_end(controls, 12);
    gtk_widget_set_margin_top(controls, 8);
    gtk_widget_set_margin_bottom(controls, 8);

    gtk_box_pack_start(GTK_BOX(page), controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), app->canvas, TRUE, TRUE, 0);

    g_signal_connect(app->back_button, "clicked", G_CALLBACK(back_clicked), app);
    g_signal_connect(prev, "clicked", G_CALLBACK(prev_clicked), app);
    g_signal_connect(next, "clicked", G_CALLBACK(next_clicked), app);
    g_signal_connect(minus, "clicked", G_CALLBACK(zoom_out_clicked), app);
    g_signal_connect(plus, "clicked", G_CALLBACK(zoom_in_clicked), app);
    return page;
}

static void install_keybindings(App *app) {
    app->accels = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app->window), app->accels);

    /* Page turns: Left/Right are the primary bindings requested for the
       reader. Keep the existing Space/PageUp/PageDown/Backspace shortcuts. */
    gtk_widget_add_accelerator(app->prev_button, "clicked", app->accels,
                               GDK_KEY_Left, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->prev_button, "clicked", app->accels,
                               GDK_KEY_KP_Left, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->prev_button, "clicked", app->accels,
                               GDK_KEY_Page_Up, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->prev_button, "clicked", app->accels,
                               GDK_KEY_BackSpace, 0, GTK_ACCEL_VISIBLE);

    gtk_widget_add_accelerator(app->next_button, "clicked", app->accels,
                               GDK_KEY_Right, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->next_button, "clicked", app->accels,
                               GDK_KEY_KP_Right, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->next_button, "clicked", app->accels,
                               GDK_KEY_Page_Down, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->next_button, "clicked", app->accels,
                               GDK_KEY_space, 0, GTK_ACCEL_VISIBLE);

    /* Up/Down zoom the document in/out without requiring canvas focus. */
    gtk_widget_add_accelerator(app->zoom_in_button, "clicked", app->accels,
                               GDK_KEY_Up, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->zoom_in_button, "clicked", app->accels,
                               GDK_KEY_KP_Up, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->zoom_out_button, "clicked", app->accels,
                               GDK_KEY_Down, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(app->zoom_out_button, "clicked", app->accels,
                               GDK_KEY_KP_Down, 0, GTK_ACCEL_VISIBLE);

    /* Escape is also a real Home accelerator, so it works even when a
       control has keyboard focus. */
    gtk_widget_add_accelerator(app->back_button, "clicked", app->accels,
                               GDK_KEY_Escape, 0, GTK_ACCEL_VISIBLE);
}

static void on_app_open(GApplication *gapp, GFile **files, gint n_files,
                        const gchar *hint, gpointer user_data) {
    (void)hint;
    App *app = user_data;
    on_app_activate(GTK_APPLICATION(gapp), app);
    if (n_files > 0) {
        char *path = g_file_get_path(files[0]);
        if (path) {
            import_path(app, path);
            g_free(path);
        }
    }
}

static void on_app_activate(GtkApplication *gtk_app, gpointer user_data) {
    App *app = user_data;
    if (app->window) {
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }

    app->app = gtk_app;
    install_css();
    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_default_size(GTK_WINDOW(app->window), WINDOW_W, WINDOW_H);
    gtk_window_set_title(GTK_WINDOW(app->window), APP_NAME);
    gtk_widget_add_events(app->window, GDK_KEY_PRESS_MASK);
    g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key), app);

    app->header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(app->header), APP_NAME);
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(app->header), TRUE);
    GtkWidget *add = gtk_button_new_with_label("+ Open PDF");
    gtk_style_context_add_class(gtk_widget_get_style_context(add), "suggested-action");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(app->header), add);
    gtk_window_set_titlebar(GTK_WINDOW(app->window), app->header);
    g_signal_connect(add, "clicked", G_CALLBACK(add_clicked), app);

    app->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_container_add(GTK_CONTAINER(app->window), app->stack);

    app->library_page = make_library_page(app);
    app->reader_page = make_reader_page(app);
    gtk_stack_add_named(GTK_STACK(app->stack), app->library_page, "library");
    gtk_stack_add_named(GTK_STACK(app->stack), app->reader_page, "reader");
    install_keybindings(app);

    app->status = gtk_label_new("");
    gtk_widget_set_no_show_all(app->status, TRUE);
    rebuild_library_list(app);
    gtk_stack_set_visible_child(GTK_STACK(app->stack), app->library_page);
    gtk_widget_show_all(app->window);
}

static void app_shutdown(App *app) {
    if (app->animation_source) {
        g_source_remove(app->animation_source);
        app->animation_source = 0;
    }
    library_save(&app->library);
    pdf_drop_context(&app->pdf);
    library_free(&app->library);
}

int main(int argc, char **argv) {
    App app;
    memset(&app, 0, sizeof(app));
    library_load(&app.library);

    if (!pdf_init(&app.pdf)) {
        g_printerr("Leafturn: could not initialize MuPDF.\n");
        library_free(&app.library);
        return EXIT_FAILURE;
    }

    GtkApplication *gtk_app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_app_activate), &app);
    g_signal_connect(gtk_app, "open", G_CALLBACK(on_app_open), &app);
    const int exit_code = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    app_shutdown(&app);
    g_object_unref(gtk_app);
    return exit_code;
}
