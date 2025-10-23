// By Dr. Eric Oliver Flores
// NED — simple terminal text editor (pure C99, no curses)
// - Clean start/exit using terminal alternate screen
// - Safe handling for empty/new files (no segfault on Return)
// - Resize-safe (signal handler sets a flag; resize done in main loop)

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>

#include "config.h"
#ifndef MAX_FILENAME
#define MAX_FILENAME 256
#endif

#define SCREEN_ROWS 24
#define SCREEN_COLS 80

/* ---------- Types ---------- */
typedef struct {
    char *chars;
    size_t length;
    size_t capacity;
} editor_row;

struct editor_config {
    struct termios orig_termios;
    int screen_rows;
    int screen_cols;
    int cursor_x;
    int cursor_y;
    int row_offset;
    int col_offset;
    int num_rows;
    editor_row *rows;
    char filename[MAX_FILENAME];
    bool modified;
    char status_msg[80];
};

static struct editor_config E;

/* ---------- Prototypes ---------- */
static void die(const char *s);
static void disable_raw_mode(void);
static void enable_raw_mode(void);
static int  get_window_size(int *rows, int *cols);
static void refresh_screen(void);
static void set_status_message(const char *fmt, ...);
static void save_file(void);
static void open_file(const char *path);
static void init_editor(void);
static void process_keypress(void);

/* ---------- Terminal ---------- */

static void wrlit(const char *s, size_t n) {
    while (n) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0) { if (errno == EINTR) continue; break; }
        s += (size_t)w; n -= (size_t)w;
    }
}

static void die(const char *s) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    wrlit("\x1b[?25h\x1b[0m\x1b[?1049l", 18); // show cursor, reset attrs, leave alt screen
    perror(s);
    exit(1);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    wrlit("\x1b[?25h\x1b[0m\x1b[?1049l", 18);
}

static int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        *rows = SCREEN_ROWS; *cols = SCREEN_COLS; return -1;
    }
    *cols = ws.ws_col; *rows = ws.ws_row; return 0;
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disable_raw_mode);
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");

    // Enter alt screen, clear, home, hide cursor
    wrlit("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l", 18);
}

/* ---------- Row ops ---------- */

static void editor_update_row(editor_row *row, const char *s, size_t len) {
    if (row->capacity < len + 1) {
        size_t new_cap = len + 1;
        char *p = realloc(row->chars, new_cap);
        if (!p) die("realloc");
        row->chars = p; row->capacity = new_cap;
    }
    memcpy(row->chars, s, len);
    row->chars[len] = '\0';
    row->length = len;
}

static void editor_free_row(editor_row *row) {
    free(row->chars);
    row->chars = NULL;
    row->length = row->capacity = 0;
}

static void editor_delete_row(int at) {
    if (at < 0 || at >= E.num_rows) return;
    editor_free_row(&E.rows[at]);
    memmove(&E.rows[at], &E.rows[at + 1], sizeof(editor_row) * (E.num_rows - at - 1));
    E.num_rows--;
    if (E.cursor_y >= E.num_rows) E.cursor_y = E.num_rows ? (E.num_rows - 1) : 0;
    if (E.num_rows) {
        int rowlen = (int)E.rows[E.cursor_y].length;
        if (E.cursor_x > rowlen) E.cursor_x = rowlen;
    } else {
        E.cursor_x = 0;
    }
    E.modified = true;
}

static void editor_insert_row(int at, const char *s, size_t len) {
    if (at < 0 || at > E.num_rows) return;
    editor_row *nr = realloc(E.rows, sizeof(editor_row) * (E.num_rows + 1));
    if (!nr) die("realloc");
    E.rows = nr;
    memmove(&E.rows[at + 1], &E.rows[at], sizeof(editor_row) * (E.num_rows - at));
    E.rows[at].chars = NULL;
    E.rows[at].capacity = 0;
    E.rows[at].length = 0;
    editor_update_row(&E.rows[at], s, len);
    E.num_rows++;
    E.modified = true;
}

static void editor_row_insert_char(editor_row *row, int at, int c) {
    if (at < 0 || at > (int)row->length) at = (int)row->length;
    if (row->capacity < row->length + 2) {
        size_t new_cap = (row->length + 2) * 2;
        char *p = realloc(row->chars, new_cap);
        if (!p) die("realloc");
        row->chars = p; row->capacity = new_cap;
    }
    memmove(&row->chars[at + 1], &row->chars[at], row->length - (size_t)at + 1);
    row->chars[at] = (char)c;
    row->length++;
    E.modified = true;
}

static void editor_row_delete_char(editor_row *row, int at) {
    if (at < 0 || at >= (int)row->length) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->length - (size_t)at);
    row->length--;
    E.modified = true;
}

static void editor_row_append_string(editor_row *row, const char *s, size_t len) {
    size_t new_len = row->length + len;
    if (row->capacity < new_len + 1) {
        size_t new_cap = new_len + 1;
        char *p = realloc(row->chars, new_cap);
        if (!p) die("realloc");
        row->chars = p; row->capacity = new_cap;
    }
    memcpy(&row->chars[row->length], s, len);
    row->length = new_len;
    row->chars[row->length] = '\0';
    E.modified = true;
}

