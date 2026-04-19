/*
 * airpods-ctl - AirPods control tool for FreeBSD
 * Controls ANC, reads battery, ear detection via AACP protocol over L2CAP
 *
 * Usage: airpods-ctl <command> [args]
 *   airpods-ctl anc off|on|transparency|adaptive
 *   airpods-ctl ca on|off          (conversational awareness)
 *   airpods-ctl battery
 *   airpods-ctl ear
 *   airpods-ctl info
 *   airpods-ctl raw <hex>          (send raw packet)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#define L2CAP_SOCKET_CHECKED
#include <bluetooth.h>

/* AACP PSM for AirPods control channel */
#define AACP_PSM	0x1001

/* Opcodes */
#define OP_HANDSHAKE		0x01
#define OP_BATTERY		0x04
#define OP_EAR_DETECTION	0x06
#define OP_CONTROL		0x09
#define OP_NOISE_CONTROL_STATUS	0x0D
#define OP_REQUEST_NOTIF	0x0F
#define OP_METADATA		0x1D
#define OP_SET_FEATURES		0x4D

/* Control command IDs */
#define CTL_EAR_DETECTION	0x0A
#define CTL_LISTENING_MODE	0x0D
#define CTL_ONE_BUD_ANC		0x1B
#define CTL_VOLUME_SWIPE_INT	0x23
#define CTL_VOLUME_SWIPE_MODE	0x25
#define CTL_ADAPTIVE_VOLUME	0x26
#define CTL_CONVERSATION_DETECT	0x28
#define CTL_HEARING_AID		0x2C
#define CTL_ADAPTIVE_NOISE	0x2E

/* Listening mode values */
#define ANC_OFF			0x01
#define ANC_ON			0x02
#define ANC_TRANSPARENCY	0x03
#define ANC_ADAPTIVE		0x04

/* Battery component IDs */
#define BATT_RIGHT		0x02
#define BATT_LEFT		0x04
#define BATT_CASE		0x08

/* Battery status */
#define BATT_UNKNOWN		0x00
#define BATT_CHARGING		0x01
#define BATT_DISCHARGING	0x02
#define BATT_DISCONNECTED	0x04

/* Ear detection states */
#define EAR_IN			0x00
#define EAR_OUT			0x01
#define EAR_CASE		0x02

/* Handshake packet */
static const uint8_t pkt_handshake[] = {
	0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Set specific features */
static const uint8_t pkt_set_features[] = {
	0x04, 0x00, 0x04, 0x00, 0x4d, 0x00, 0xff, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Request notifications */
static const uint8_t pkt_request_notif[] = {
	0x04, 0x00, 0x04, 0x00, 0x0f, 0x00, 0xff, 0xff,
	0xff, 0xff
};

static int
l2cap_connect(const char *addr_str)
{
	struct sockaddr_l2cap addr;
	bdaddr_t raddr;
	int fd;

	if (!bt_aton(addr_str, &raddr)) {
		struct hostent *he = bt_gethostbyname(addr_str);
		if (he == NULL) {
			fprintf(stderr, "Unknown device: %s\n", addr_str);
			return -1;
		}
		bdaddr_copy(&raddr, (bdaddr_t *)he->h_addr);
	}

	fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	/* Bind to local adapter */
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = AF_BLUETOOTH;
	/* laddr all zeros = use default adapter */

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}

	/* Connect to AirPods AACP PSM */
	bdaddr_copy(&addr.l2cap_bdaddr, &raddr);
	addr.l2cap_psm = htole16(AACP_PSM);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(fd);
		return -1;
	}

	return fd;
}

static int
do_handshake(int fd)
{
	uint8_t buf[256];
	ssize_t n;

	/* Send handshake */
	if (write(fd, pkt_handshake, sizeof(pkt_handshake)) < 0) {
		perror("write handshake");
		return -1;
	}

	/* Read handshake ACK */
	n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		perror("read handshake ack");
		return -1;
	}

	/* Verify ACK (starts with 01 00 04 00) */
	if (n < 4 || buf[0] != 0x01) {
		fprintf(stderr, "Bad handshake response\n");
		return -1;
	}

	/* Send set features */
	if (write(fd, pkt_set_features, sizeof(pkt_set_features)) < 0) {
		perror("write set_features");
		return -1;
	}

	/* Read features ACK */
	n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		perror("read features ack");
		return -1;
	}

	/* Request notifications */
	if (write(fd, pkt_request_notif, sizeof(pkt_request_notif)) < 0) {
		perror("write request_notif");
		return -1;
	}

	return 0;
}

