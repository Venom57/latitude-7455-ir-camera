// SPDX-License-Identifier: GPL-2.0
/*
 * ir-relay: make the Latitude 7455's HM1092 IR camera an ordinary webcam.
 *
 * Feeds a v4l2loopback device (default /dev/video43, "IR Camera") from the
 * CAMSS capture node, but only while some application is streaming from the
 * loopback device (v4l2loopback's client-usage event), so the sensor and its
 * IR emitter are off otherwise.
 *
 *  - Input: Y10P 560x360 (MIPI packed 10-bit grey) from the node that
 *    ir-relay-setup.sh prepares and prints.
 *  - Only emitter-lit frames are passed on: the driver pulses the emitter two
 *    frames on, two off, so a frame is dropped when its mean is close to the
 *    recent darkest. If all recent frames are alike (emitter steady or off),
 *    everything is passed on.
 *  - Black level removed, then a slowly adapting gain so the 99th percentile
 *    lands near 230.
 *  - Output: YUYV 560x360 (grey in Y, 128 in U/V), the most widely accepted
 *    webcam format.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define W		560
#define H		360
#define IN_STRIDE_MIN	(W * 5 / 4)
#define NBUF		4
#define BLACK10		32	/* sensor black level, 10-bit (about 8 in 8-bit) */
#define TARGET		230	/* where the 99th percentile should land */
#define HIST		8	/* frames of history for the lit/dark decision */
#define LIT_MARGIN	2.0	/* 8-bit mean above the recent darkest = lit */

/* v4l2loopback private event (v4l2loopback.c) */
#define V4L2_EVENT_PRI_CLIENT_USAGE (V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1)

static const char *loop_path = "/dev/video43";
static const char *setup_cmd = "/usr/lib/ir-relay/ir-relay-setup.sh";
static volatile sig_atomic_t quit;

static void on_signal(int sig) { (void)sig; quit = 1; }

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

/* ---- output (v4l2loopback) ---- */

static uint8_t out[W * H * 2];

static int loop_open(void)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct v4l2_event_subscription sub = { .type = V4L2_EVENT_PRI_CLIENT_USAGE };
	int fd = open(loop_path, O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", loop_path, strerror(errno));
		return -1;
	}
	f.fmt.pix.width = W;
	f.fmt.pix.height = H;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = W * 2;
	f.fmt.pix.sizeimage = W * H * 2;
	f.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) {
		fprintf(stderr, "loopback S_FMT: %s\n", strerror(errno));
		goto fail;
	}
	if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		fprintf(stderr, "loopback SUBSCRIBE_EVENT: %s (v4l2loopback too old?)\n",
			strerror(errno));
		goto fail;
	}
	/* One black frame so the capture side is announced (exclusive_caps). */
	for (int i = 0; i < W * H; i++) {
		out[2 * i] = 16;
		out[2 * i + 1] = 128;
	}
	if (write(fd, out, sizeof(out)) < 0) {
		fprintf(stderr, "loopback write: %s\n", strerror(errno));
		goto fail;
	}
	return fd;
fail:
	close(fd);
	return -1;
}

/* Drain events; returns the latest client count, or -1 if there was none. */
static int loop_events(int fd)
{
	struct v4l2_event ev;
	int count = -1;

	while (xioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
		if (ev.type == V4L2_EVENT_PRI_CLIENT_USAGE)
			count = *(uint32_t *)ev.u.data;
		if (!ev.pending)
			break;
	}
	return count;
}

/* ---- input (CAMSS) ---- */

struct cam {
	int fd;
	unsigned int stride;
	void *buf[NBUF];
	size_t len[NBUF];
	int nbuf;
};

