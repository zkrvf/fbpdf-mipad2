/*
 * FBPDF LINUX FRAMEBUFFER PDF VIEWER
 *
 * Copyright (C) 2009-2025 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <ctype.h>
#include <errno.h>

#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/time.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "draw.h"
#include "doc.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#define PAGESTEPS	8
#define MAXZOOM		1000
#define MARGIN		1
#define CTRLKEY(x)	((x) - 96)
#define ISMARK(x)	(isalpha(x) || (x) == '\'' || (x) == '`')
#define KEY_MENU_OPEN  0x100
#define KEY_MENU_CLOSE 0x101
#define MT_SLOTS 10

static struct doc *doc;
static char *pbuf;		/* current page */
static int srows, scols;	/* screen dimentions */
static int prows, pcols;	/* current page dimensions */
static int prow, pcol;		/* page position */
static int srow, scol;		/* screen position */
static int bpp;			/* bytes per pixel */

static struct termios termios;
static char filename[256];
static int mark[128];		/* mark page number */
static int mark_row[128];	/* mark head position */
static int num = 1;		/* page number */
static int numdiff;		/* G command page number difference */
static int zoom = 150;
static int zoom_def = 150;	/* default zoom */
static int rotate;
static int count;
static int invert;		/* invert colors? */


/* ===== Resume state (per file) ===== */
static unsigned long hash_path(const char *s)
{
	unsigned long h = 5381;
	unsigned char c;
	while ((c = (unsigned char)*s++))
		h = ((h << 5) + h) + c;
	return h;
}

static void ensure_state_dir(char *out, size_t outsz)
{
	const char *home = getenv("HOME");
	char dir[256];
	if (!home) home = "/tmp";
	snprintf(dir, sizeof(dir), "%s/.local", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.local/state", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.local/state/fbpdf", home);
	mkdir(dir, 0755);

	unsigned long h = hash_path(filename);
	snprintf(out, outsz, "%s/.local/state/fbpdf/%lx.state", home, h);
}

static void load_state(void)
{
	char path[320];
	int p = 0;
	FILE *fp;
	ensure_state_dir(path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (fscanf(fp, "%d", &p) == 1 && p > 0)
		num = p;
	fclose(fp);
}

static void save_state(void)
{
	char path[320];
	FILE *fp;
	ensure_state_dir(path, sizeof(path));
	fp = fopen(path, "w");
	if (!fp)
		return;
	fprintf(fp, "%d\n", num);
	fclose(fp);
}


static void draw(void)
{
	int i;
	char *rbuf = malloc(scols * bpp);
	for (i = srow; i < srow + srows; i++) {
		int cbeg = MAX(scol, pcol);
		int cend = MIN(scol + scols, pcol + pcols);
		memset(rbuf, 0, scols * bpp);
		if (i >= prow && i < prow + prows && cbeg < cend) {
			memcpy(rbuf + (cbeg - scol) * bpp,
				pbuf + ((i - prow) * pcols + cbeg - pcol) * bpp,
				(cend - cbeg) * bpp);
		}
		memcpy(fb_mem(i - srow), rbuf, scols * bpp);
	}
	free(rbuf);
}

static int loadpage(int p)
{
	int i;
	if (p < 1 || p > doc_pages(doc))
		return 1;
	prows = 0;
	free(pbuf);
	pbuf = doc_draw(doc, p, zoom, rotate, bpp, &prows, &pcols);
	if (invert) {
		for (i = 0; i < prows * pcols * bpp; i++) {
			int val = (unsigned char) pbuf[i] ^ 0xff;
			pbuf[i] = val * invert / 255 + (255 - invert);
		}
	}
	prow = -prows / 2;
	pcol = -pcols / 2;
	num = p;
	return 0;
}

static void zoom_page(int z)
{
	int _zoom = zoom;
	zoom = MIN(MAXZOOM, MAX(1, z));
	if (!loadpage(num))
		srow = srow * zoom / _zoom;
}

static void setmark(int c)
{
	if (ISMARK(c)) {
		mark[c] = num;
		mark_row[c] = srow * 100 / zoom;
	}
}

static void jmpmark(int c, int offset)
{
	if (c == '`')
		c = '\'';
	if (ISMARK(c) && mark[c]) {
		int dst = mark[c];
		int dst_row = offset ? mark_row[c] * zoom / 100 : 0;
		setmark('\'');
		if (!loadpage(dst))
			srow = offset ? dst_row : prow;
	}
}

static int readkey(void)
{
	unsigned char b;
	if (read(0, &b, 1) <= 0)
		return -1;
	return b;
}

static int getcount(int def)
{
	int result = count ? count : def;
	count = 0;
	return result;
}

static void printinfo(void)
{
	printf("\x1b[H");
	printf("FBPDF:     file:%s  page:%d(%d)  zoom:%d%% \x1b[K\r",
		filename, num, doc_pages(doc), zoom);
	fflush(stdout);
}

static void term_setup(void)
{
	struct termios newtermios;
	tcgetattr(0, &termios);
	newtermios = termios;
	newtermios.c_lflag &= ~ICANON;
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(0, TCSAFLUSH, &newtermios);
	printf("\x1b[?25l");		/* hide the cursor */
	printf("\x1b[2J");		/* clear the screen */
	fflush(stdout);
}

static void term_cleanup(void)
{
	tcsetattr(0, 0, &termios);
	printf("\x1b[?25h\n");		/* show the cursor */
}

static void sigcont(int sig)
{
	term_setup();
}

static int reload(void)
{
	doc_close(doc);
	doc = doc_open(filename);
	load_state();
	if (num < 1) num = 1;
	if (num > doc_pages(doc)) num = doc_pages(doc);

	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "\nfbpdf: cannot open <%s>\n", filename);
		return 1;
	}
	if (!loadpage(num))
		draw();
	return 0;
}

