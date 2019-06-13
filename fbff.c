/*
 * fbff - a small ffmpeg-based framebuffer/oss media player
 *
 * Copyright (C) 2009-2015 Ali Gholami Rudi
 *	Port to Microwindows and miniaudio Copyright (c) 2019 Greg haerr
 *
 * This program is released under the Modified BSD license.
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MA_NO_DECODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ffs.h"
#include "draw.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#define PROGNAME	"fbff"	/* program name*/
#define WAITMS		20		/* msecs to wait for events*/

static int eof;
static int paused;
static int exited;
static int domark;
static int dojump;
static int loop;
static int arg;
static char filename[32];

static float wzoom = 1;
static float hzoom = 1;
static int magnify = 1;
static int jump = 0;
static int fullscreen = 0;
static int frame = APPFRAME;/* application window frame type*/
static int flipy = 0;
static int video = 1;		/* video stream; 0:none, 1:auto, >1:idx */
static int audio = 1;		/* audio stream; 0:none, 1:auto, >1:idx */
static int posx, posy;		/* video position */
static int vidw, vidh;		/* specified video width/height*/
static int rjust, bjust;	/* justify video to screen right/bottom */

static struct ffs *affs;	/* audio ffmpeg stream */
static struct ffs *vffs;	/* video ffmpeg stream */
static int vnum;			/* decoded video frame count */
static long mark[256];		/* marks */

static int sync_diff;		/* audio/video frame position diff */
static int sync_period;		/* sync after every this many number of frames */
static int sync_since;		/* frames since th last sync */
static int sync_cnt = 32;	/* synchronization steps */
static int sync_cur;		/* synchronization steps left */
static int sync_first;		/* first frame to record sync_diff */

static void stroll(void)
{
	usleep(10000);
}

static void draw_row(int rb, int cb, void *img, int cn)
{
	if (rb < 0 || rb >= fb_rows())
		return;
	if (flipy)
		rb = fb_rows() - 1 - rb;
	if (cb < 0) {
		cn = -cb < cn ? cn + cb : 0;
		img += -cb;
		cb = 0;
	}
	if (cb + cn >= fb_cols())
		cn = cb < fb_cols() ? fb_cols() - cb : 0;
	fb_set(rb, cb, img, cn);
}

static void draw_frame(void *img, int linelen)
{
	int w, h, rn, cn, cb, rb;
	int i, r, c;
	ffs_vinfo(vffs, &w, &h);
	rn = h * hzoom;
	cn = w * wzoom;
	cb = rjust ? fb_cols() - cn * magnify + posx : posx;
	rb = bjust ? fb_rows() - rn * magnify + posy : posy;
	if (magnify == 1) {
		for (r = 0; r < rn; r++)
			draw_row(rb + r, cb, img + r * linelen, cn);
	} else {
		fbval_t *brow = malloc(cn * magnify * sizeof(fbval_t));
		for (r = 0; r < rn; r++) {
			fbval_t *row = img + r * linelen;
			for (c = 0; c < cn; c++)
				for (i = 0; i < magnify; i++)
					brow[c * magnify + i] = row[c];
			for (i = 0; i < magnify; i++)
				draw_row((rb + r) * magnify + i, cb, brow, cn * magnify);
		}
		free(brow);
	}
	fb_update();
}

/* audio buffers */
static ma_device_config audio_config;
static ma_device audio_device;
static int audio_channels;
static unsigned int audio_buf_size = 0;
static unsigned int audio_buf_index = 0;

static void audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
	int cnt, audio_size;
	char *stream = pOutput;
	int len = frameCount * audio_channels * sizeof(short);
	static unsigned char audio_buf[8192];

	while(len > 0) {
		if(audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = ffs_adec(affs, audio_buf, sizeof(audio_buf));

			if(audio_size < 0) {
				//eof++;		/* uncomment to end playback when audio ends*/
//printf("audio ended\n");
				/* If error, output silence */
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			} else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		cnt = MIN(audio_buf_size - audio_buf_index, len);
		memcpy(stream, audio_buf + audio_buf_index, cnt);
		len -= cnt;
		stream += cnt;
		audio_buf_index += cnt;
	}
}