static int cam_start(struct cam *c)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct v4l2_requestbuffers rb = {
		.count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	};
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	char node[256] = "";
	FILE *p = popen(setup_cmd, "r");

	if (!p || !fgets(node, sizeof(node), p)) {
		fprintf(stderr, "%s gave no capture node\n", setup_cmd);
		if (p)
			pclose(p);
		return -1;
	}
	if (pclose(p) != 0) {
		fprintf(stderr, "%s failed\n", setup_cmd);
		return -1;
	}
	node[strcspn(node, "\n")] = 0;

	memset(c, 0, sizeof(*c));
	c->fd = open(node, O_RDWR | O_NONBLOCK);
	if (c->fd < 0) {
		fprintf(stderr, "open %s: %s\n", node, strerror(errno));
		return -1;
	}
	f.fmt.pix_mp.width = W;
	f.fmt.pix_mp.height = H;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_Y10P;
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	if (xioctl(c->fd, VIDIOC_S_FMT, &f) < 0 ||
	    f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_Y10P ||
	    f.fmt.pix_mp.width != W || f.fmt.pix_mp.height != H) {
		fprintf(stderr, "%s: cannot set Y10P %dx%d\n", node, W, H);
		goto fail;
	}
	c->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (c->stride < IN_STRIDE_MIN)
		c->stride = IN_STRIDE_MIN;

	if (xioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		fprintf(stderr, "REQBUFS: %s\n", strerror(errno));
		goto fail;
	}
	c->nbuf = rb.count > NBUF ? NBUF : rb.count;
	for (int i = 0; i < c->nbuf; i++) {
		struct v4l2_plane pl[1] = { 0 };
		struct v4l2_buffer b = {
			.type = type, .memory = V4L2_MEMORY_MMAP, .index = i,
			.m.planes = pl, .length = 1,
		};

		if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0)
			goto fail;
		c->len[i] = pl[0].length;
		c->buf[i] = mmap(NULL, pl[0].length, PROT_READ, MAP_SHARED,
				 c->fd, pl[0].m.mem_offset);
		if (c->buf[i] == MAP_FAILED) {
			c->buf[i] = NULL;
			goto fail;
		}
		if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0)
			goto fail;
	}
	if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "STREAMON: %s\n", strerror(errno));
		goto fail;
	}
	fprintf(stderr, "streaming from %s (stride %u)\n", node, c->stride);
	return 0;
fail:
	for (int i = 0; i < NBUF; i++)
		if (c->buf[i])
			munmap(c->buf[i], c->len[i]);
	close(c->fd);
	c->fd = -1;
	return -1;
}

static void cam_stop(struct cam *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (c->fd < 0)
		return;
	xioctl(c->fd, VIDIOC_STREAMOFF, &type);
	for (int i = 0; i < c->nbuf; i++)
		if (c->buf[i])
			munmap(c->buf[i], c->len[i]);
	close(c->fd);
	c->fd = -1;
	fprintf(stderr, "stopped\n");
}

/* ---- processing ---- */

static double means[HIST];
static int nmeans;
static double gain = 4.0;

/* Returns 1 if the frame should be passed on; fills out[] in that case. */
static int process(const uint8_t *in, unsigned int stride)
{
	static uint16_t px[W * H];
	unsigned int hist[1024] = { 0 };
	double sum = 0, dark = 1e9, bright = 0, m;
	int i, x, y, lit;

	for (y = 0; y < H; y++) {
		const uint8_t *r = in + (size_t)y * stride;

		for (x = 0; x < W; x += 4, r += 5) {
			for (i = 0; i < 4; i++) {
				uint16_t v = r[i] << 2 | ((r[4] >> (2 * i)) & 3);

				px[y * W + x + i] = v;
				hist[v]++;
				sum += v;
			}
		}
	}
	m = sum / (W * H) / 4.0;	/* 8-bit scale */

	memmove(means + 1, means, sizeof(means) - sizeof(means[0]));
	means[0] = m;
	if (nmeans < HIST)
		nmeans++;
	for (i = 0; i < nmeans; i++) {
		if (means[i] < dark)
			dark = means[i];
		if (means[i] > bright)
			bright = means[i];
	}
	/* Pulsed emitter: drop the dark frames. Steady or off: keep all. */
	if (nmeans < 4)	/* no history yet: anything clearly above black */
		lit = m > BLACK10 / 4.0 + LIT_MARGIN;
	else
		lit = bright - dark < LIT_MARGIN || m > dark + LIT_MARGIN;
	if (!lit)
		return 0;

	/* 99th percentile -> TARGET, adapting slowly to avoid pumping. */
	{
		unsigned int want = W * H / 100, acc = 0;
		int p99 = 1023;

		while (p99 > 0 && acc + hist[p99] < want)
			acc += hist[p99--];
		if (p99 > BLACK10 + 8) {
			double g = (double)TARGET * 4 / (p99 - BLACK10);

			if (g < 1)
				g = 1;
			if (g > 16)
				g = 16;
			gain = 0.8 * gain + 0.2 * g;
		}
	}
	for (i = 0; i < W * H; i++) {
		int v = (int)((px[i] - BLACK10) * gain / 4);

		out[2 * i] = v < 0 ? 0 : v > 255 ? 255 : v;
		out[2 * i + 1] = 128;
	}
	return 1;
}