static int
send_control(int fd, uint8_t ctl_id, uint8_t value)
{
	uint8_t pkt[] = {
		0x04, 0x00, 0x04, 0x00, 0x09, 0x00,
		ctl_id, value, 0x00, 0x00, 0x00
	};

	if (write(fd, pkt, sizeof(pkt)) < 0) {
		perror("write control");
		return -1;
	}
	return 0;
}

static void
parse_battery(const uint8_t *buf, ssize_t len)
{
	int count, i, off;

	if (len < 8) return;

	/* buf[6] = component count */
	count = buf[6];
	off = 7;

	for (i = 0; i < count && off + 4 < len; i++) {
		uint8_t comp = buf[off];
		uint8_t level = buf[off + 2];
		uint8_t status = buf[off + 3];
		const char *name, *state;

		switch (comp) {
		case BATT_RIGHT: name = "Right"; break;
		case BATT_LEFT:  name = "Left";  break;
		case BATT_CASE:  name = "Case";  break;
		default:         name = "Unknown"; break;
		}

		switch (status) {
		case BATT_CHARGING:     state = "charging"; break;
		case BATT_DISCHARGING:  state = "discharging"; break;
		case BATT_DISCONNECTED: state = "disconnected"; break;
		default:                state = "unknown"; break;
		}

		printf("  %-6s: %d%% (%s)\n", name, level, state);
		off += 5; /* component + 01 + level + status + 01 */
	}
}

static void
parse_ear_detection(const uint8_t *buf, ssize_t len)
{
	const char *states[] = {"In Ear", "Out of Ear", "In Case"};

	if (len < 8) return;

	uint8_t primary = buf[6];
	uint8_t secondary = buf[7];

	printf("  Primary:   %s\n", primary < 3 ? states[primary] : "unknown");
	printf("  Secondary: %s\n", secondary < 3 ? states[secondary] : "unknown");
}

static void
parse_metadata(const uint8_t *buf, ssize_t len)
{
	const char *fields[] = {
		"Name", "Model", "Manufacturer", "Serial",
		"Firmware", "Firmware2", "Software"
	};
	int field = 0;
	int off = 6;

	while (off < len && field < 7) {
		int slen = strnlen((const char *)buf + off, len - off);
		if (slen > 0)
			printf("  %-14s: %.*s\n", fields[field], slen, buf + off);
		off += slen + 1;
		field++;
	}
}

static void
parse_noise_control(const uint8_t *buf, ssize_t len)
{
	if (len < 7) return;

	uint8_t mode = buf[6];
	const char *name;

	switch (mode) {
	case ANC_OFF:          name = "Off"; break;
	case ANC_ON:           name = "Noise Cancellation"; break;
	case ANC_TRANSPARENCY: name = "Transparency"; break;
	case ANC_ADAPTIVE:     name = "Adaptive"; break;
	default:               name = "Unknown"; break;
	}

	printf("  Noise Control: %s\n", name);
}

static int
read_notifications(int fd, int timeout_sec, int target_opcode)
{
	uint8_t buf[512];
	ssize_t n;
	struct timeval tv;
	int found = 0;

	tv.tv_sec = timeout_sec;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	while (!found) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0) break;
		if (n < 6) continue;

		uint8_t opcode = buf[4];

		switch (opcode) {
		case OP_BATTERY:
			if (target_opcode == 0 || target_opcode == OP_BATTERY) {
				printf("Battery:\n");
				parse_battery(buf, n);
				if (target_opcode == OP_BATTERY) found = 1;
			}
			break;
		case OP_EAR_DETECTION:
			if (target_opcode == 0 || target_opcode == OP_EAR_DETECTION) {
				printf("Ear Detection:\n");
				parse_ear_detection(buf, n);
				if (target_opcode == OP_EAR_DETECTION) found = 1;
			}
			break;
		case OP_METADATA:
			if (target_opcode == 0 || target_opcode == OP_METADATA) {
				printf("Device Info:\n");
				parse_metadata(buf, n);
				if (target_opcode == OP_METADATA) found = 1;
			}
			break;
		case OP_NOISE_CONTROL_STATUS:
			if (target_opcode == 0 || target_opcode == OP_NOISE_CONTROL_STATUS) {
				parse_noise_control(buf, n);
				if (target_opcode == OP_NOISE_CONTROL_STATUS) found = 1;
			}
			break;
		default:
			if (target_opcode == 0) {
				printf("Packet opcode=0x%02x len=%zd:", opcode, n);
				for (int i = 0; i < n && i < 32; i++)
					printf(" %02x", buf[i]);
				printf("\n");
			}
			break;
		}

		/* For target_opcode == 0, read a few packets then stop */
		if (target_opcode == 0) {
			tv.tv_sec = 2;
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		}
	}

	return found ? 0 : -1;
}

