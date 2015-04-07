#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <curses.h>
#include <locale.h>
#include <sys/ioctl.h>

#include "ui.h"
#include "ui-curses.h"
#include "util.h"

#ifdef NCURSES_VERSION
# ifndef NCURSES_EXT_COLORS
#  define NCURSES_EXT_COLORS 0
# endif
# if !NCURSES_EXT_COLORS
#  define MAX_COLOR_PAIRS 256
# endif
#endif
#ifndef MAX_COLOR_PAIRS
# define MAX_COLOR_PAIRS COLOR_PAIRS
#endif

#ifdef PDCURSES
int ESCDELAY;
#endif
#ifndef NCURSES_REENTRANT
# define set_escdelay(d) (ESCDELAY = (d))
#endif

#if 0
#define wresize(win, y, x) do { \
	if (wresize(win, y, x) == ERR) { \
		printf("ERROR resizing: %d x %d\n", x, y); \
	} else { \
		printf("OK resizing: %d x %d\n", x, y); \
	} \
	fflush(stdout); \
} while (0);

#define mvwin(win, y, x) do { \
	if (mvwin(win, y, x) == ERR) { \
		printf("ERROR moving: %d x %d\n", x, y); \
	} else { \
		printf("OK moving: %d x %d\n", x, y); \
	} \
	fflush(stdout); \
} while (0);
#endif

typedef struct UiCursesWin UiCursesWin;

typedef struct {
	Ui ui;                    /* generic ui interface, has to be the first struct member */
	Editor *ed;               /* editor instance to which this ui belongs */
	UiCursesWin *windows;     /* all windows managed by this ui */
	UiCursesWin *selwin;      /* the currently selected layout */
	char prompt_title[255];   /* prompt_title[0] == '\0' if prompt isn't shown */
	UiCursesWin *prompt_win;  /* like a normal window but without a status bar */
	char info[255];           /* info message displayed at the bottom of the screen */
	int width, height;        /* terminal dimensions available for all windows */
	enum UiLayout layout;     /* whether windows are displayed horizontally or vertically */
} UiCurses;

struct UiCursesWin {
	UiWin uiwin;              /* generic interface, has to be the first struct member */
	UiCurses *ui;             /* ui which manages this window */
	Text *text;               /* underlying text management */
	Win *view;                /* current viewport */
	WINDOW *win;              /* curses window for the text area */
	WINDOW *winstatus;        /* curses window for the status bar */
	WINDOW *winside;          /* curses window for the side bar (line numbers) */
	int width, height;        /* window dimension including status bar */
	int x, y;                 /* window position */
	int sidebar_width;        /* width of the sidebar showing line numbers etc. */
	UiCursesWin *next, *prev; /* pointers to neighbouring windows */
	enum UiOption options;    /* display settings for this window */
};

static unsigned int color_hash(short fg, short bg) {
	if (fg == -1)
		fg = COLORS;
	if (bg == -1)
		bg = COLORS + 1;
	return fg * (COLORS + 2) + bg;
}

static short color_get(short fg, short bg) {
	static bool has_default_colors;
	static short *color2palette, default_fg, default_bg;
	static short color_pairs_max, color_pair_current;

	if (!color2palette) {
		pair_content(0, &default_fg, &default_bg);
		if (default_fg == -1)
			default_fg = COLOR_WHITE;
		if (default_bg == -1)
			default_bg = COLOR_BLACK;
		has_default_colors = (use_default_colors() == OK);
		color_pairs_max = MIN(COLOR_PAIRS, MAX_COLOR_PAIRS);
		if (COLORS)
			color2palette = calloc((COLORS + 2) * (COLORS + 2), sizeof(short));
	}

	if (fg >= COLORS)
		fg = default_fg;
	if (bg >= COLORS)
		bg = default_bg;

	if (!has_default_colors) {
		if (fg == -1)
			fg = default_fg;
		if (bg == -1)
			bg = default_bg;
	}

	if (!color2palette || (fg == -1 && bg == -1))
		return 0;

	unsigned int index = color_hash(fg, bg);
	if (color2palette[index] == 0) {
		short oldfg, oldbg;
		if (++color_pair_current >= color_pairs_max)
			color_pair_current = 1;
		pair_content(color_pair_current, &oldfg, &oldbg);
		unsigned int old_index = color_hash(oldfg, oldbg);
		if (init_pair(color_pair_current, fg, bg) == OK) {
			color2palette[old_index] = 0;
			color2palette[index] = color_pair_current;
		}
	}

	return color2palette[index];
}

