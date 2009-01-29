#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "vterm.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#ifdef DEBUG
# define DEBUG_PRINT_INPUT
#endif

int master;
vterm_t *vt;

int cell_width_pango;
int cell_width;
int cell_height;

GdkRectangle invalid_area;
int cursor_visible;
GdkRectangle cursor_area;

guint cursor_timer_id;

GtkWidget *termwin;

// Actual stores of Pixmaps
GdkPixmap *termbuffer_main;
GdkPixmap *termbuffer_alternate;

// This always points at one of the above
GdkPixmap *termbuffer;
GdkGC *termbuffer_gc;

GdkGC *cursor_gc;

typedef struct {
  GdkColor fg_col;
  GdkColor bg_col;
} term_cell;

term_cell **cells;

static char *default_fg = "gray90";
static char *default_bg = "black";

static char *cursor_col = "white";
static gint cursor_blink_interval = 500;

static char *default_font = "DejaVu Sans Mono";
static int default_size = 9;

static GOptionEntry option_entries[] = {
  /* long_name, short_name, flags, arg, arg_data, description, arg_description */
  { "foreground", 0,   0, G_OPTION_ARG_STRING, &default_fg, "Default foreground colour", "COL" },
  { "background", 0,   0, G_OPTION_ARG_STRING, &default_bg, "Default background colour", "COL" },
  { "cursor",     0,   0, G_OPTION_ARG_STRING, &cursor_col, "Cursor colour", "COL" },

  { "font",       0,   0, G_OPTION_ARG_STRING, &default_font, "Font name", "FONT" },
  { "size",       's', 0, G_OPTION_ARG_INT,    &default_size, "Font size", "INT" },

  { NULL },
};

PangoFontDescription *fontdesc;

GtkIMContext *im_context;

vterm_mousefunc mousefunc;
void *mousedata;

const char *col_spec[] = {
  "black",
  "red",
  "green",
  "yellow",
  "blue",
  "magenta",
  "cyan",
  "white"
};

typedef struct {
  GdkColor fg_col;
  GdkColor bg_col;
  gboolean reverse;
  PangoAttrList *attrs;
  PangoLayout *layout;
} term_pen;

GString *glyphs = NULL;
GArray *glyph_widths = NULL;
GdkRectangle glyph_area;
term_pen *glyph_pen;

vterm_key convert_keyval(guint gdk_keyval)
{
  switch(gdk_keyval) {
  case GDK_BackSpace:
    return VTERM_KEY_BACKSPACE;
  case GDK_Tab:
    return VTERM_KEY_TAB;
  case GDK_Return:
    return VTERM_KEY_ENTER;
  case GDK_Escape:
    return VTERM_KEY_ESCAPE;

  case GDK_Up:
    return VTERM_KEY_UP;
  case GDK_Down:
    return VTERM_KEY_DOWN;
  case GDK_Left:
    return VTERM_KEY_LEFT;
  case GDK_Right:
    return VTERM_KEY_RIGHT;

  case GDK_Insert:
    return VTERM_KEY_INS;
  case GDK_Delete:
    return VTERM_KEY_DEL;
  case GDK_Home:
    return VTERM_KEY_HOME;
  case GDK_End:
    return VTERM_KEY_END;
  case GDK_Page_Up:
    return VTERM_KEY_PAGEUP;
  case GDK_Page_Down:
    return VTERM_KEY_PAGEDOWN;

  default:
    return VTERM_KEY_NONE;
  }
}

static void update_termbuffer(void)
{
  if(termbuffer_gc) {
    g_object_unref(termbuffer_gc);
    termbuffer_gc = NULL;
  }

  if(termbuffer)
    termbuffer_gc = gdk_gc_new(termbuffer);
}

static void add_glyph(const uint32_t chars[], int width)
{
  char *chars_str = g_ucs4_to_utf8(chars, -1, NULL, NULL, NULL);

  g_array_set_size(glyph_widths, glyphs->len + 1);
  g_array_index(glyph_widths, int, glyphs->len) = width;

  g_string_append(glyphs, chars_str);

  g_free(chars_str);

  return;
}