/* ---------- Editor ops ---------- */

static void editor_insert_char(int c) {
    // Safety: ensure there is at least one row to type into
    if (E.num_rows == 0) editor_insert_row(0, "", 0);
    if (E.cursor_y == E.num_rows) editor_insert_row(E.num_rows, "", 0);
    editor_row_insert_char(&E.rows[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

// Fixed: handles empty buffer, end-of-buffer, and split line safely
static void editor_insert_newline(void) {
    // Case 1: completely empty file → first row, then a new empty row below
    if (E.num_rows == 0) {
        editor_insert_row(0, "", 0);
        editor_insert_row(1, "", 0);
        E.cursor_y = 1;
        E.cursor_x = 0;
        return;
    }

    // Case 2: cursor is below last row → append a new blank line
    if (E.cursor_y >= E.num_rows) {
        editor_insert_row(E.num_rows, "", 0);
        E.cursor_y = E.num_rows - 1; // newly created is last
        E.cursor_x = 0;
        return;
    }

    // Case 3: at start of current line → insert blank line above
    if (E.cursor_x == 0) {
        editor_insert_row(E.cursor_y, "", 0);
        E.cursor_y++;
        E.cursor_x = 0;
        return;
    }

    // Case 4: split current line at cursor
    editor_row *row = &E.rows[E.cursor_y];
    const char *tail = &row->chars[E.cursor_x];
    size_t tail_len = row->length - (size_t)E.cursor_x;

    editor_insert_row(E.cursor_y + 1, tail, tail_len);  // insert new row with tail
    // reacquire row pointer in case realloc moved memory
    row = &E.rows[E.cursor_y];
    row->length = (size_t)E.cursor_x;
    row->chars[row->length] = '\0';

    E.cursor_y++;
    E.cursor_x = 0;
}

static void editor_delete_char(void) {
    if (E.num_rows == 0) return;
    if (E.cursor_y >= E.num_rows) return;
    if (E.cursor_x == 0 && E.cursor_y == 0) return;

    editor_row *row = &E.rows[E.cursor_y];
    if (E.cursor_x > 0) {
        editor_row_delete_char(row, E.cursor_x - 1);
        E.cursor_x--;
    } else {
        // merge with previous line
        int prev = E.cursor_y - 1;
        int prev_len = (int)E.rows[prev].length;
        editor_row_append_string(&E.rows[prev], row->chars, row->length);
        editor_delete_row(E.cursor_y);
        E.cursor_y = prev;
        E.cursor_x = prev_len;
    }
}

/* ---------- File I/O ---------- */

static void open_file(const char *path) {
    if (path) {
        strncpy(E.filename, path, MAX_FILENAME - 1);
        E.filename[MAX_FILENAME - 1] = '\0';
    } else {
        E.filename[0] = '\0';
    }

    FILE *fp = path ? fopen(path, "r") : NULL;
    if (!fp) {
        set_status_message("New file: %s", E.filename[0] ? E.filename : "(unnamed)");
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        editor_insert_row(E.num_rows, line, (size_t)len);
    }
    free(line);
    fclose(fp);
    E.modified = false;
    set_status_message("Opened: %s", E.filename);
}

static void save_file(void) {
    if (!E.filename[0]) { set_status_message("ERROR: No filename"); return; }
    FILE *fp = fopen(E.filename, "w");
    if (!fp) { set_status_message("I/O error: %s", strerror(errno)); return; }
    for (int i = 0; i < E.num_rows; i++)
        fprintf(fp, "%s\n", E.rows[i].chars ? E.rows[i].chars : "");
    fclose(fp);
    E.modified = false;
    set_status_message("Saved: %s", E.filename);
}

/* ---------- Screen drawing ---------- */

struct abuf { char *b; int len; };
#define ABUF_INIT {NULL, 0}

static void ab_append(struct abuf *ab, const char *s, int len) {
    char *p = realloc(ab->b, (size_t)ab->len + (size_t)len);
    if (!p) return;
    memcpy(p + ab->len, s, (size_t)len);
    ab->b = p; ab->len += len;
}
static void ab_free(struct abuf *ab) { free(ab->b); ab->b = NULL; ab->len = 0; }

static void editor_scroll(void) {
    if (E.cursor_y < E.row_offset) E.row_offset = E.cursor_y;
    if (E.cursor_y >= E.row_offset + (E.screen_rows - 2))
        E.row_offset = E.cursor_y - (E.screen_rows - 2) + 1;

    if (E.cursor_x < E.col_offset) E.col_offset = E.cursor_x;
    if (E.cursor_x >= E.col_offset + E.screen_cols)
        E.col_offset = E.cursor_x - E.screen_cols + 1;
}

static void draw_rows(struct abuf *ab) {
    int text_rows = E.screen_rows - 2;
    for (int y = 0; y < text_rows; y++) {
        int filerow = y + E.row_offset;
        if (filerow >= E.num_rows) {
            ab_append(ab, "~\x1b[K\r\n", 6);
        } else {
            editor_row *row = &E.rows[filerow];
            int len = (int)row->length - E.col_offset;
            if (len < 0) len = 0;
            if (len > E.screen_cols) len = E.screen_cols;
            if (len > 0) ab_append(ab, &row->chars[E.col_offset], len);
            ab_append(ab, "\x1b[K\r\n", 5);
        }
    }
}

static void draw_status_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[7m", 3);
    char status[160];
    int len = snprintf(status, sizeof(status), "[%s] %s",
        E.filename[0] ? E.filename : "[No Name]",
        E.modified ? "*" : "");
    if (len > E.screen_cols) len = E.screen_cols;
    ab_append(ab, status, len);
    while (len < E.screen_cols) { ab_append(ab, " ", 1); len++; }
    ab_append(ab, "\x1b[m\r\n", 5);
}

static void draw_message_bar(struct abuf *ab) {
    ab_append(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.status_msg);
    if (msglen > E.screen_cols) msglen = E.screen_cols;
    if (msglen > 0) ab_append(ab, E.status_msg, msglen);
}

static void refresh_screen(void) {
    editor_scroll();
    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[?25l\x1b[H", 9);     // hide cursor, go home
    draw_rows(&ab);
    draw_status_bar(&ab);
    draw_message_bar(&ab);

    // place cursor
    int cy = (E.cursor_y - E.row_offset) + 1;
    int cx = (E.cursor_x - E.col_offset) + 1;
    if (cy < 1) cy = 1; if (cx < 1) cx = 1;
    char pos[32];
    int n = snprintf(pos, sizeof(pos), "\x1b[%d;%dH", cy, cx);
    ab_append(&ab, pos, n);
    ab_append(&ab, "\x1b[?25h", 6);          // show cursor

    wrlit(ab.b, (size_t)ab.len);
    ab_free(&ab);
}

static void set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
    va_end(ap);
}

/* ---------- Input ---------- */

static void editor_move_cursor(int key) {
    if (E.num_rows == 0) return;
    editor_row *row = (E.cursor_y >= E.num_rows) ? NULL : &E.rows[E.cursor_y];

    switch (key) {
        case 'A': if (E.cursor_y > 0) E.cursor_y--; break;                 // Up
        case 'B': if (E.cursor_y < E.num_rows - 1) E.cursor_y++; break;    // Down
        case 'D':                                                         // Left
            if (E.cursor_x > 0) E.cursor_x--;
            else if (E.cursor_y > 0) { E.cursor_y--; E.cursor_x = (int)E.rows[E.cursor_y].length; }
            break;
        case 'C':                                                         // Right
            if (row && E.cursor_x < (int)row->length) E.cursor_x++;
            else if (E.cursor_y < E.num_rows - 1) { E.cursor_y++; E.cursor_x = 0; }
            break;
    }
    row = (E.cursor_y >= E.num_rows) ? NULL : &E.rows[E.cursor_y];
    int rowlen = row ? (int)row->length : 0;
    if (E.cursor_x > rowlen) E.cursor_x = rowlen;
}

static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != '\x1b') return c;
    unsigned char s1; if (read(STDIN_FILENO, &s1, 1) != 1) return '\x1b';
    if (s1 != '[') return '\x1b';
    unsigned char s2; if (read(STDIN_FILENO, &s2, 1) != 1) return '\x1b';
    switch (s2) { case 'A': case 'B': case 'C': case 'D': return s2; }
    return '\x1b';
}

static void process_keypress(void) {
    int k = read_key();
    if (k < 0) return;

    switch (k) {
        case '\r': editor_insert_newline(); break;
        case 17:   /* Ctrl-Q */ exit(0);   // atexit() will clean up the TTY
        case 19:   /* Ctrl-S */ save_file(); break;
        case 127:  /* Backspace */ editor_delete_char(); break;
        case 'A': case 'B': case 'C': case 'D':
            editor_move_cursor(k); break;
        default:
            if (k >= 32 && k < 127) editor_insert_char(k);
            break;
    }
}

/* ---------- Resize Handling ---------- */
static volatile sig_atomic_t ned_need_resize = 0;
static void on_winch(int sig) { (void)sig; ned_need_resize = 1; }

/* ---------- Init / Main ---------- */

static void init_editor(void) {
    E.cursor_x = E.cursor_y = 0;
    E.row_offset = E.col_offset = 0;
    E.num_rows = 0;
    E.rows = NULL;
    E.filename[0] = '\0';
    E.modified = false;
    E.status_msg[0] = '\0';
    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
        E.screen_rows = SCREEN_ROWS; E.screen_cols = SCREEN_COLS;
    }
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    signal(SIGWINCH, on_winch);

    if (argc > 1) open_file(argv[1]);
    else set_status_message("Help: Ctrl+S=Save | Ctrl+Q=Quit");

    for (;;) {
        if (ned_need_resize) {
            ned_need_resize = 0;
            get_window_size(&E.screen_rows, &E.screen_cols);
        }
        refresh_screen();
        process_keypress();
    }
    return 0;
}