static void ui_window_resize(UiCursesWin *win, int width, int height) {
	win->width = width;
	win->height = height;
	if (win->winstatus)
		wresize(win->winstatus, 1, width);
	wresize(win->win, win->winstatus ? height - 1 : height, width - win->sidebar_width);
	if (win->winside)
		wresize(win->winside, height-1, win->sidebar_width);
	window_resize(win->view, width - win->sidebar_width, win->winstatus ? height - 1 : height);
}

static void ui_window_move(UiCursesWin *win, int x, int y) {
	win->x = x;
	win->y = y;
	mvwin(win->win, y, x + win->sidebar_width);
	if (win->winside)
		mvwin(win->winside, y, x);
	if (win->winstatus)
		mvwin(win->winstatus, y + win->height - 1, x);
}

static void ui_window_draw_status(UiWin *w) {
	UiCursesWin *win = (UiCursesWin*)w;
	if (!win->winstatus)
		return;
	UiCurses *uic = win->ui;
	Editor *vis = uic->ed;
	bool focused = uic->selwin == win;
	const char *filename = text_filename_get(win->text);
	CursorPos pos = window_cursor_getpos(win->view);
	wattrset(win->winstatus, focused ? A_REVERSE|A_BOLD : A_REVERSE);
	mvwhline(win->winstatus, 0, 0, ' ', win->width);
	mvwprintw(win->winstatus, 0, 0, "%s %s %s %s",
	          "", // TODO mode->name && mode->name[0] == '-' ? mode->name : "",
	          filename ? filename : "[No Name]",
	          text_modified(win->text) ? "[+]" : "",
	          vis->recording ? "recording": "");
	char buf[win->width + 1];
	int len = snprintf(buf, win->width, "%zd, %zd", pos.line, pos.col);
	if (len > 0) {
		buf[len] = '\0';
		mvwaddstr(win->winstatus, 0, win->width - len - 1, buf);
	}
}

static void ui_window_draw(UiWin *w) {
	UiCursesWin *win = (UiCursesWin*)w;
	if (win->winstatus)
		ui_window_draw_status((UiWin*)win);
	window_draw(win->view);
	window_cursor_to(win->view, window_cursor_get(win->view));
}

static void ui_window_reload(UiWin *w, Text *text) {
	UiCursesWin *win = (UiCursesWin*)w;
	win->text = text;
	win->sidebar_width = 0;
	ui_window_draw(w);
}

static void ui_window_draw_sidebar(UiCursesWin *win, const Line *line) {
	wattrset(win->winside,COLOR_PAIR(5));
	if (!win->winside || !line)
		return;
	int sidebar_width = snprintf(NULL, 0, "%zd", line->lineno + win->height - 2) + 1;
	if (win->sidebar_width != sidebar_width) {
		win->sidebar_width = sidebar_width;
		ui_window_resize(win, win->width, win->height);
		ui_window_move(win, win->x, win->y);
	} else {
		int i = 0;
		size_t prev_lineno = 0;
		size_t cursor_lineno = window_cursor_getpos(win->view).line;
		werase(win->winside);
		for (const Line *l = line; l; l = l->next, i++) {
			if (l->lineno != prev_lineno) {
				if (win->options & UI_OPTION_LINE_NUMBERS_ABSOLUTE) {
					mvwprintw(win->winside, i, 0, "%*u", sidebar_width-1, l->lineno);
				} else if (win->options & UI_OPTION_LINE_NUMBERS_RELATIVE) {
					size_t rel = l->lineno > cursor_lineno ?
					             l->lineno - cursor_lineno :
					             cursor_lineno - l->lineno;
					mvwprintw(win->winside, i, 0, "%*u", sidebar_width-1, rel);
				}
			}
			prev_lineno = l->lineno;
		}
		mvwvline(win->winside, 0, sidebar_width-1, ' ', win->height-1);
	}
}