/* this can be optimised based on framebuffer pixel format */
void fb_set(char *d, unsigned r, unsigned g, unsigned b)
{
	unsigned c = fb_val(r, g, b);
	int i;
	for (i = 0; i < bpp; i++)
		d[i] = (c >> (i << 3)) & 0xff;
}

/* forward decl: menu_bar_h is defined later (menu state) */
static int menu_bar_h;
static int menu_exit_x0, menu_exit_y0, menu_exit_x1, menu_exit_y1;


static void menu_fill_rect(int r0, int c0, int rh, int cw, unsigned r, unsigned g, unsigned b)
{
        int y, x;
        if (r0 < 0) r0 = 0;
        if (c0 < 0) c0 = 0;
        for (y = r0; y < r0 + rh && y < srows; y++) {
                char *row = fb_mem(y);
                for (x = c0; x < c0 + cw && x < scols; x++)
                        fb_set(row + x * bpp, r, g, b);
        }
}

static void menu_hline(int r, int c0, int c1, int t)
{
        if (c1 < c0) { int tmp = c0; c0 = c1; c1 = tmp; }
        menu_fill_rect(r, c0, t, c1 - c0 + 1, 240, 240, 240);
}

static void menu_vline(int c, int r0, int r1, int t)
{
        if (r1 < r0) { int tmp = r0; r0 = r1; r1 = tmp; }
        menu_fill_rect(r0, c, r1 - r0 + 1, t, 240, 240, 240);
}

static void menu_diag(int r0, int c0, int r1, int c1, int t)
{
        int dr = (r1 >= r0) ? 1 : -1;
        int dc = (c1 >= c0) ? 1 : -1;
        int ar = (r1 - r0) * dr;
        int ac = (c1 - c0) * dc;
        int n = ar > ac ? ar : ac;
        int r = r0, c = c0;
        for (int i = 0; i <= n; i++) {
                menu_fill_rect(r, c, t, t, 240, 240, 240);
                if (ar) r = r0 + (i * (r1 - r0)) / n;
                if (ac) c = c0 + (i * (c1 - c0)) / n;
        }
}

