#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <pty.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
  GtkWidget *window;
  GtkWidget *textview;
  GtkWidget *scroll;
  GtkTextBuffer *buffer;
  GIOChannel *io_channel;
  int pty_fd;
  pid_t child_pid;
  bool ctrl;
  bool alt;
  GString *line;
  int pos;
} AppState;

static bool write_all(int fd, const char *buf, size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = write(fd, buf + written, len - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    written += (size_t)n;
  }
  return true;
}

static void strip_ansi_sgr(char *input, gsize *len) {
  gsize read_idx = 0;
  gsize write_idx = 0;

  while (read_idx < *len) {
    if ((unsigned char)input[read_idx] == 0x1b && read_idx + 1 < *len &&
        input[read_idx + 1] == '[') {
      gsize i = read_idx + 2;
      while (i < *len &&
             (g_ascii_isdigit((guchar)input[i]) || input[i] == ';')) {
        i++;
      }
      if (i < *len && input[i] == 'm') {
        read_idx = i + 1;
        continue;
      }
    }

    input[write_idx++] = input[read_idx++];
  }

  *len = write_idx;
}

static void update_last_line(AppState *state, const char *text, gsize len) {
  GtkTextIter start;
  GtkTextIter end;
  GtkTextIter insert_at;
  int line_count = gtk_text_buffer_get_line_count(state->buffer);

  gtk_text_buffer_get_iter_at_line(state->buffer, &start, line_count - 1);
  gtk_text_buffer_get_end_iter(state->buffer, &end);
  gtk_text_buffer_delete(state->buffer, &start, &end);

  for (gsize i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    switch (c) {
    case '\a':
      break;
    case '\n':
      gtk_text_buffer_get_end_iter(state->buffer, &insert_at);
      gtk_text_buffer_insert(state->buffer, &insert_at, state->line->str, -1);
      gtk_text_buffer_insert(state->buffer, &insert_at, "\n", 1);
      g_string_truncate(state->line, 0);
      state->pos = 0;
      break;
    case '\b':
      state->pos--;
      if (state->pos < 0) {
        state->pos = 0;
      }
      break;
    case '\r':
      state->pos = 0;
      break;
    default:
      if (!isprint(c)) {
        break;
      }

      if (state->pos < (int)state->line->len) {
        state->line->str[state->pos] = (char)c;
      } else {
        while (state->pos > (int)state->line->len) {
          g_string_append_c(state->line, ' ');
        }
        g_string_append_c(state->line, (char)c);
      }
      state->pos++;
      break;
    }
  }

  gtk_text_buffer_get_end_iter(state->buffer, &insert_at);
  gtk_text_buffer_insert(state->buffer, &insert_at, state->line->str, -1);

  line_count = gtk_text_buffer_get_line_count(state->buffer);
  gtk_text_buffer_get_iter_at_line_offset(state->buffer, &insert_at, line_count - 1,
                                          state->pos);
  gtk_text_buffer_place_cursor(state->buffer, &insert_at);
}

static gboolean on_pty_hup(GIOChannel *channel, GIOCondition condition,
                           gpointer user_data) {
  (void)channel;
  (void)condition;
  (void)user_data;
  gtk_main_quit();
  return FALSE;
}