static void flush_glyphs(void)
{
  if(!glyphs->len) {
    glyph_area.width = 0;
    glyph_area.height = 0;
    return;
  }

  gdk_gc_set_clip_rectangle(termbuffer_gc, &glyph_area);

  PangoLayout *layout = glyph_pen->layout;

  pango_layout_set_text(layout, glyphs->str, glyphs->len);

  if(glyph_pen->attrs)
    pango_layout_set_attributes(layout, glyph_pen->attrs);

  // Now adjust all the widths
  PangoLayoutIter *iter = pango_layout_get_iter(layout);
  do {
    PangoLayoutRun *run = pango_layout_iter_get_run(iter);
    if(!run)
      continue;

    PangoGlyphString *glyph_str = run->glyphs;
    int i;
    for(i = 0; i < glyph_str->num_glyphs; i++) {
      PangoGlyphInfo *glyph = &glyph_str->glyphs[i];
      int str_index = run->item->offset + glyph_str->log_clusters[i];
      int char_width = g_array_index(glyph_widths, int, str_index);
      if(glyph->geometry.width && glyph->geometry.width != char_width * cell_width_pango) {
        /* Adjust its x_offset to match the width change, to ensure it still
         * remains centered in the cell */
        glyph->geometry.x_offset -= (glyph->geometry.width - char_width * cell_width_pango) / 2;
        glyph->geometry.width = char_width * cell_width_pango;
      }
    }
  } while(pango_layout_iter_next_run(iter));

  pango_layout_iter_free(iter);

  gdk_draw_layout_with_colors(termbuffer,
      termbuffer_gc,
      glyph_area.x,
      glyph_area.y,
      layout,
      glyph_pen->reverse ? &glyph_pen->bg_col : &glyph_pen->fg_col,
      NULL);

  gdk_rectangle_union(&glyph_area, &invalid_area, &invalid_area);

  glyph_area.width = 0;
  glyph_area.height = 0;

  g_string_truncate(glyphs, 0);
}

void repaint_area(GdkRectangle *area)
{
  GdkWindow *win = termwin->window;

  gdk_gc_set_clip_rectangle(termbuffer_gc, area);

  gdk_draw_drawable(win,
      termbuffer_gc,
      termbuffer,
      0, 0, 0, 0, -1, -1);
}

gboolean term_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  repaint_area(&event->area);

  if(cursor_visible && gdk_rectangle_intersect(&cursor_area, &event->area, NULL))
    gdk_draw_rectangle(termwin->window,
        cursor_gc,
        FALSE,
        cursor_area.x,
        cursor_area.y,
        cursor_area.width - 1,
        cursor_area.height - 1);

  return TRUE;
}

gboolean term_keypress(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  gboolean ret = gtk_im_context_filter_keypress(im_context, event);

  if(ret)
    return TRUE;

  // We don't need to track the state of modifier bits
  if(event->is_modifier)
    return FALSE;

  vterm_mod state = VTERM_MOD_NONE;
  if(event->state & GDK_SHIFT_MASK)
    state |= VTERM_MOD_SHIFT;
  if(event->state & GDK_CONTROL_MASK)
    state |= VTERM_MOD_CTRL;
  if(event->state & GDK_MOD1_MASK)
    state |= VTERM_MOD_ALT;

  vterm_key keyval = convert_keyval(event->keyval);

  if(keyval)
    vterm_input_push_key(vt, state, keyval);
  else {
    size_t len = strlen(event->string);
    if(len)
      vterm_input_push_str(vt, state, event->string, len);
    else
      printf("Unsure how to handle key %d with no string\n", event->keyval);
  }

  size_t bufflen = vterm_output_bufferlen(vt);
  if(bufflen) {
    char buffer[bufflen];
    bufflen = vterm_output_bufferread(vt, buffer, bufflen);
    write(master, buffer, bufflen);
  }

  return FALSE;
}

gboolean term_mousepress(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
  (*mousefunc)(event->x / cell_width, event->y / cell_height, event->button, event->type == GDK_BUTTON_PRESS, mousedata);

  size_t bufflen = vterm_output_bufferlen(vt);
  if(bufflen) {
    char buffer[bufflen];
    bufflen = vterm_output_bufferread(vt, buffer, bufflen);
    write(master, buffer, bufflen);
  }

  return FALSE;
}

