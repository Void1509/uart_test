#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

#include <glib.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/types.h>

#define RBUF_MAX 		256
#define XCORE_HEADER 0xAA
#define XCORE_HEADER_ANSWER 0x55
#define XCORE_TERMINATOR_1b 0xEB // first byte
#define XCORE_TERMINATOR_2b 0xAA // second byte

struct CmdLine_s {
	int argc;
	char **argv;
};

struct ReadBuf_s {
	uint16_t inx;
	uint8_t buf[RBUF_MAX];
}rBuf = {.inx = 0};

static GIOChannel *ttyFd = NULL;
// cmd table
static const uint8_t cmdGetSN[] = {3, 00, 00, 00};
static const uint8_t cmdGetPN[] = {3, 00, 01, 00};
static const uint8_t cmdGetMatrixW[] = {3, 00, 02, 00};
static const uint8_t cmdGetMatrixH[] = {3, 00, 03, 00};
static const uint8_t cmdGetZoom[] = {3, 00, 0x2A, 00};
static const uint8_t cmdGetPalitra[] = {3, 00, 0x2d, 00};


// answer table
static const uint8_t ansGetSN[] = {0x0c, 00, 00, 0x33, 0x30, 0x31, 0x30, 0x30, 0x30, 0x31, 0x30, 0x30, 00};
// Name XCORE_LA
static const uint8_t ansGetPN[] = {0x17, 00, 01, 0x33, 0x4c, 0x41, 0x37, 0x31, 0x31, 0x33, 0x30, 0x30, 0x32, 0x59, 
								   0x30, 0x33, 0x35, 0x31, 0x30, 0x58, 0x45, 0x4e, 0x50, 0x58};

static const uint8_t ansGetPalitra[] = {0x4, 00, 0x2d, 0x33, 0x03};
static const uint8_t ansGetZoom[] = {0x5, 00, 0x2a, 0x33, 0x64, 00};
static const uint8_t ansGetMatrixW[] = {5, 00, 02, 0x33, 0x80, 0x01};
static const uint8_t ansGetMatrixH[] = {5, 00, 03, 0x33, 0x20, 0x01};


static void hex_dump(unsigned char *buf, unsigned short len, unsigned char col);
static uint8_t* build_packet(const uint8_t *data, uint8_t ans);

static void nop_func(GIOChannel *ch, struct ReadBuf_s *rb);
static void get_sn_func(GIOChannel *ch, struct ReadBuf_s *rb);
static void get_pn_func(GIOChannel *ch, struct ReadBuf_s *rb);
static void get_palitra_func(GIOChannel *ch, struct ReadBuf_s *rb);
static void get_zoom_func(GIOChannel *ch, struct ReadBuf_s *rb);
static void get_matrixH_func(GIOChannel *ch, struct ReadBuf_s *rb);
static void get_matrixW_func(GIOChannel *ch, struct ReadBuf_s *rb);

static struct ParsePack_s{
	const uint8_t *cmd;
	void (*cmd_func)(GIOChannel *ch, struct ReadBuf_s *rb);
}parsePack[] = { {cmdGetSN, get_sn_func}, {cmdGetPN, get_pn_func}, {cmdGetPalitra, get_palitra_func},
	{cmdGetMatrixW, get_matrixW_func}, {cmdGetMatrixH, get_matrixH_func}, {cmdGetZoom, get_zoom_func},
	{NULL, NULL}};

// service functions ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
static GIOChannel *open_tty(char *drv) {
	GError *err = NULL;

	GIOChannel *ret = g_io_channel_new_file(drv, "r+", &err);
	if (!ret) {
		fprintf(stderr, "g_io_channel_new_file error: %s\n", err->message);
		g_error_free(err);
	} else {
		g_io_channel_set_encoding(ret, NULL, NULL);
		g_io_channel_set_buffered(ret, FALSE);
		g_io_channel_set_close_on_unref(ret, TRUE);
		puts("Open OK");
		int fd = g_io_channel_unix_get_fd(ret);
		{
			struct termios tc;

			tcgetattr(fd, &tc);
//			cfmakeraw(&tc);
			tc.c_iflag &= ~(BRKINT | ICRNL | IMAXBEL | INLCR | IGNBRK | IGNCR | IXON);
			tc.c_oflag &= ~(OPOST | ONLCR);
			tc.c_cflag &= ~(CSIZE | PARENB);
			tc.c_cflag |= CS8;
			tc.c_lflag &= ~(ECHO | ICANON | ISIG | ECHOE);
			tc.c_cc[VMIN] = 1;
			tc.c_cc[VTIME] = 5;
			cfsetispeed(&tc, B115200);
			cfsetospeed(&tc, B115200);
			tcsetattr(fd, TCSANOW, &tc);
		}
	}
	return ret;
}