static gboolean on_pty_input(GIOChannel *channel, GIOCondition condition,
                             gpointer user_data) {
  (void)channel;
  AppState *state = (AppState *)user_data;
  char buffer[1024];

  if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
    gtk_main_quit();
    return FALSE;
  }

  ssize_t bytes_read = read(state->pty_fd, buffer, sizeof(buffer));
  if (bytes_read <= 0) {
    if (bytes_read < 0 && errno == EAGAIN) {
      return TRUE;
    }
    gtk_main_quit();
    return FALSE;
  }

  gsize len = (gsize)bytes_read;
  strip_ansi_sgr(buffer, &len);
  update_last_line(state, buffer, len);

  return TRUE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
                             gpointer user_data) {
  (void)widget;
  AppState *state = (AppState *)user_data;
  char send_buf[8];
  int send_len = 0;

  switch (event->keyval) {
  case GDK_KEY_Escape:
    gtk_main_quit();
    return TRUE;
  case GDK_KEY_Control_L:
  case GDK_KEY_Control_R:
    state->ctrl = true;
    return TRUE;
  case GDK_KEY_Alt_L:
  case GDK_KEY_Alt_R:
    state->alt = true;
    return TRUE;
  case GDK_KEY_BackSpace:
    send_buf[send_len++] = '\b';
    break;
  case GDK_KEY_Up:
    memcpy(send_buf, "\x1b[A", 3);
    send_len = 3;
    break;
  case GDK_KEY_Down:
    memcpy(send_buf, "\x1b[B", 3);
    send_len = 3;
    break;
  case GDK_KEY_Right:
    memcpy(send_buf, "\x1b[C", 3);
    send_len = 3;
    break;
  case GDK_KEY_Left:
    memcpy(send_buf, "\x1b[D", 3);
    send_len = 3;
    break;
  case GDK_KEY_Tab:
    send_buf[send_len++] = '\t';
    break;
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:
    gtk_widget_hide(state->window);
    (void)write_all(state->pty_fd, "\rexit\r", 6);
    return TRUE;
  default: {
    if (event->keyval < 128) {
      char c = (char)event->keyval;
      if (state->ctrl) {
        c = (char)(event->keyval - 96);
      }
      if (state->alt) {
        send_buf[send_len++] = '\x1b';
      }
      send_buf[send_len++] = c;
    }
    break;
  }
  }

  if (send_len > 0) {
    (void)write_all(state->pty_fd, send_buf, (size_t)send_len);
  }

  return TRUE;
}

static gboolean on_key_release(GtkWidget *widget, GdkEventKey *event,
                               gpointer user_data) {
  (void)widget;
  AppState *state = (AppState *)user_data;

  switch (event->keyval) {
  case GDK_KEY_Control_L:
  case GDK_KEY_Control_R:
    state->ctrl = false;
    break;
  case GDK_KEY_Alt_L:
  case GDK_KEY_Alt_R:
    state->alt = false;
    break;
  default:
    break;
  }

  return TRUE;
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event,
                                 gpointer user_data) {
  (void)widget;
  (void)event;
  (void)user_data;
  gtk_main_quit();
  return FALSE;
}

static void on_textview_size_allocate(GtkWidget *widget, GtkAllocation *allocation,
                                      gpointer user_data) {
  (void)widget;
  (void)allocation;
  GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW(user_data);
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(scroll);
  gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj) -
                                     gtk_adjustment_get_page_size(vadj));
}

static int spawn_shell_pty(pid_t *child_pid) {
  int master_fd = -1;
  pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    const char *home = getenv("HOME");
    if (home != NULL) {
      if (chdir(home) != 0) {
      }
    }
    (void)setenv("TERM", "dumb", 1);
    execlp("bash", "bash", "--login", (char *)NULL);
    _exit(127);
  }

  *child_pid = pid;
  return master_fd;
}

static void place_window_one_third(GtkWindow *window) {
  int win_w = 0;
  int win_h = 0;
  int screen_x = 0;
  int screen_y = 0;
  int screen_w = 0;
  int screen_h = 0;

  gtk_window_get_size(window, &win_w, &win_h);
  if (win_w <= 1 || win_h <= 1) {
    win_w = 800;
    win_h = 250;
  }

#if GTK_CHECK_VERSION(3, 22, 0)
  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
  GdkMonitor *monitor = NULL;
  if (display != NULL) {
    GdkWindow *gdk_window = gtk_widget_get_window(GTK_WIDGET(window));
    if (gdk_window != NULL) {
      monitor = gdk_display_get_monitor_at_window(display, gdk_window);
    }
    if (monitor == NULL) {
      monitor = gdk_display_get_primary_monitor(display);
    }
    if (monitor == NULL) {
      monitor = gdk_display_get_monitor(display, 0);
    }
  }
  if (monitor != NULL) {
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    screen_x = geometry.x;
    screen_y = geometry.y;
    screen_w = geometry.width;
    screen_h = geometry.height;
  }
#else
  GdkScreen *screen = gtk_window_get_screen(window);
  if (screen != NULL) {
    int monitor_num = gdk_screen_get_primary_monitor(screen);
    GdkRectangle geometry;
    if (monitor_num < 0) {
      monitor_num = 0;
    }
    gdk_screen_get_monitor_geometry(screen, monitor_num, &geometry);
    screen_x = geometry.x;
    screen_y = geometry.y;
    screen_w = geometry.width;
    screen_h = geometry.height;
  }
#endif

  if (screen_w <= 0 || screen_h <= 0) {
    return;
  }

  int x = screen_x + (screen_w / 3) - (win_w / 2);
  int y = screen_y + (screen_h / 3) - (win_h / 2);
  int max_x = screen_x + screen_w - win_w;
  int max_y = screen_y + screen_h - win_h;

  if (x < screen_x) {
    x = screen_x;
  }
  if (y < screen_y) {
    y = screen_y;
  }
  if (max_x < screen_x) {
    max_x = screen_x;
  }
  if (max_y < screen_y) {
    max_y = screen_y;
  }
  if (x > max_x) {
    x = max_x;
  }
  if (y > max_y) {
    y = max_y;
  }

  gtk_window_move(window, x, y);
}