/* ir-relay --test OUT.pgm: 30 frames without a loopback device. */
static int self_test(const char *pgm)
{
	struct cam cam = { .fd = -1 };
	int kept = 0;

	if (cam_start(&cam) < 0)
		return 1;
	for (int n = 0; n < 30;) {
		struct pollfd pf = { .fd = cam.fd, .events = POLLIN };
		struct v4l2_plane pl[1] = { 0 };
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1,
		};
		int lit;

		if (poll(&pf, 1, 2000) <= 0) {
			fprintf(stderr, "no frames\n");
			break;
		}
		if (xioctl(cam.fd, VIDIOC_DQBUF, &b) < 0)
			continue;
		lit = process(cam.buf[b.index], cam.stride);
		kept += lit;
		printf("frame %2d mean %5.1f %s gain %.2f\n", n++, means[0],
		       lit ? "kept   " : "dropped", gain);
		xioctl(cam.fd, VIDIOC_QBUF, &b);
	}
	cam_stop(&cam);
	if (kept) {
		FILE *f = fopen(pgm, "wb");

		if (!f)
			return 1;
		fprintf(f, "P5\n%d %d\n255\n", W, H);
		for (int i = 0; i < W * H; i++)
			fputc(out[2 * i], f);
		fclose(f);
	}
	printf("%d of 30 frames kept\n", kept);
	return 0;
}

int main(int argc, char **argv)
{
	struct cam cam = { .fd = -1 };
	int lfd, clients = 0;

	if (argc > 2 && !strcmp(argv[1], "--test")) {
		setup_cmd = argc > 3 ? argv[3] : setup_cmd;
		return self_test(argv[2]);
	}

	if (argc > 1)
		loop_path = argv[1];
	if (argc > 2)
		setup_cmd = argv[2];
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	lfd = loop_open();
	if (lfd < 0)
		return 1;
	fprintf(stderr, "ready on %s\n", loop_path);

	while (!quit) {
		struct pollfd pf[2] = {
			{ .fd = lfd, .events = POLLPRI },
			{ .fd = cam.fd, .events = POLLIN },
		};
		int n = poll(pf, cam.fd >= 0 ? 2 : 1, 1000);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pf[0].revents & POLLPRI) {
			int c = loop_events(lfd);

			if (c >= 0)
				clients = c;
			if (clients && cam.fd < 0) {
				nmeans = 0;
				if (cam_start(&cam) < 0)
					sleep(1);	/* retried on the next event */
			} else if (!clients && cam.fd >= 0) {
				cam_stop(&cam);
			}
		}
		if (cam.fd >= 0 && (pf[1].revents & POLLIN)) {
			struct v4l2_plane pl[1] = { 0 };
			struct v4l2_buffer b = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				.memory = V4L2_MEMORY_MMAP,
				.m.planes = pl, .length = 1,
			};

			if (xioctl(cam.fd, VIDIOC_DQBUF, &b) == 0) {
				if (process(cam.buf[b.index], cam.stride) &&
				    write(lfd, out, sizeof(out)) < 0)
					fprintf(stderr, "loopback write: %s\n",
						strerror(errno));
				xioctl(cam.fd, VIDIOC_QBUF, &b);
			}
		}
		if (cam.fd >= 0 && (pf[1].revents & (POLLERR | POLLHUP))) {
			fprintf(stderr, "capture error; restarting\n");
			cam_stop(&cam);
			if (clients)
				cam_start(&cam);
		}
	}
	cam_stop(&cam);
	close(lfd);
	return 0;
}