static uint8_t calcSC(uint8_t *bf) {
	uint16_t ui1 = 0;
	uint8_t sz;

	sz = bf[1] + 1;

	for (int i = 0; i < sz; i++)
		ui1 += bf[i];

	return((uint8_t)(ui1 & 0xff));
}
/*
 * data - data to send
 * ans - flag 0 - cmd; 1 - answer
 */
static uint8_t* build_packet(const uint8_t *data, uint8_t ans) {
	uint8_t len = data[0];
	uint8_t *ret = (uint8_t*)malloc(len + 5);
	ret[0] = (ans)?XCORE_HEADER_ANSWER:XCORE_HEADER;
	ret[1] = len + 1;
	memcpy(&ret[2], &data[1], len);
	{
		uint8_t pack_crc = 0;
		for(uint8_t i = 0; i < (len + 2); i++) {
			pack_crc += ret[i];
		}
		ret[len + 2] = pack_crc;
	}
	ret[len + 3] = XCORE_TERMINATOR_1b;
	ret[len + 4] = XCORE_TERMINATOR_2b;

	return ret;
}

static void hex_dump(unsigned char *buf, unsigned short len, unsigned char col) {
	unsigned inx = 0;
	uint8_t cl = 0;

	fprintf(stdout, "%04X : ", inx);
	while(inx < len) {
		fprintf(stdout, "%02x ", buf[inx++]);
		cl++;
		if (cl == col) {
			cl = 0;
			fprintf(stdout, "\n%04X : ", inx);
		}
	}
	fprintf(stdout, "\r\n");
}
// service functions ------------------------------------------------------------------------------
// parse functions ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
static void get_sn_func(GIOChannel *ch, struct ReadBuf_s *rb) {

	fprintf(stdout, "Get packet Get SN:");
	hex_dump(rb->buf, rb->inx, rb->inx + 1);
	uint8_t *wbuf = build_packet(ansGetSN, TRUE);
	gsize wb = 0;
	GError *err = NULL;

	GIOStatus st = g_io_channel_write_chars(ch, wbuf, (gssize)(wbuf[1] + 4), &wb, &err);
	fprintf(stdout, "Send answer:");
	hex_dump(wbuf, wb, wb+1);
	g_io_channel_flush(ch, &err);
	if (err) {
		fprintf(stdout, "Write port error: %s\n", err->message);
		g_error_free(err);
	}
	free(wbuf);	
}

static void get_pn_func(GIOChannel *ch, struct ReadBuf_s *rb) {
	fprintf(stdout, "Get packet Get PN:");
	hex_dump(rb->buf, rb->inx, rb->inx + 1);
	uint8_t *wbuf = build_packet(ansGetPN, TRUE);
	gsize wb = 0;
	GError *err = NULL;

	GIOStatus st = g_io_channel_write_chars(ch, wbuf, (gssize)(wbuf[1] + 4), &wb, &err);
	fprintf(stdout, "Send answer:");
	hex_dump(wbuf, wb, wb+1);
	g_io_channel_flush(ch, &err);
	if (err) {
		fprintf(stdout, "Write port error: %s\n", err->message);
		g_error_free(err);
	}
	free(wbuf);	
}

static void get_palitra_func(GIOChannel *ch, struct ReadBuf_s *rb) {
	fprintf(stdout, "Get packet Get Palitra:");
	hex_dump(rb->buf, rb->inx, rb->inx + 1);
	uint8_t *wbuf = build_packet(ansGetPalitra, TRUE);
	gsize wb = 0;
	GError *err = NULL;

	GIOStatus st = g_io_channel_write_chars(ch, wbuf, (gssize)(wbuf[1] + 4), &wb, &err);
	fprintf(stdout, "Send answer:");
	hex_dump(wbuf, wb, wb+1);
	g_io_channel_flush(ch, &err);
	if (err) {
		fprintf(stdout, "Write port error: %s\n", err->message);
		g_error_free(err);
	}
	free(wbuf);	
}

static void get_matrixW_func(GIOChannel *ch, struct ReadBuf_s *rb) {

	fprintf(stdout, "Get packet Get matrix W:");
	hex_dump(rb->buf, rb->inx, rb->inx + 1);
	uint8_t *wbuf = build_packet(ansGetMatrixW, TRUE);
	gsize wb = 0;
	GError *err = NULL;

	GIOStatus st = g_io_channel_write_chars(ch, wbuf, (gssize)(wbuf[1] + 4), &wb, &err);
	fprintf(stdout, "Send answer:");
	hex_dump(wbuf, wb, wb+1);
	g_io_channel_flush(ch, &err);
	if (err) {
		fprintf(stdout, "Write port error: %s\n", err->message);
		g_error_free(err);
	}
	free(wbuf);	
}