static void menu_draw_exit_label(int r0, int c0, int rh, int cw)
{
        int pad = rh / 6;
        int t = rh / 10;
        int gap = cw / 25;
        int lh, lw;
        int x, y;
        if (pad < 3) pad = 3;
        if (t < 2) t = 2;
        if (t > 6) t = 6;
        if (gap < 4) gap = 4;
        if (gap > 12) gap = 12;

        lh = rh - 2 * pad;
        y = r0 + pad;
        lw = (cw - 5 * gap) / 4;
        if (lw < 12) lw = 12;

        /* E */
        x = c0 + gap;
        menu_vline(x, y, y + lh, t);
        menu_hline(y, x, x + lw, t);
        menu_hline(y + lh / 2, x, x + (lw * 4) / 5, t);
        menu_hline(y + lh, x, x + lw, t);

        /* X */
        x += lw + gap;
        menu_diag(y, x, y + lh, x + lw, t);
        menu_diag(y, x + lw, y + lh, x, t);

        /* I */
        x += lw + gap;
        menu_hline(y, x, x + lw, t);
        menu_hline(y + lh, x, x + lw, t);
        menu_vline(x + lw / 2, y, y + lh, t);

        /* T */
        x += lw + gap;
        menu_hline(y, x, x + lw, t);
        menu_vline(x + lw / 2, y, y + lh, t);
}

static void menu_draw_overlay(void)
{
        int pad = 6;
        int box_w;

        menu_bar_h = srows / 10;
        if (menu_bar_h < 44) menu_bar_h = 44;
        if (menu_bar_h > 120) menu_bar_h = 120;

        menu_fill_rect(0, 0, menu_bar_h, scols, 25, 25, 25);

        box_w = scols / 5;
        if (box_w < 90) box_w = 90;
        if (box_w > scols / 2) box_w = scols / 2;

        menu_exit_x0 = scols - box_w - pad;
        menu_exit_y0 = pad;
        menu_exit_x1 = scols - pad - 1;
        menu_exit_y1 = menu_bar_h - pad - 1;
        menu_fill_rect(menu_exit_y0, menu_exit_x0, menu_exit_y1 - menu_exit_y0 + 1, menu_exit_x1 - menu_exit_x0 + 1, 90, 90, 90);
        menu_draw_exit_label(menu_exit_y0, menu_exit_x0, menu_exit_y1 - menu_exit_y0 + 1, menu_exit_x1 - menu_exit_x0 + 1);
}

static int iswhite(char *pix)
{
	int val = 255 - invert;
	int i;
	for (i = 0; i < 3 && i < bpp; i++)
		if (((unsigned char) pix[i]) != val)
			return 0;
	return 1;
}

static int rmargin(void)
{
	int ret = 0;
	int i, j;
	for (i = 0; i < prows; i++) {
		j = pcols - 1;
		while (j > ret && iswhite(pbuf + (i * pcols + j) * bpp))
			j--;
		if (ret < j)
			ret = j;
	}
	return ret;
}

static int lmargin(void)
{
	int ret = pcols;
	int i, j;
	for (i = 0; i < prows; i++) {
		j = 0;
		while (j < ret && iswhite(pbuf + (i * pcols + j) * bpp))
			j++;
		if (ret > j)
			ret = j;
	}
	return ret;
}


/* ===== Button handling via evdev (Mi Pad 2) =====
 * event9  -> gpio-keys (KEY_VOLUMEUP / KEY_VOLUMEDOWN)
 * event10 -> gpio-keys (KEY_POWER)
 *
 * Vol+  -> Ctrl+F (next page)
 * Vol-  -> Ctrl+B (prev page)
 * Power long-press (>=700ms) -> q
 * Power short press -> ignored
 */
static int evfd_vol = -1;
static int evfd_touch = -1;