gboolean im_commit(GtkIMContext *context, gchar *str, gpointer user_data)
{
  vterm_input_push_str(vt, 0, str, strlen(str));

  size_t bufflen = vterm_output_bufferlen(vt);
  if(bufflen) {
    char buffer[bufflen];
    bufflen = vterm_output_bufferread(vt, buffer, bufflen);
    write(master, buffer, bufflen);
  }

  return FALSE;
}

int term_putglyph(vterm_t *vt, const uint32_t chars[], int width, vterm_position_t pos, void *pen_p)
{
  term_pen *pen = pen_p;

  cells[pos.row][pos.col].fg_col = pen->reverse ? pen->bg_col : pen->fg_col;
  GdkColor bg = cells[pos.row][pos.col].bg_col = pen->reverse ? pen->fg_col : pen->bg_col;

  gdk_gc_set_rgb_fg_color(termbuffer_gc, &bg);

  GdkRectangle destarea = {
    .x      = pos.col * cell_width,
    .y      = pos.row * cell_height,
    .width  = cell_width * width,
    .height = cell_height
  };

  if(destarea.y != glyph_area.y || destarea.x != glyph_area.x + glyph_area.width)
    flush_glyphs();

  gdk_gc_set_clip_rectangle(termbuffer_gc, &destarea);

  gdk_draw_rectangle(termbuffer,
      termbuffer_gc,
      TRUE,
      destarea.x,
      destarea.y,
      destarea.width,
      destarea.height);

  add_glyph(chars, width);
  glyph_pen = pen;

  if(glyph_area.width && glyph_area.height)
    gdk_rectangle_union(&destarea, &glyph_area, &glyph_area);
  else
    glyph_area = destarea;

  return 1;
}

int term_movecursor(vterm_t *vt, vterm_position_t pos, vterm_position_t oldpos, int visible)
{
  GdkRectangle destarea = {
    .x      = oldpos.col * cell_width,
    .y      = oldpos.row * cell_height,
    .width  = cell_width,
    .height = cell_height
  };

  gdk_rectangle_union(&destarea, &invalid_area, &invalid_area);

  cursor_area.x      = pos.col * cell_width;
  cursor_area.y      = pos.row * cell_height;

  cursor_visible = visible;

  return 1;
}

gboolean cursor_blink(gpointer data)
{
  invalid_area.x = 0;
  invalid_area.y = 0;
  invalid_area.width = 0;
  invalid_area.height = 0;

  cursor_visible = !cursor_visible;
  gdk_rectangle_union(&cursor_area, &invalid_area, &invalid_area);

  if(invalid_area.width && invalid_area.height)
    repaint_area(&invalid_area);

  if(cursor_visible)
    gdk_draw_rectangle(termwin->window,
        cursor_gc,
        FALSE,
        cursor_area.x,
        cursor_area.y,
        cursor_area.width - 1,
        cursor_area.height - 1);

  return TRUE;
}

int term_scroll(vterm_t *vt, vterm_rectangle_t rect, int downward, int rightward)
{
  flush_glyphs();

  int rows = rect.end_row - rect.start_row - downward;
  int cols = rect.end_col - rect.start_col - rightward;

  GdkRectangle destarea = {
    .x      = rect.start_col * cell_width,
    .y      = rect.start_row * cell_height,
    .width  = cols * cell_width,
    .height = rows * cell_height
  };

  gdk_gc_set_clip_rectangle(termbuffer_gc, &destarea);

  gdk_draw_drawable(termbuffer,
      termbuffer_gc,
      termbuffer,
      (rect.start_col + rightward) * cell_width,
      (rect.start_row + downward ) * cell_height,
      destarea.x,
      destarea.y,
      destarea.width,
      destarea.height);

  gdk_rectangle_union(&destarea, &invalid_area, &invalid_area);

  return 0; // Because we still need to get copycell to move the metadata
}

int term_copycell(vterm_t *vt, vterm_position_t destpos, vterm_position_t srcpos)
{
  cells[destpos.row][destpos.col].fg_col = cells[srcpos.row][srcpos.col].fg_col;
  cells[destpos.row][destpos.col].bg_col = cells[srcpos.row][srcpos.col].bg_col;

  return 1;
}