static void ui_window_update(UiCursesWin *win) {
	if (win->winstatus)
		wnoutrefresh(win->winstatus);
	if (win->winside)
		wnoutrefresh(win->winside);
	wnoutrefresh(win->win);
}

static void update(Ui *ui) {
	UiCurses *uic = (UiCurses*)ui;
	for (UiCursesWin *win = uic->windows; win; win = win->next) {
		if (win != uic->selwin)
			ui_window_update(win);
	}

	if (uic->selwin)
		ui_window_update(uic->selwin);
	if (uic->prompt_title[0]) {
		wnoutrefresh(uic->prompt_win->win);
		ui_window_update(uic->prompt_win);
	}
	doupdate();
}

static void arrange(Ui *ui, enum UiLayout layout) {
	UiCurses *uic = (UiCurses*)ui;
	uic->layout = layout;
	int n = 0, x = 0, y = 0;
	for (UiCursesWin *win = uic->windows; win; win = win->next)
		n++;
	int max_height = uic->height - !!(uic->prompt_title[0] || uic->info[0]);
	int width = (uic->width / MAX(1, n)) - 1;
	int height = max_height / MAX(1, n);
	for (UiCursesWin *win = uic->windows; win; win = win->next) {
		if (layout == UI_LAYOUT_HORIZONTAL) {
			ui_window_resize(win, uic->width, win->next ? height : max_height - y);
			ui_window_move(win, x, y);
			y += height;
		} else {
			ui_window_resize(win, win->next ? width : uic->width - x, max_height);
			ui_window_move(win, x, y);
			x += width;
			if (win->next)
				mvvline(0, x++, ACS_VLINE, max_height);
		}
	}
}

static void draw(Ui *ui) { 
	UiCurses *uic = (UiCurses*)ui;
	erase();
	arrange(ui, uic->layout);
	
	for (UiCursesWin *win = uic->windows; win; win = win->next)
		ui_window_draw((UiWin*)win);

	if (uic->info[0]) {
	        attrset(A_BOLD);
        	mvaddstr(uic->height-1, 0, uic->info);
	}

	if (uic->prompt_title[0]) {
	        attrset(A_NORMAL);
        	mvaddstr(uic->height-1, 0, uic->prompt_title);
		ui_window_draw((UiWin*)uic->prompt_win);
	}

	wnoutrefresh(stdscr);
}

static void ui_resize_to(Ui *ui, int width, int height) {
	UiCurses *uic = (UiCurses*)ui;
	uic->width = width;
	uic->height = height;
	if (uic->prompt_title[0]) {
		size_t title_width = strlen(uic->prompt_title);
		ui_window_resize(uic->prompt_win, width - title_width, 1);
		ui_window_move(uic->prompt_win, title_width, height-1);
	}
	draw(ui);
}

static void ui_resize(Ui *ui) {
	struct winsize ws;
	int width, height;

	if (ioctl(0, TIOCGWINSZ, &ws) == -1) {
		getmaxyx(stdscr, height, width);
	} else {
		width = ws.ws_col;
		height = ws.ws_row;
	}

	resizeterm(height, width);
	wresize(stdscr, height, width);
	ui_resize_to(ui, width, height);
}

static void ui_window_free(UiWin *w) {
	UiCursesWin *win = (UiCursesWin*)w;
	if (!win)
		return;
	UiCurses *uic = win->ui;
	if (win->prev)
		win->prev->next = win->next;
	if (win->next)
		win->next->prev = win->prev;
	if (uic->windows == win)
		uic->windows = win->next;
	if (uic->selwin == win)
		uic->selwin = NULL;
	win->next = win->prev = NULL;
	if (win->winstatus)
		delwin(win->winstatus);
	if (win->winside)
		delwin(win->winside);
	if (win->win)
		delwin(win->win);
	window_free(win->view);
	free(win);
}