/* touch tracking for swipe-down menu */
static int touch_x_max = 0, touch_y_max = 0;
static int touch_down = 0;
static int touch_start_x = 0, touch_start_y = 0;
static int touch_last_x = 0, touch_last_y = 0;
static long long touch_tdown_ms = 0;
static int mt_slot = 0;
static int mt_x[MT_SLOTS], mt_y[MT_SLOTS];
static int mt_active[MT_SLOTS];

/* menu state */
static int menu_active = 0;
static int menu_bar_h = 0;
static int menu_exit_x0 = 0, menu_exit_y0 = 0, menu_exit_x1 = 0, menu_exit_y1 = 0;

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL);
}

static void buttons_init(void)
{
        if (evfd_vol == -1)
                evfd_vol = open("/dev/input/event9", O_RDONLY | O_NONBLOCK);

        if (evfd_touch == -1) {
                struct input_absinfo ax, ay;
                char path[64];
                for (int i = 0; i < 32; i++) {
                        int fd;
                        snprintf(path, sizeof(path), "/dev/input/event%d", i);
                        fd = open(path, O_RDONLY | O_NONBLOCK);
                        if (fd == -1)
                                continue;
                        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 &&
                            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0) {
                                evfd_touch = fd;
                                touch_x_max = ax.maximum;
                                touch_y_max = ay.maximum;
                                break;
                        }
                        close(fd);
                }
        }
}


static int map_evdev_event(const struct input_event *ev)
{
        if (ev->type != EV_KEY)
                return 0;
        /* act only on press/release; ignore repeats */
        if (ev->value != 1 && ev->value != 0)
                return 0;

        if (ev->code == KEY_VOLUMEUP && ev->value == 1)
                return CTRLKEY(98); /* b */
        if (ev->code == KEY_VOLUMEDOWN && ev->value == 1)
                return CTRLKEY(102); /* f */

        return 0;
}


static int eval_touch_gesture(int sx, int sy, int ex, int ey, long long dt)
{
        int xmax = touch_x_max ? touch_x_max : 4096;
        int ymax = touch_y_max ? touch_y_max : 4096;
        int dx = ex - sx;
        int dy = ey - sy;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;

        /* menu open: swipe down starting near the top */
        if (!menu_active) {
                if (sy <= ymax / 6 && dy >= ymax / 5 && adx <= xmax / 5 && dt <= 1000)
                        return KEY_MENU_OPEN;
                return 0;
        }

        /* menu active: tap to exit (hit-box) or tap anywhere to close */
        if (dt <= 500 && adx <= xmax / 80 && ady <= ymax / 80) {
                int xs = (sx * scols) / xmax;
                int ys = (sy * srows) / ymax;
                int xsm = (scols - 1) - xs;
                if ((ys >= menu_exit_y0 && ys <= menu_exit_y1 && xs >= menu_exit_x0 && xs <= menu_exit_x1) ||
                    (ys >= menu_exit_y0 && ys <= menu_exit_y1 && xsm >= menu_exit_x0 && xsm <= menu_exit_x1))
                        return 113; /* q */
                return KEY_MENU_CLOSE;
        }

        /* swipe up closes menu */
        if (dy <= -(ymax / 6))
                return KEY_MENU_CLOSE;

        return 0;
}