static void
hexdump(const uint8_t *buf, ssize_t len)
{
	for (int i = 0; i < len; i++)
		printf("%02x ", buf[i]);
	printf("\n");
}

static int
parse_hex(const char *hex, uint8_t *out, int maxlen)
{
	int len = 0;
	while (*hex && len < maxlen) {
		while (*hex == ' ' || *hex == ':') hex++;
		if (!*hex) break;
		unsigned int byte;
		if (sscanf(hex, "%2x", &byte) != 1) break;
		out[len++] = byte;
		hex += 2;
	}
	return len;
}

static void
usage(void)
{
	fprintf(stderr,
	    "Usage: airpods-ctl [-d addr] <command> [args]\n"
	    "\n"
	    "Commands:\n"
	    "  anc off|on|transparency|adaptive\n"
	    "  ca on|off              Conversational Awareness\n"
	    "  battery                Show battery levels\n"
	    "  ear                    Show ear detection state\n"
	    "  info                   Show device info\n"
	    "  listen                 Listen for all notifications\n"
	    "  raw <hex bytes>        Send raw packet\n"
	    "\n"
	    "Options:\n"
	    "  -d addr    BT address or hostname (default: airpods)\n"
	);
	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *device = "airpods";
	int ch, fd;

	while ((ch = getopt(argc, argv, "d:h")) != -1) {
		switch (ch) {
		case 'd':
			device = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage();

	const char *cmd = argv[0];

	/* Connect */
	printf("Connecting to %s (PSM 0x%04x)...\n", device, AACP_PSM);
	fd = l2cap_connect(device);
	if (fd < 0) {
		fprintf(stderr, "Connection failed. Is the device connected via bluecontrol?\n");
		return 1;
	}
	printf("Connected.\n");

	/* Handshake */
	if (do_handshake(fd) < 0) {
		fprintf(stderr, "Handshake failed.\n");
		close(fd);
		return 1;
	}
	printf("Handshake OK.\n");

	/* Process command */
	if (strcmp(cmd, "anc") == 0) {
		if (argc < 2) {
			fprintf(stderr, "Usage: airpods-ctl anc off|on|transparency|adaptive\n");
			close(fd);
			return 1;
		}
		uint8_t mode;
		if (strcmp(argv[1], "off") == 0)
			mode = ANC_OFF;
		else if (strcmp(argv[1], "on") == 0)
			mode = ANC_ON;
		else if (strcmp(argv[1], "transparency") == 0)
			mode = ANC_TRANSPARENCY;
		else if (strcmp(argv[1], "adaptive") == 0)
			mode = ANC_ADAPTIVE;
		else {
			fprintf(stderr, "Unknown mode: %s\n", argv[1]);
			close(fd);
			return 1;
		}
		send_control(fd, CTL_LISTENING_MODE, mode);
		printf("ANC → %s\n", argv[1]);

	} else if (strcmp(cmd, "ca") == 0) {
		if (argc < 2) {
			fprintf(stderr, "Usage: airpods-ctl ca on|off\n");
			close(fd);
			return 1;
		}
		uint8_t val = strcmp(argv[1], "on") == 0 ? 0x01 : 0x02;
		send_control(fd, CTL_CONVERSATION_DETECT, val);
		printf("Conversational Awareness → %s\n", argv[1]);

	} else if (strcmp(cmd, "battery") == 0) {
		printf("Waiting for battery status...\n");
		read_notifications(fd, 5, OP_BATTERY);

	} else if (strcmp(cmd, "ear") == 0) {
		printf("Waiting for ear detection...\n");
		read_notifications(fd, 5, OP_EAR_DETECTION);

	} else if (strcmp(cmd, "info") == 0) {
		printf("Waiting for device info...\n");
		read_notifications(fd, 5, OP_METADATA);

	} else if (strcmp(cmd, "listen") == 0) {
		printf("Listening for notifications (Ctrl+C to stop)...\n");
		read_notifications(fd, 0, 0);

	} else if (strcmp(cmd, "raw") == 0) {
		if (argc < 2) {
			fprintf(stderr, "Usage: airpods-ctl raw <hex bytes>\n");
			close(fd);
			return 1;
		}
		uint8_t pkt[256];
		int pktlen = parse_hex(argv[1], pkt, sizeof(pkt));
		if (pktlen == 0) {
			fprintf(stderr, "Invalid hex\n");
			close(fd);
			return 1;
		}
		printf("Sending %d bytes: ", pktlen);
		hexdump(pkt, pktlen);
		write(fd, pkt, pktlen);
		printf("Listening for response...\n");
		read_notifications(fd, 5, 0);

	} else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		usage();
	}

	close(fd);
	return 0;
}