static void get_matrixH_func(GIOChannel *ch, struct ReadBuf_s *rb) {

	fprintf(stdout, "Get packet Get matrix H:");
	hex_dump(rb->buf, rb->inx, rb->inx + 1);
	uint8_t *wbuf = build_packet(ansGetMatrixH, TRUE);
	gsize wb = 0;
	GError *err = NULL;

	GIOStatus st = g_io_channel_write_chars(ch, wbuf, (gssize)(wbuf[1] + 4), &wb, &err);
	fprintf(stdout, "Send answer:");
	hex_dump(wbuf, wb, wb+1);
	g_io_channel_flush(ch, &err);
	if (err) {
		fprintf(stdout, "Write port error: %s\n", err->message);
		g_error_free(err);
	}
	free(wbuf);	
}

static void get_zoom_func(GIOChannel *ch, struct ReadBuf_s *rb) {

	fprintf(stdout, "Get packet Get zoom:");
	hex_dump(rb->buf, rb->inx, rb->inx + 1);
	uint8_t *wbuf = build_packet(ansGetZoom, TRUE);
	gsize wb = 0;
	GError *err = NULL;

	GIOStatus st = g_io_channel_write_chars(ch, wbuf, (gssize)(wbuf[1] + 4), &wb, &err);
	fprintf(stdout, "Send answer:");
	hex_dump(wbuf, wb, wb+1);
	g_io_channel_flush(ch, &err);
	if (err) {
		fprintf(stdout, "Write port error: %s\n", err->message);
		g_error_free(err);
	}
	free(wbuf);	
}

static void nop_func(GIOChannel *ch, struct ReadBuf_s *rb) {
	(void)rb;
	(void)ch;
}

static void parse_pack(GIOChannel *ch, struct ReadBuf_s *rb) {
	struct ParsePack_s *pp = parsePack;
//	printf("Recive msg:");
//	hex_dump(rb->buf, rb->inx, 16);
	while(pp->cmd) {

		if (rb->inx >= (pp->cmd[0] + 5)) {
			if (!memcmp(&rb->buf[2], &pp->cmd[1], pp->cmd[0])) {
				pp->cmd_func(ch, rb);
				break;
			}
		}

		pp++;
	}
	rb->inx = 0;
}

static gboolean parse_buf(GIOChannel *ch, struct ReadBuf_s *rb) {
	gboolean ret = FALSE;

	if (rb->buf[0] == 0xaa) {
		if (rb->inx >= 2) {
			uint8_t sz = rb->buf[1];
			if (rb->inx >= (sz + 4)) {
				uint8_t crc = calcSC(rb->buf);
				if (crc == rb->buf[sz + 1]) {
					ret = TRUE;
				} else {
					rb->inx = 0;
				}
			} 
		}
	} else {
		rb->inx = 0;
	}
	return ret;
}
// parse functions --------------------------------------------------------------------------------

static gboolean read_bytes(GIOChannel *ch, GIOCondition cnd, gpointer p) {
	if (cnd & G_IO_HUP) {
		puts("Hung up");
		return FALSE;
	}

	static int rbsw = 0;
	struct ReadBuf_s *rb = p;

	gsize rbytes = 0;
	GError *err = NULL;
	GIOStatus st;

	if (!rbsw) {
		rBuf.inx = 0;
		st = g_io_channel_read_chars(ch, rBuf.buf, 2, &rbytes, &err);
		if (st == G_IO_STATUS_NORMAL) {
			if (rbytes == 2) {
				if (rBuf.buf[0] == 0xaa) {
					rbsw = rBuf.buf[1] + 2;
					rBuf.inx = 2;
				}
			}
		}
	} else {
		st = g_io_channel_read_chars(ch, &rBuf.buf[rBuf.inx], rbsw, &rbytes, &err);
		if (st == G_IO_STATUS_NORMAL) {
			rBuf.inx += rbytes;
			if (rbytes < rbsw) {
				rbsw -= rbytes;
			} else {
				if (parse_buf(ch, rb))
					parse_pack(ch, rb);
			}
		}
	}
	return TRUE;
}

static gboolean app_activate(struct CmdLine_s *cmd) {
	puts(cmd->argv[1]);
	ttyFd = open_tty(cmd->argv[1]);
	if (!ttyFd) {
		fprintf(stderr, "Error open device: %s\n", cmd->argv[1]);
		return FALSE;
	}
	g_io_add_watch(ttyFd, G_IO_IN | G_IO_PRI | G_IO_HUP, read_bytes, &rBuf);
	return TRUE;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stdout, "Нет аргумента !\nИспользовать: %s /dev/<driver>\n", argv[0]);
		return -1;
	}

	struct CmdLine_s cmdLine = {.argc = argc, .argv = argv};

	GMainLoop *ml = g_main_loop_new(NULL, FALSE);

	if (app_activate(&cmdLine))
		g_main_loop_run(ml);

	if (ml) {
		if (ttyFd) {
			g_object_unref(ttyFd);
		}
		g_main_loop_unref(ml);
	}

	return 0;
}