static Win *ui_window_view_get(UiWin *win) {
	UiCursesWin *cwin = (UiCursesWin*)win;
	return cwin->view;
}

static void ui_window_cursor_to(UiWin *w, int x, int y) {
	UiCursesWin *win = (UiCursesWin*)w;
	wmove(win->win, y, x);
	ui_window_draw_status(w);
	if (win->options & UI_OPTION_LINE_NUMBERS_RELATIVE)
		ui_window_draw_sidebar(win, window_lines_get(win->view));
}

static void ui_window_draw_text(UiWin *w, const Line *line) {
	UiCursesWin *win = (UiCursesWin*)w;
	wmove(win->win, 0, 0);
	for (const Line *l = line; l; l = l->next) {
		/* add a single space in an otherwise empty line to make
		 * the selection cohorent */
		if (l->width == 1 && l->cells[0].data[0] == '\n') {
			wattrset(win->win, l->cells[0].attr);
			waddstr(win->win, " \n");
		} else {
			for (int x = 0; x < l->width; x++) {
				wattrset(win->win, l->cells[x].attr);
				waddstr(win->win, l->cells[x].data);
			}
		}
		wclrtoeol(win->win);
	}
	wclrtobot(win->win);

	ui_window_draw_sidebar(win, line);
}

static void ui_window_focus(UiWin *w) {
	UiCursesWin *win = (UiCursesWin*)w;
	UiCursesWin *oldsel = win->ui->selwin;
	win->ui->selwin = win;
	if (oldsel)
		ui_window_draw_status((UiWin*)oldsel);
	ui_window_draw_status(w);
}

static void ui_window_options(UiWin *w, enum UiOption options) {
	UiCursesWin *win = (UiCursesWin*)w;
	win->options = options;
	switch (options) {
	case UI_OPTION_LINE_NUMBERS_NONE:
		if (win->winside) {
			delwin(win->winside);
			win->winside = NULL;
			win->sidebar_width = 0;
		}
		break;
	case UI_OPTION_LINE_NUMBERS_ABSOLUTE:
	case UI_OPTION_LINE_NUMBERS_RELATIVE:
		if (!win->winside)
			win->winside = newwin(1, 1, 1, 1);
		break;
	}
	ui_window_draw(w);
}

static UiWin *ui_window_new(Ui *ui, Text *text) {
	UiCurses *uic = (UiCurses*)ui;
	UiCursesWin *win = calloc(1, sizeof(UiCursesWin));
	if (!win)
		return NULL;

	win->uiwin = (UiWin) {
		.draw = ui_window_draw,
		.draw_status = ui_window_draw_status,
		.draw_text = ui_window_draw_text,
		.cursor_to = ui_window_cursor_to,
		.view_get = ui_window_view_get,
		.options = ui_window_options,
		.reload = ui_window_reload,
	};

	if (!(win->view = window_new(text, &win->uiwin, uic->width, uic->height)) ||
	    !(win->win = newwin(0, 0, 0, 0)) ||
	    !(win->winstatus = newwin(1, 0, 0, 0))) {
		ui_window_free((UiWin*)win);
		return NULL;
	}

	win->ui = uic;
	win->text = text;
	if (uic->windows)
		uic->windows->prev = win;
	win->next = uic->windows;
	uic->windows = win;

	return &win->uiwin;
}

static void info(Ui *ui, const char *msg, va_list ap) {
	UiCurses *uic = (UiCurses*)ui;
	vsnprintf(uic->info, sizeof(uic->info), msg, ap);
	draw(ui);
}

static void info_hide(Ui *ui) {
	UiCurses *uic = (UiCurses*)ui;
	if (uic->info[0]) { 
		uic->info[0] = '\0';
		draw(ui);
	}
}