int main(int argc, char **argv) {
  (void)argv;
  AppState state = {0};
  char *initial_text = g_strnfill(10, '\n');
  GtkCssProvider *css_provider = NULL;
  GtkStyleContext *style_context = NULL;

  gtk_init(&argc, NULL);

  state.pty_fd = spawn_shell_pty(&state.child_pid);
  if (state.pty_fd < 0) {
    g_printerr("failed to spawn bash PTY\n");
    g_free(initial_text);
    return 1;
  }

  state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(state.window), "gbashrun");
  gtk_window_set_keep_above(GTK_WINDOW(state.window), TRUE);
  gtk_window_set_modal(GTK_WINDOW(state.window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(state.window), 800, 250);
  gtk_widget_set_can_focus(state.window, TRUE);
  g_signal_connect(state.window, "delete-event", G_CALLBACK(on_window_delete),
                   &state);
  g_signal_connect(state.window, "key-press-event", G_CALLBACK(on_key_press),
                   &state);
  g_signal_connect(state.window, "key-release-event", G_CALLBACK(on_key_release),
                   &state);

  state.textview = gtk_text_view_new();
  style_context = gtk_widget_get_style_context(state.textview);
  gtk_style_context_add_class(style_context, "gbashrun-output");

  css_provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
      css_provider, ".gbashrun-output { font: 18pt monospace; }", -1, NULL);
  gtk_style_context_add_provider(
      style_context, GTK_STYLE_PROVIDER(css_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  state.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state.textview));
  gtk_text_buffer_set_text(state.buffer, initial_text, -1);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state.textview), TRUE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(state.textview), FALSE);

  state.scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(state.scroll), state.textview);
  gtk_container_add(GTK_CONTAINER(state.window), state.scroll);

  g_signal_connect(state.textview, "size-allocate",
                   G_CALLBACK(on_textview_size_allocate), state.scroll);

  state.line = g_string_new("");
  state.pos = 0;

  state.io_channel = g_io_channel_unix_new(state.pty_fd);
  g_io_channel_set_encoding(state.io_channel, NULL, NULL);
  g_io_channel_set_buffered(state.io_channel, FALSE);
  g_io_add_watch(state.io_channel, G_IO_IN, on_pty_input, &state);
  g_io_add_watch(state.io_channel, G_IO_HUP, on_pty_hup, &state);

  gtk_widget_show_all(state.window);
  place_window_one_third(GTK_WINDOW(state.window));
  gtk_window_present(GTK_WINDOW(state.window));
  gtk_widget_grab_focus(state.window);

  gtk_main();

  if (state.io_channel != NULL) {
    g_io_channel_unref(state.io_channel);
  }
  if (state.pty_fd >= 0) {
    close(state.pty_fd);
  }
  if (state.line != NULL) {
    g_string_free(state.line, TRUE);
  }
  if (css_provider != NULL) {
    g_object_unref(css_provider);
  }
  g_free(initial_text);

  if (state.child_pid > 0) {
    int status = 0;
    (void)waitpid(state.child_pid, &status, 0);
  }

  return 0;
}