static int aud_open(void)
{
	int rate, bps;
	ffs_ainfo(affs, &rate, &bps, &audio_channels);
    audio_config = ma_device_config_init(ma_device_type_playback);
    audio_config.playback.format   = ma_format_s16;		/* bps always = 16*/
    audio_config.playback.channels = audio_channels;
    audio_config.sampleRate        = rate;
    audio_config.dataCallback      = audio_callback;
    audio_config.pUserData         = 0;
    if (ma_device_init(NULL, &audio_config, &audio_device) != MA_SUCCESS) {
		audio = 0;
		return 1;
	}
	return 0;
}

static void aud_close(void)
{
    ma_device_uninit(&audio_device);
}

static void a_doreset(int pause)
{
	audio_buf_size = audio_buf_index = 0;
}

/* subtitle handling */
#define SUBSCNT		2048		/* number of subtitles */
#define SUBSLEN		80		/* maximum subtitle length */

static char *sub_path;			/* subtitles file */
static char sub_text[SUBSCNT][SUBSLEN];	/* subtitle text */
static long sub_beg[SUBSCNT];		/* printing position */
static long sub_end[SUBSCNT];		/* hiding position */
static int sub_n;			/* subtitle count */
static int sub_last;			/* last printed subtitle */

static void sub_read(void)
{
	struct ffs *sffs = ffs_alloc(sub_path, FFS_SUBTS);
	if (!sffs)
		return;
	while (sub_n < SUBSCNT && !ffs_sdec(sffs, &sub_text[sub_n][0], SUBSLEN,
			&sub_beg[sub_n], &sub_end[sub_n])) {
		sub_n++;
	}
	ffs_free(sffs);
}

static void sub_print(void)
{
	struct ffs *ffs = video ? vffs : affs;
	int l = 0;
	int h = sub_n;
	long pos = ffs_pos(ffs);
	while (l < h) {
		int m = (l + h) >> 1;
		if (pos >= sub_beg[m] && pos <= sub_end[m]) {
			if (sub_last != m)
				printf("\r\33[K%s", sub_text[m]);
			sub_last = m;
			fflush(stdout);
			return;
		}
		if (pos < sub_beg[m])
			h = m;
		else
			l = m + 1;
	}
	if (sub_last >= 0) {
		printf("\r\33[K");
		fflush(stdout);
		sub_last = -1;
	}
}

/* fbff commands */

static void cmdjmp(int n, int rel)
{
	struct ffs *ffs = video ? vffs : affs;
	long pos = (rel ? ffs_pos(ffs) : 0) + n * 1000;
	a_doreset(0);
	sync_first = 1;				/* sync audio and video together after jump*/
	vnum = 0;
	if (pos < 0)
		pos = 0;
	if (!rel)
		mark['\''] = ffs_pos(ffs);
	if (audio)
		ffs_seek(affs, affs, pos);
	if (video)
		ffs_seek(vffs, ffs, pos);
}

static void cmdinfo(void)
{
	struct ffs *ffs = video ? vffs : affs;
	long pos = ffs_pos(ffs);
	long percent = ffs_duration(ffs) ? pos * 10 / (ffs_duration(ffs) / 100) : 0;
	printf("\r\33[K%c %3ld.%01ld%%  %3ld:%02ld.%01ld  (AV:%4d)     [%s] \r",
		paused ? (audio == 0 ? '*' : ' ') : '>',
		percent / 10, percent % 10,
		pos / 60000, (pos % 60000) / 1000, (pos % 1000) / 100,
		video && audio ? ffs_avdiff(vffs, affs) : 0,
		filename);
	fflush(stdout);
}

static int cmdarg(int def)
{
	int n = arg;
	arg = 0;
	return n ? n : def;
}