static int map_touch_event(const struct input_event *ev)
{
        static int abs_x = 0, abs_y = 0;

        if (ev->type == EV_ABS) {
                switch (ev->code) {
                case ABS_X:
                        abs_x = ev->value;
                        touch_last_x = abs_x;
                        break;
                case ABS_Y:
                        abs_y = ev->value;
                        touch_last_y = abs_y;
                        break;
                case ABS_MT_SLOT:
                        mt_slot = ev->value;
                        if (mt_slot < 0 || mt_slot >= MT_SLOTS)
                                mt_slot = 0;
                        break;
                case ABS_MT_POSITION_X:
                        mt_x[mt_slot] = ev->value;
                        if (mt_slot == 0)
                                touch_last_x = ev->value;
                        break;
                case ABS_MT_POSITION_Y:
                        mt_y[mt_slot] = ev->value;
                        if (mt_slot == 0)
                                touch_last_y = ev->value;
                        break;
                case ABS_MT_TRACKING_ID:
                        if (ev->value >= 0) {
                                mt_active[mt_slot] = 1;
                                if (mt_slot == 0) {
                                        touch_down = 1;
                                        touch_start_x = touch_last_x;
                                        touch_start_y = touch_last_y;
                                        touch_tdown_ms = now_ms();
                                }
                        } else {
                                mt_active[mt_slot] = 0;
                                if (mt_slot == 0 && touch_down) {
                                        long long dt = now_ms() - touch_tdown_ms;
                                        touch_down = 0;
                                        return eval_touch_gesture(touch_start_x, touch_start_y, touch_last_x, touch_last_y, dt);
                                }
                        }
                        break;
                }
        }

        if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
                if (ev->value == 1) {
                        touch_down = 1;
                        touch_start_x = touch_last_x;
                        touch_start_y = touch_last_y;
                        touch_tdown_ms = now_ms();
                } else if (ev->value == 0 && touch_down) {
                        long long dt = now_ms() - touch_tdown_ms;
                        touch_down = 0;
                        return eval_touch_gesture(touch_start_x, touch_start_y, touch_last_x, touch_last_y, dt);
                }
        }

        return 0;
}

static int readkey_with_buttons(void)
{
        struct pollfd fds[3];
        int nfds = 0;

        buttons_init();

        /* stdin (tty) */
        fds[nfds].fd = 0;
        fds[nfds].events = POLLIN;
        nfds++;

        if (evfd_touch != -1) {
                fds[nfds].fd = evfd_touch;
                fds[nfds].events = POLLIN;
                nfds++;
        }
        if (evfd_vol != -1) {
                fds[nfds].fd = evfd_vol;
                fds[nfds].events = POLLIN;
                nfds++;
        }

        for (;;) {
                int r = poll(fds, nfds, -1);
                if (r < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }

                /* touch + evdev */
                for (int i = 1; i < nfds; i++) {
                        if (fds[i].revents & POLLIN) {
                                struct input_event ev;
                                ssize_t n = read(fds[i].fd, &ev, sizeof(ev));
                                if (n == (ssize_t)sizeof(ev)) {
                                        int c = 0;
                                        if (fds[i].fd == evfd_touch)
                                                c = map_touch_event(&ev);
                                        else
                                                c = map_evdev_event(&ev);
                                        if (c)
                                                return c;
                                }
                        }
                }

                /* then tty */
                if (fds[0].revents & POLLIN)
                        return readkey();
        }
}