static UiWin *prompt_new(Ui *ui, Text *text) {
	UiCurses *uic = (UiCurses*)ui;
	if (uic->prompt_win)
		return (UiWin*)uic->prompt_win;
	UiWin *uiwin = ui_window_new(ui, text);
	UiCursesWin *win = (UiCursesWin*)uiwin;
	if (!win)
		return NULL;
	uic->windows = win->next;
	if (uic->windows)
		uic->windows->prev = NULL;
	if (win->winstatus)
		delwin(win->winstatus);
	if (win->winside)
		delwin(win->winside);
	win->winstatus = NULL;
	win->winside = NULL;
	uic->prompt_win = win;
	return uiwin;
}

static void prompt(Ui *ui, const char *title, const char *text) {
	UiCurses *uic = (UiCurses*)ui;
	if (uic->prompt_title[0])
		return;
	size_t text_len = strlen(text);
	strncpy(uic->prompt_title, title, sizeof(uic->prompt_title)-1);
	text_insert(uic->prompt_win->text, 0, text, text_len);
	window_cursor_to(uic->prompt_win->view, text_len);
	ui_resize_to(ui, uic->width, uic->height);
}

static char *prompt_input(Ui *ui) {
	UiCurses *uic = (UiCurses*)ui;
	if (!uic->prompt_win)
		return NULL;
	Text *text = uic->prompt_win->text;
	char *buf = malloc(text_size(text) + 1);
	if (!buf)
		return NULL;
	size_t len = text_bytes_get(text, 0, text_size(text), buf);
	buf[len] = '\0';
	return buf;
}

static void prompt_hide(Ui *ui) {
	UiCurses *uic = (UiCurses*)ui;
	uic->prompt_title[0] = '\0';
	if (uic->prompt_win) {
		while (text_undo(uic->prompt_win->text) != EPOS);
		window_cursor_to(uic->prompt_win->view, 0);
	}
	ui_resize_to(ui, uic->width, uic->height);
}

static bool ui_init(Ui *ui, Editor *ed) {
	UiCurses *uic = (UiCurses*)ui;
	uic->ed = ed;
	return true;
}

static void ui_suspend(Ui *ui) {
	endwin();
	raise(SIGSTOP);
}

Ui *ui_curses_new(void) {
	setlocale(LC_CTYPE, "");
	if (!getenv("ESCDELAY"))
		set_escdelay(50);
	char *term = getenv("TERM");
	if (!term)
		term = "xterm";
	if (!newterm(term, stderr, stdin))
		return NULL;
	start_color();
	raw();
	noecho();
	keypad(stdscr, TRUE);
	meta(stdscr, TRUE);
	/* needed because we use getch() which implicitly calls refresh() which
	   would clear the screen (overwrite it with an empty / unused stdscr */
	refresh();

	UiCurses *uic = calloc(1, sizeof(UiCurses));
	Ui *ui = (Ui*)uic;
	if (!uic)
		return NULL;

	*ui = (Ui) {
		.init = ui_init,
		.free = ui_curses_free,
		.suspend = ui_suspend,
		.resume = ui_resize,
		.resize = ui_resize,
		.update = update,
		.window_new = ui_window_new,
		.window_free = ui_window_free,
		.window_focus = ui_window_focus,
		.prompt_new = prompt_new,
		.prompt = prompt,
		.prompt_input = prompt_input,
		.prompt_hide = prompt_hide,
		.draw = draw,
		.arrange = arrange,
		.info = info,
		.info_hide = info_hide,
		.color_get = color_get,
	};

	ui_resize(ui);

	return ui;
}

void ui_curses_free(Ui *ui) {
	UiCurses *uic = (UiCurses*)ui;
	if (!uic)
		return;
	ui_window_free((UiWin*)uic->prompt_win);
	while (uic->windows)
		ui_window_free((UiWin*)uic->windows);
	endwin();
	free(uic);
}