static void cmdexec(void)
{
	int c;
	while ((c = readkey(WAITMS)) != 0) {
		if (domark) {
			domark = 0;
			mark[c] = ffs_pos(video ? vffs : affs);
			continue;
		}
		if (dojump) {
			dojump = 0;
			if (mark[c] > 0)
				cmdjmp(mark[c] / 1000, 0);
			continue;
		}
		switch (c) {
		case -1:
		case 'q':
			exited = 1;
			break;
		case 'l':
			cmdjmp(cmdarg(1) * 10, 1);
			break;
		case 'h':
			cmdjmp(-cmdarg(1) * 10, 1);
			break;
		case 'j':
			cmdjmp(cmdarg(1) * 60, 1);
			break;
		case 'k':
			cmdjmp(-cmdarg(1) * 60, 1);
			break;
		case 'J':
			cmdjmp(cmdarg(1) * 600, 1);
			break;
		case 'K':
			cmdjmp(-cmdarg(1) * 600, 1);
			break;
		case 'G':
			cmdjmp(cmdarg(0) * 60, 0);
			break;
		case '%':
			cmdjmp(cmdarg(0) * ffs_duration(vffs ? vffs : affs) / 100000, 0);
			break;
		case 'm':
			domark = 1;
			break;
		case '\'':
			dojump = 1;
			break;
		case 'i':
			cmdinfo();
			break;
		case 'L':
			loop = !loop;
			if (!paused && !loop) break;
			/* fall through if paused to unpause*/
		case ' ':
		case 'p':
			paused = !paused;
			if (paused)
					ma_device_stop(&audio_device);
			else ma_device_start(&audio_device);
			sync_cur = sync_cnt;
			break;
		case '-':
			sync_diff = -cmdarg(0);
			break;
		case '+':
			sync_diff = cmdarg(0);
			break;
		case 'a':
			sync_diff = ffs_avdiff(vffs, affs);
			break;
		case 'c':
			sync_cnt = cmdarg(0);
			break;
		case 's':
			sync_cur = cmdarg(sync_cnt);
			break;
		case 27:
			arg = 0;
			break;
		default:
			if (isdigit(c))
				arg = arg * 10 + c - '0';
		}
	}
}

/* return nonzero if one more video frame can be decoded */
static int vsync(void)
{
	if (sync_period && sync_since++ >= sync_period) {
		sync_cur = sync_cnt;
		sync_since = 0;
	}
	if (sync_first) {
		sync_cur = 0;
		if (sync_first < vnum) {
			sync_first = 0;
			sync_diff = ffs_avdiff(vffs, affs);
		}
	}
	if (sync_cur > 0) {
		sync_cur--;
		return ffs_avdiff(vffs, affs) >= sync_diff;
	}
	ffs_wait(vffs);
	return 1;
}

static void mainloop(void)
{
	eof = 0;
	while (eof < audio + video) {
		cmdexec();
		if (exited)
			break;
		if (paused) {
			a_doreset(1);
			continue;
		}
		if (video && (/*!audio || eof ||*/ vsync())) {
			int ignore = jump && (vnum % (jump + 1));
			void *buf;
			int ret = ffs_vdec(vffs, ignore ? NULL : &buf);
			vnum++;
			if (ret < 0)
				eof++;
			if (ret > 0)
				draw_frame((void *) buf, ret);
			sub_print();
		} else {
			stroll();
		}
	}
}

static char *usage = "usage: " PROGNAME " [options] file\n"
	"\noptions:\n"
	"  -z n     zoom the video\n"
	"  -m n     magnify the video by duplicating pixels\n"
	"  -j n     jump every n video frames; for slow machines\n"
	"  -f       full screen (stretched)\n"
	"  -F       fit to screen (maintain aspect ratio)\n"
	"  -v n     select video stream; '-' disables video\n"
	"  -a n     select audio stream; '-' disables audio\n"
	"  -s       always synchronize (-sx for every x frames)\n"
	"  -u       record A/V delay after the first few frames\n"
	"  -t path  subtitles file\n"
	"  -x n     horizontal video position\n"
	"  -y n     vertical video position\n"
	"  -w n     video width\n"
	"  -y n     video height\n"
	"  -d       flip video upside down\n"
	"  -r       adjust the video to the right of the screen\n"
	"  -b       adjust the video to the bottom of the screen\n\n";

