/*
 * systray_multi_monitor.c – one tray‑icon per modifier (JWM‑safe)
 *
 *  • Watches boolean sysfs attributes with **inotify + select()**
 *  • Separate background colours for active (1) / inactive (0)
 *  • Docks icons in **reverse CLI order** so visual order matches argument list
 *
 * Argument format (repeat for each attribute):
 *     PATH:LABEL:FG:BG1:BG0
 *       PATH  – sysfs file that contains "0" or "1"
 *       LABEL – up to 7 ASCII chars to display when active
 *       FG    – foreground text colour  (0xRRGGBB)
 *       BG1   – background when value == 1 (active)
 *       BG0   – background when value == 0 (inactive)
 *
 * Build:
 *     gcc -O2 -Wall systray_multi_monitor.c -o systray_multi_monitor -lX11
 *
 * Example:
 *     ./systray_multi_monitor \
 *         /sys/.../ctrl:CTL:0xFFFFFF:0x770000:0x303030 \
 *         /sys/.../alt :ALT:0x000000:0xFFFF00:0x303030 \
 *         /sys/.../sym :SYM:0xFFFFFF:0x00AA00:0x303030
 *   → JWM panel order: CTRL | ALT | SYM (left‑to‑right)
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <unistd.h>

#define ICON_SZ 24 /* JWM tray slot size */
#define BUF_LEN (sizeof(struct inotify_event) + 256)

typedef struct {
	char path[256];
	char label[8];
	unsigned long fg;
	unsigned long bg_active;
	unsigned long bg_inactive;
	int state;  /* cached 0/1 */
	int wd;	    /* inotify watch‑descriptor */
	Window win; /* X11 dock window */
} Attr;

/* ─────────────────────────── Globals ─────────────────────────── */
static Display *dpy;
static int scr;
static Atom a_xembed, a_opcode;

static Attr *attrs;
static int n_attr;
static int ino_fd;

/* ────────────────────────── Helpers ──────────────────────────── */
static unsigned long parse_color(const char *s, const char *def) {
	return strtoul(s ? s : def, NULL, 0);
}

static int read_bool(const char *p) {
	char b[8] = {0};
	int fd = open(p, O_RDONLY);
	if (fd < 0)
		return -1;
	int n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 ? atoi(b) : -1;
}

static void x_flush(void) { XSync(dpy, False); }

static void draw_icon(const Attr *a) {
	GC gc = XCreateGC(dpy, a->win, 0, NULL);

	unsigned long bg = a->state ? a->bg_active : a->bg_inactive;
	XSetForeground(dpy, gc, bg);
	XFillRectangle(dpy, a->win, gc, 0, 0, ICON_SZ, ICON_SZ);

	if (a->state) {
		XSetForeground(dpy, gc, a->fg);
		XDrawString(dpy, a->win, gc, 3, ICON_SZ - 8, a->label, (int)strlen(a->label));
	}
	XFreeGC(dpy, gc);
	x_flush();
}

static void dock_to_tray(Window w) {
	char sel[64];
	snprintf(sel, sizeof sel, "_NET_SYSTEM_TRAY_S%d", scr);

	Atom sel_atom = XInternAtom(dpy, sel, False);
	Window tray = XGetSelectionOwner(dpy, sel_atom);
	if (!tray)
		return; /* no system tray present */

	XClientMessageEvent ev = {0};
	ev.type = ClientMessage;
	ev.window = tray;
	ev.message_type = a_opcode;
	ev.format = 32;
	ev.data.l[0] = CurrentTime;
	ev.data.l[1] = 0; /* SYSTEM_TRAY_REQUEST_DOCK */
	ev.data.l[2] = w;

	XSendEvent(dpy, tray, False, NoEventMask, (XEvent *)&ev);
}