int term_erase(vterm_t *vt, vterm_rectangle_t rect, void *pen_p)
{
  flush_glyphs();

  term_pen *pen = pen_p;

  GdkColor bg = pen->reverse ? pen->fg_col : pen->bg_col;
  gdk_gc_set_rgb_fg_color(termbuffer_gc, &bg);

  int row, col;
  for(row = rect.start_row; row < rect.end_row; row++)
    for(col = rect.start_col; col < rect.end_col; col++) {
      cells[row][col].bg_col = bg;
    }

  GdkRectangle destarea = {
    .x      = rect.start_col * cell_width,
    .y      = rect.start_row * cell_height,
    .width  = (rect.end_col - rect.start_col) * cell_width,
    .height = (rect.end_row - rect.start_row) * cell_height,
  };

  gdk_gc_set_clip_rectangle(termbuffer_gc, &destarea);

  gdk_draw_rectangle(termbuffer,
      termbuffer_gc,
      TRUE,
      destarea.x,
      destarea.y,
      destarea.width,
      destarea.height);

  gdk_rectangle_union(&destarea, &invalid_area, &invalid_area);

  return 1;
}

int term_setpen(vterm_t *vt, int sgrcmd, void **penstore)
{
  term_pen *pen = *penstore;

  if(!*penstore) {
    pen = g_new0(term_pen, 1);
    *penstore = pen;
    pen->attrs = pango_attr_list_new();
    pen->layout = pango_layout_new(gtk_widget_get_pango_context(termwin));
    pango_layout_set_font_description(pen->layout, fontdesc);
    gdk_color_parse(default_fg, &pen->fg_col);
    gdk_color_parse(default_bg, &pen->bg_col);
    return 1;
  }

  return 0;
}

static void lookup_colour(int palette, int index, const char *def, GdkColor *col)
{
  switch(palette) {
  case 0:
    if(index == -1)
      gdk_color_parse(def,col);
    else if(index >= 0 && index < 8)
      gdk_color_parse(col_spec[index], col);
    break;

  case 5: // XTerm 256-colour mode
    if(index >= 0 && index < 16)
      // Normal 16 colours
      // TODO: support low/high intensities
      gdk_color_parse(col_spec[index % 8], col);
    else if(index >= 16 && index < 232) {
      // 216-colour cube
      index -= 16;

      col->blue  = (index     % 6) * (0xffff/6);
      col->green = (index/6   % 6) * (0xffff/6);
      col->red   = (index/6/6 % 6) * (0xffff/6);
    }
    else if(index >= 232 && index < 256) {
      // 24 greyscales
      index -= 232;

      col->blue  = index * 0xffff / 24;
      col->green = index * 0xffff / 24;
      col->red   = index * 0xffff / 24;
    }
    break;

  default:
    printf("Unrecognised colour palette %d\n", palette);
  }
}

int term_setpenattr(vterm_t *vt, vterm_attr attr, vterm_attrvalue *val, void **penstore)
{
  flush_glyphs();

#define ADDATTR(a) \
  do { \
    PangoAttribute *newattr = (a); \
    newattr->start_index = 0; \
    newattr->end_index = -1; \
    pango_attr_list_change(pen->attrs, newattr); \
  } while(0)

  term_pen *pen = *penstore;

  switch(attr) {
  case VTERM_ATTR_BOLD:
    ADDATTR(pango_attr_weight_new(val->boolean ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL));
    break;

  case VTERM_ATTR_UNDERLINE:
    ADDATTR(pango_attr_underline_new(val->value == 1 ? PANGO_UNDERLINE_SINGLE :
                                     val->value == 2 ? PANGO_UNDERLINE_DOUBLE :
                                                      PANGO_UNDERLINE_NONE));
    break;

  case VTERM_ATTR_ITALIC:
    ADDATTR(pango_attr_style_new(val->boolean ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL));
    break;

  case VTERM_ATTR_REVERSE:
    pen->reverse = val->boolean;
    break;

  case VTERM_ATTR_FOREGROUND:
    lookup_colour(val->color.palette, val->color.index, default_fg, &pen->fg_col);
    break;

  case VTERM_ATTR_BACKGROUND:
    lookup_colour(val->color.palette, val->color.index, default_bg, &pen->bg_col);
    break;

  default:
    return 0;
  }

  return 1;
}