static void read_args(int argc, char *argv[])
{
	int i = 1;
	while (i < argc) {
		char *c = argv[i];
		if (c[0] != '-')
			break;
		if (c[1] == 'm')
			magnify = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'z')
			hzoom = wzoom = c[2] ? atof(c + 2) : atof(argv[++i]);
		if (c[1] == 'j')
			jump = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'f')
			{ fullscreen = 1; frame = NOFRAME; }
		if (c[1] == 'F')
			{ fullscreen = 2; frame = BORDER; }
		if (c[1] == 's')
			sync_period = c[2] ? atoi(c + 2) : 1;
		if (c[1] == 't')
			sub_path = c[2] ? c + 2 : argv[++i];
		if (c[1] == 'h')
			printf("%s", usage);
		if (c[1] == 'x')
			posx = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'y')
			posy = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'w')
			vidw = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'h')
			vidh = c[2] ? atoi(c + 2) : atoi(argv[++i]);
		if (c[1] == 'd')
			flipy = 1;
		if (c[1] == 'r')
			rjust = 1;
		if (c[1] == 'b')
			bjust = 1;
		if (c[1] == 'u')
			sync_first = 32;
		if (c[1] == 'v') {
			char *arg = c[2] ? c + 2 : argv[++i];
			video = arg[0] == '-' ? 0 : atoi(arg) + 2;
		}
		if (c[1] == 'a') {
			char *arg = c[2] ? c + 2 : argv[++i];
			audio = arg[0] == '-' ? 0 : atoi(arg) + 2;
		}
		i++;
	}
}

int main(int argc, char *argv[])
{
	char *path = argv[argc - 1];
	if (argc < 2) {
		printf("usage: %s [-u -s60 ...] file\n", PROGNAME);
		return 1;
	}
	read_args(argc, argv);
	ffs_globinit();
	snprintf(filename, sizeof(filename), "%s", path);
	if (video && !(vffs = ffs_alloc(path, FFS_VIDEO | (video - 1)))) {
		fprintf(stderr, "Can't open %s\n", filename);
		video = 0;
	}
	if (audio && !(affs = ffs_alloc(path, FFS_AUDIO | (audio - 1))))
		audio = 0;
	if (!video && !audio)
		return 1;
	if (sub_path)
		sub_read();
	if (audio) {
		ffs_aconf(affs);
		if (aud_open()) {
			fprintf(stderr, "%s: Can't open audio subsystem\n", PROGNAME);
			return 1;
		}
	}
	if (video) {
		int w, h;
		ffs_vinfo(vffs, &w, &h);
		if (magnify != 1 && sizeof(fbval_t) != FBM_BPP(fb_mode()))
			fprintf(stderr, "fbff: fbval_t does not match\n");
		if (fullscreen) {
			if (fb_init())					/* sets fb_rows/cols*/
				return 1;
			vidw = fb_cols();
			vidh = fb_rows();
			if (fullscreen == 2) {
				/* fit-to-screen maintaint aspect ratio*/
				float wi = w;
				float hi = h;
				float ri = wi / hi;
				float ws = vidw;
				float hs = vidh;
				float rs = ws / hs;
				if (rs > ri) {
					/* use full screen height, shortened width*/
					hzoom = wzoom = hs/hi;
					vidw = wi * wzoom;
				} else {
					/* use full screen width, shortened height*/
					wzoom = hzoom = ws/wi;
					vidh = hi * hzoom;
				}
//printf("vid %d,%d w,h %f,%f screen %f,%f zoom %f, %f\n", vidw, vidh, wi, hi, ws, hs, wzoom, hzoom);
			} else {
				/* fullscreen stretch*/
				wzoom = (float) vidw / w / magnify;
				hzoom = (float) vidh / h / magnify;
			}
			if (fb_open(PROGNAME, vidw, vidh, frame))				/* use fullscreen*/
				return 1;
		} else if (vidw || vidh) {
			if (!vidw) vidw = w;
			if (!vidh) vidh = h;
			if (fb_init() || fb_open(PROGNAME, vidw, vidh, frame))	/* set specified window size and zoom*/
				return 1;
			wzoom = (float) fb_cols() / w / magnify;
			hzoom = (float) fb_rows() / h / magnify;
		} else {
			if (fb_init() || fb_open(PROGNAME, w, h, frame))		/* create window size of video*/
				return 1;
		}
		ffs_vconf(vffs, wzoom, hzoom, fb_mode());
	}

	term_setup();
    ma_device_start(&audio_device);
	do {
		mainloop();

		if (audio && !paused && !loop)
			ma_device_stop(&audio_device);
		cmdjmp(0, 0);
		if (!loop) paused = 1;
	} while(!exited);
	term_cleanup();

	if (video) {
		fb_free();
		ffs_free(vffs);
	}
	if (audio) {
		aud_close();
		ffs_free(affs);
	}
	return 0;
}