static void create_window(Attr *a) {
	a->win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, ICON_SZ, ICON_SZ, 0,
				     BlackPixel(dpy, scr), BlackPixel(dpy, scr));

	long info[2] = {0, 0}; /* XEmbed version, flags */
	XChangeProperty(dpy, a->win, a_xembed, a_xembed, 32, PropModeReplace, (unsigned char *)info,
			2);

	XSelectInput(dpy, a->win, ExposureMask);
	XMapWindow(dpy, a->win);
	dock_to_tray(a->win);
}

/* ──────────────────────────── main ───────────────────────────── */
int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s PATH:LABEL:FG:BG1:BG0 [...]\n", argv[0]);
		return 1;
	}

	/* -------- argument parsing -------- */
	n_attr = argc - 1;
	attrs = calloc(n_attr, sizeof *attrs);

	for (int i = 0; i < n_attr; ++i) {
		char *rest = argv[i + 1];
		char *tok;

		tok = strsep(&rest, ":");
		strncpy(attrs[i].path, tok, sizeof attrs[i].path - 1);

		tok = strsep(&rest, ":");
		strncpy(attrs[i].label, tok ? tok : "", sizeof attrs[i].label - 1);

		attrs[i].fg = parse_color(strsep(&rest, ":"), "0x000000");
		attrs[i].bg_active = parse_color(strsep(&rest, ":"), "0xFFFFFF");
		attrs[i].bg_inactive = parse_color(strsep(&rest, ":"), "0x303030");

		attrs[i].state = read_bool(attrs[i].path);
	}

	/* -------- X11 init -------- */
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		perror("XOpenDisplay");
		return 1;
	}
	scr = DefaultScreen(dpy);
	a_xembed = XInternAtom(dpy, "_XEMBED_INFO", False);
	a_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);

	/* -------- create & dock windows in **reverse** CLI order -------- */
	for (int i = n_attr - 1; i >= 0; --i) {
		create_window(&attrs[i]);
		draw_icon(&attrs[i]);
	}

	/* -------- inotify init -------- */
	ino_fd = inotify_init1(IN_NONBLOCK);
	if (ino_fd < 0) {
		perror("inotify_init1");
		return 1;
	}

	for (int i = 0; i < n_attr; ++i) {
		attrs[i].wd = inotify_add_watch(ino_fd, attrs[i].path,
						IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE);
		if (attrs[i].wd < 0)
			fprintf(stderr, "warning: cannot watch %s (%s)\n", attrs[i].path,
				strerror(errno));
	}

	/* -------- main event loop -------- */
	int xfd = ConnectionNumber(dpy);
	char buf[BUF_LEN];

	for (;;) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(xfd, &rfds);
		FD_SET(ino_fd, &rfds);
		int maxfd = (xfd > ino_fd ? xfd : ino_fd);

		if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			break;
		}

		/* -------------------- X11 events -------------------- */
		if (FD_ISSET(xfd, &rfds)) {
			while (XPending(dpy)) {
				XEvent ev;
				XNextEvent(dpy, &ev);

				if (ev.type == Expose) {
					for (int i = 0; i < n_attr; ++i) {
						if (ev.xexpose.window == attrs[i].win) {
							draw_icon(&attrs[i]);
							break;
						}
					}
				}
			}
		}

		/* ------------------ inotify events ------------------ */
		if (FD_ISSET(ino_fd, &rfds)) {
			int len = read(ino_fd, buf, sizeof buf);
			if (len > 0) {
				char *ptr = buf;
				while (ptr < buf + len) {
					struct inotify_event *ie = (struct inotify_event *)ptr;

					for (int i = 0; i < n_attr; ++i) {
						if (attrs[i].wd == ie->wd) {
							int v = read_bool(attrs[i].path);
							if (v >= 0 && v != attrs[i].state) {
								attrs[i].state = v;
								draw_icon(&attrs[i]);
							}
							break;
						}
					}
					ptr += sizeof(struct inotify_event) + ie->len;
				}
			}
		}
	}

	return 0;
}