int term_setmode(vterm_t *vt, vterm_mode mode, int val)
{
  switch(mode) {
  case VTERM_MODE_DEC_CURSORVISIBLE:
    cursor_visible = val;
    gdk_rectangle_union(&cursor_area, &invalid_area, &invalid_area);
    break;

  case VTERM_MODE_DEC_CURSORBLINK:
    if(val) {
      cursor_timer_id = g_timeout_add(cursor_blink_interval, cursor_blink, NULL);
    }
    else {
      g_source_remove(cursor_timer_id);
    }
    break;

  case VTERM_MODE_DEC_ALTSCREEN:
    {
      int rows, cols;
      vterm_get_size(vt, &rows, &cols);

      GdkRectangle rect = {
        .x = 0,
        .y = 0,
        .width  = cols * cell_width,
        .height = rows * cell_height,
      };

      termbuffer = val ? termbuffer_alternate : termbuffer_main;
      update_termbuffer();

      gdk_rectangle_union(&rect, &invalid_area, &invalid_area);
    }
    break;

  default:
    return 0;
  }

  return 1;
}

int term_setmousefunc(vterm_t *vt, vterm_mousefunc func, void *data)
{
  mousefunc = func;
  mousedata = data;

  GdkEventMask mask = gdk_window_get_events(termwin->window);

  if(func)
    mask |= GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK;
  else
    mask &= ~(GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

  gdk_window_set_events(termwin->window, mask);

  return 1;
}

int term_bell(vterm_t *vt)
{
  gtk_widget_error_bell(GTK_WIDGET(termwin));
  return 1;
}

static vterm_state_callbacks_t cb = {
  .putglyph     = term_putglyph,
  .movecursor   = term_movecursor,
  .scroll       = term_scroll,
  .copycell     = term_copycell,
  .erase        = term_erase,
  .setpen       = term_setpen,
  .setpenattr   = term_setpenattr,
  .setmode      = term_setmode,
  .setmousefunc = term_setmousefunc,
  .bell         = term_bell,
};

int term_osc(vterm_t *vt, const char *command, size_t cmdlen)
{
  if(cmdlen < 2)
    return 0;

  if(strncmp(command, "0;", 2) == 0) {
    gchar *title = g_strndup(command + 2, cmdlen - 2);
    gtk_window_set_title(GTK_WINDOW(termwin), title);
    gdk_window_set_icon_name(GDK_WINDOW(termwin->window), title);
    g_free(title);
    return 1;
  }
  else if(strncmp(command, "1;", 2) == 0) {
    gchar *title = g_strndup(command + 2, cmdlen - 2);
    gdk_window_set_icon_name(GDK_WINDOW(termwin->window), title);
    g_free(title);
    return 1;
  }
  else if(strncmp(command, "2;", 2) == 0) {
    gchar *title = g_strndup(command + 2, cmdlen - 2);
    gtk_window_set_title(GTK_WINDOW(termwin), title);
    g_free(title);
    return 1;
  }

  return 0;
}

static vterm_parser_callbacks_t parser_cb = {
  .osc = term_osc,
};

gboolean master_readable(GIOChannel *source, GIOCondition cond, gpointer data)
{
  char buffer[8192];

  ssize_t bytes = read(master, buffer, sizeof buffer);

  if(bytes == 0) {
    fprintf(stderr, "master closed\n");
    exit(0);
  }
  if(bytes < 0) {
    fprintf(stderr, "read(master) failed - %s\n", strerror(errno));
    exit(1);
  }

#ifdef DEBUG_PRINT_INPUT
  printf("Read %d bytes from master:\n", bytes);
  int i;
  for(i = 0; i < bytes; i++) {
    printf(i % 16 == 0 ? " |  %02x" : " %02x", buffer[i]);
    if(i % 16 == 15)
      printf("\n");
  }
  if(i % 16)
    printf("\n");
#endif

  invalid_area.x = 0;
  invalid_area.y = 0;
  invalid_area.width = 0;
  invalid_area.height = 0;

  vterm_push_bytes(vt, buffer, bytes);

  flush_glyphs();

  if(invalid_area.width && invalid_area.height)
    repaint_area(&invalid_area);

  if(cursor_visible)
    gdk_draw_rectangle(termwin->window,
        cursor_gc,
        FALSE,
        cursor_area.x,
        cursor_area.y,
        cursor_area.width - 1,
        cursor_area.height - 1);

  return TRUE;
}

int main(int argc, char *argv[])
{
  GError *args_error = NULL;
  GOptionContext *args_context;

  args_context = g_option_context_new("commandline...");
  g_option_context_add_main_entries(args_context, option_entries, NULL);
  g_option_context_add_group(args_context, gtk_get_option_group(TRUE));
  if(!g_option_context_parse(args_context, &argc, &argv, &args_error)) {
    fprintf(stderr, "Option parsing failed: %s\n", args_error->message);
    exit (1);
  }

  gtk_init(&argc, &argv);

  struct winsize size = { 25, 80, 0, 0 };

  vt = vterm_new(size.ws_row, size.ws_col);
  vterm_parser_set_utf8(vt, 1);

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  termwin = window;

  glyphs = g_string_sized_new(128);
  glyph_widths = g_array_new(FALSE, FALSE, sizeof(int));

  gtk_widget_realize(window);

  vterm_set_parser_callbacks(vt, &parser_cb);
  vterm_set_state_callbacks(vt, &cb);

  g_signal_connect(G_OBJECT(window), "expose-event", GTK_SIGNAL_FUNC(term_expose), NULL);
  g_signal_connect(G_OBJECT(window), "key-press-event", GTK_SIGNAL_FUNC(term_keypress), NULL);

  g_signal_connect(G_OBJECT(termwin), "button-press-event",   GTK_SIGNAL_FUNC(term_mousepress), NULL);
  g_signal_connect(G_OBJECT(termwin), "button-release-event", GTK_SIGNAL_FUNC(term_mousepress), NULL);

  im_context = gtk_im_context_simple_new();

  g_signal_connect(G_OBJECT(im_context), "commit", GTK_SIGNAL_FUNC(im_commit), NULL);

  PangoContext *pctx = gtk_widget_get_pango_context(window);

  fontdesc = pango_font_description_new();
  pango_font_description_set_family(fontdesc, default_font);
  pango_font_description_set_size(fontdesc, default_size * PANGO_SCALE);

  pango_context_set_font_description(pctx, fontdesc);

  cells = g_new0(term_cell*, size.ws_row);

  int row;
  for(row = 0; row < size.ws_row; row++) {
    cells[row] = g_new0(term_cell, size.ws_col);
  }

  PangoFontMetrics *metrics = pango_context_get_metrics(pctx,
      pango_context_get_font_description(pctx), pango_context_get_language(pctx));

  int width = (pango_font_metrics_get_approximate_char_width(metrics) + 
               pango_font_metrics_get_approximate_digit_width(metrics)) / 2;

  int height = pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent(metrics);

  cell_width_pango = width;
  cell_width  = PANGO_PIXELS_CEIL(width);
  cell_height = PANGO_PIXELS_CEIL(height);

  cursor_area.width  = cell_width;
  cursor_area.height = cell_height;

  termbuffer_main = gdk_pixmap_new(window->window,
      size.ws_col * cell_width, size.ws_row * cell_height, -1);
  termbuffer_alternate = gdk_pixmap_new(window->window,
      size.ws_col * cell_width, size.ws_row * cell_height, -1);

  termbuffer = termbuffer_main;
  update_termbuffer();

  gtk_window_resize(GTK_WINDOW(window), 
      size.ws_col * cell_width, size.ws_row * cell_height);

  //gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

  cursor_gc = gdk_gc_new(window->window);

  GdkColor col;
  gdk_color_parse(cursor_col, &col);
  gdk_gc_set_rgb_fg_color(cursor_gc, &col);

  vterm_state_initialise(vt);

  pid_t kid = forkpty(&master, NULL, NULL, &size);
  if(kid == 0) {
    if(argc > 1) {
      execvp(argv[1], argv + 1);
      fprintf(stderr, "Cannot exec(%s) - %s\n", argv[1], strerror(errno));
    }
    else {
      char *shell = getenv("SHELL");
      execvp(shell, NULL);
      fprintf(stderr, "Cannot exec(%s) - %s\n", shell, strerror(errno));
    }
    _exit(1);
  }

  GIOChannel *gio_master = g_io_channel_unix_new(master);
  g_io_add_watch(gio_master, G_IO_IN|G_IO_HUP, master_readable, NULL);

  gtk_widget_show_all(window);

  gtk_main();

  return 0;
}