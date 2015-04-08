#ifndef WINDOW_H
#define WINDOW_H

#include <stddef.h>
#include <stdbool.h>
#include "text.h"
#include "ui.h"
#include "syntax.h"

typedef struct Win Win;

typedef struct {
	void *data;
	void (*selection)(void *data, Filerange*);
} ViewEvent;

typedef struct {
	int width;          /* display width i.e. number of columns ocupied by this character */
	size_t len;         /* number of bytes the character displayed in this cell uses, for
	                       character which use more than 1 column to display, their lenght
	                       is stored in the leftmost cell wheras all following cells
	                       occupied by the same character have a length of 0. */
	char data[8];       /* utf8 encoded character displayed in this cell might not be the
			       the same as in the underlying text, for example tabs get expanded */
	bool istab;
	unsigned int attr;
} Cell;

typedef struct Line Line;
struct Line {               /* a line on the screen, *not* in the file */
	Line *prev, *next;  /* pointer to neighbouring screen lines */
	size_t len;         /* line length in terms of bytes */
	size_t lineno;      /* line number from start of file */
	int width;          /* zero based position of last used column cell */
	Cell cells[];       /* win->width cells storing information about the displayed characters */
};

typedef struct {
	size_t line;
	size_t col;
} CursorPos;

Win *window_new(Text*, ViewEvent*);
void window_ui(Win*, UiWin*);
/* change associated text displayed in this window */
void window_reload(Win*, Text*);
void window_free(Win*);

/* keyboard input at cursor position */
size_t window_insert_key(Win*, const char *c, size_t len);
size_t window_replace_key(Win*, const char *c, size_t len);
size_t window_backspace_key(Win*);
size_t window_delete_key(Win*);

bool window_resize(Win*, int width, int height);
int window_height_get(Win*);
void window_draw(Win*);
/* changes how many spaces are used for one tab (must be >0), redraws the window */
void window_tabwidth_set(Win*, int tabwidth);

/* cursor movements which also update selection if one is active.
 * they return new cursor postion */
size_t window_char_next(Win*);
size_t window_char_prev(Win*);
size_t window_line_down(Win*);
size_t window_line_up(Win*);
size_t window_screenline_down(Win*);
size_t window_screenline_up(Win*);
size_t window_screenline_begin(Win*);
size_t window_screenline_middle(Win*);
size_t window_screenline_end(Win*);
/* move window content up/down, but keep cursor position unchanged unless it is
 * on a now invisible line in which case we try to preserve the column position */
size_t window_slide_up(Win*, int lines);
size_t window_slide_down(Win*, int lines);
/* scroll window contents up/down by lines, place the cursor on the newly
 * visible line, try to preserve the column position */
size_t window_scroll_up(Win*, int lines);
size_t window_scroll_down(Win*, int lines);
/* place the cursor at the start ot the n-th window line, counting from 1 */
size_t window_screenline_goto(Win*, int n);

/* get cursor position in bytes from start of the file */
size_t window_cursor_get(Win*);

const Line *window_lines_get(Win*);
/* get cursor position in terms of screen coordinates */
CursorPos window_cursor_getpos(Win*);
/* moves window viewport in direction until pos is visible. should only be
 * used for short distances between current cursor position and destination */
void window_scroll_to(Win*, size_t pos);
/* move cursor to a given position. changes the viewport to make sure that
 * position is visible. if the position is in the middle of a line, try to
 * adjust the viewport in such a way that the whole line is displayed */
void window_cursor_to(Win*, size_t pos);
/* redraw current cursor line at top/center/bottom of window */
void window_redraw_top(Win*);
void window_redraw_center(Win*);
void window_redraw_bottom(Win*);
/* start selected area at current cursor position. further cursor movements will
 * affect the selected region. */
void window_selection_start(Win*);
/* returns the currently selected text region, is either empty or well defined,
 * i.e. sel.start <= sel.end */
Filerange window_selection_get(Win*);
void window_selection_set(Win*, Filerange *sel);
/* clear selection and redraw window */
void window_selection_clear(Win*);
/* get the currently displayed area in bytes from the start of the file */
Filerange window_viewport_get(Win*);
/* associate a set of syntax highlighting rules to this window. */
void window_syntax_set(Win*, Syntax*);
Syntax *window_syntax_get(Win*);

#endif
