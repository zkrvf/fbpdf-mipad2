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
static int evfd_pwr = -1;
static int power_down = 0;
static long long power_tdown_ms = 0;

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
	if (evfd_pwr == -1)
		evfd_pwr = open("/dev/input/event10", O_RDONLY | O_NONBLOCK);
}

static int map_evdev_event(const struct input_event *ev)
{
	if (ev->type != EV_KEY)
		return 0;
	/* act only on press/release; ignore repeats */
	if (ev->value != 1 && ev->value != 0)
		return 0;

	if (ev->code == KEY_VOLUMEUP && ev->value == 1)
		return CTRLKEY('b');
	if (ev->code == KEY_VOLUMEDOWN && ev->value == 1)
		return CTRLKEY('f');

	if (ev->code == KEY_POWER) {
		if (ev->value == 1) {
			power_down = 1;
			power_tdown_ms = now_ms();
		} else if (ev->value == 0 && power_down) {
			long long dt = now_ms() - power_tdown_ms;
			power_down = 0;
			if (dt >= 700)
				return 'q';
			/* short press ignored */
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

	if (evfd_vol != -1) {
		fds[nfds].fd = evfd_vol;
		fds[nfds].events = POLLIN;
		nfds++;
	}
	if (evfd_pwr != -1) {
		fds[nfds].fd = evfd_pwr;
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

		/* evdev first */
		for (int i = 1; i < nfds; i++) {
			if (fds[i].revents & POLLIN) {
				struct input_event ev;
				ssize_t n = read(fds[i].fd, &ev, sizeof(ev));
				if (n == (ssize_t)sizeof(ev)) {
					int c = map_evdev_event(&ev);
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