static void mainloop(void)
{
	int step = srows / PAGESTEPS;
	int hstep = scols / PAGESTEPS;
	int c;
	term_setup();
	signal(SIGCONT, sigcont);
	loadpage(num);
	zoom_page(pcols ? zoom * scols / pcols : zoom); /* autofit width */
	srow = prow;
	scol = -scols / 2;
	draw();
	while ((c = readkey_with_buttons()) != -1) {
                if (c == KEY_MENU_OPEN) {
                        if (!menu_active) {
                                menu_active = 1;
                                menu_draw_overlay();
                        }
                        continue;
                }
                if (c == KEY_MENU_CLOSE) {
                        if (menu_active) {
                                menu_active = 0;
                                draw();
                        }
                        continue;
                }
                if (menu_active && c != 'q')
                        continue;

                if (c == 'q') {
                        save_state();
                        break;
                }
                if (c == 'e' && reload()) {
                        save_state();
                        break;
                }
		switch (c) {	/* commands that do not require redrawing */
		case 'o':
			numdiff = num - getcount(num);
			break;
		case 'Z':
			count *= 10;
			zoom_def = getcount(zoom);
			break;
		case 'i':
			printinfo();
			break;
		case 27:
			count = 0;
			break;
		case 'm':
			setmark(readkey());
			break;
		case 'd':
			sleep(getcount(1));
			break;
		default:
			if (isdigit(c))
				count = count * 10 + c - '0';
		}
		switch (c) {	/* commands that require redrawing */
		case CTRLKEY('f'):
		case 'J':
			if (!loadpage(num + getcount(1)))
				srow = prow;
			break;
		case CTRLKEY('b'):
		case 'K':
			if (!loadpage(num - getcount(1)))
				srow = prow;
			break;
		case 'G':
			setmark('\'');
			if (!loadpage(getcount(doc_pages(doc) - numdiff) + numdiff))
				srow = prow;
			break;
		case 'O':
			numdiff = num - getcount(num);
			setmark('\'');
			if (!loadpage(num + numdiff))
				srow = prow;
			break;
		case 'z':
			count *= 10;
			zoom_page(getcount(zoom_def));
			break;
		case 'w':
			zoom_page(pcols ? zoom * scols / pcols : zoom);
			break;
		case 'W':
			if (lmargin() < rmargin())
				zoom_page(zoom * (scols - hstep) /
					(rmargin() - lmargin()));
			break;
		case 'f':
			zoom_page(prows ? zoom * srows / prows : zoom);
			break;
		case 'r':
			rotate = getcount(0);
			if (!loadpage(num))
				srow = prow;
			break;
		case '`':
		case '\'':
			jmpmark(readkey(), c == '`');
			break;
		case 'j':
			srow += step * getcount(1);
			break;
		case 'k':
			srow -= step * getcount(1);
			break;
		case 'l':
			scol += hstep * getcount(1);
			break;
		case 'h':
			scol -= hstep * getcount(1);
			break;
		case 'H':
			srow = prow;
			break;
		case 'L':
			srow = prow + prows - srows;
			break;
		case 'M':
			srow = prow + prows / 2 - srows / 2;
			break;
		case 'C':
			scol = -scols / 2;
			break;
		case ' ':
		case CTRLKEY('d'):
			srow += srows * getcount(1) - step;
			break;
		case 127:
		case CTRLKEY('u'):
			srow -= srows * getcount(1) - step;
			break;
		case '[':
			scol = pcol;
			break;
		case ']':
			scol = pcol + pcols - scols;
			break;
		case '{':
			scol = pcol + lmargin() - hstep / 2;
			break;
		case '}':
			scol = pcol + rmargin() + hstep / 2 - scols;
			break;
		case CTRLKEY('l'):
			break;
		case 'I':
			invert = count || !invert ? 255 - (getcount(48) & 0xff) : 0;
			loadpage(num);
	zoom_page(pcols ? zoom * scols / pcols : zoom); /* autofit width */
			break;
		default:	/* no need to redraw */
			continue;
		}
		srow = MAX(prow - srows + MARGIN, MIN(prow + prows - MARGIN, srow));
		scol = MAX(pcol - scols + MARGIN, MIN(pcol + pcols - MARGIN, scol));
		draw();
	}
	term_cleanup();
}

static char *usage =
	"usage: fbpdf [-r rotation] [-z zoom x10] [-p page] filename";

int main(int argc, char *argv[])
{
	int i = 1;
	if (argc < 2) {
		puts(usage);
		return 1;
	}
	strcpy(filename, argv[argc - 1]);
	doc = doc_open(filename);
	load_state();
	if (num < 1) num = 1;
	if (num > doc_pages(doc)) num = doc_pages(doc);

	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "fbpdf: cannot open <%s>\n", filename);
		return 1;
	}
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		switch (argv[i][1]) {
		case 'r':
			rotate = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		case 'z':
			zoom = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]) * 10;
			break;
		case 'p':
			num = atoi(argv[i][2] ? argv[i] + 2 : argv[++i]);
			break;
		}
	}
	printinfo();
	if (fb_init(getenv("FBDEV")))
		return 1;
	srows = fb_rows();
	scols = fb_cols();
	bpp = FBM_BPP(fb_mode());
	mainloop();
	fb_free();
	free(pbuf);
	if (doc)
		doc_close(doc);
	return 0;
}
