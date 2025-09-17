/*
 * bpbme280.c: Read BME280 + CPU stats once, send a single JSON bundle via ION BP.
 *
 * JSON payload (compact with short headers + short keys):
 * {
 *   "hdr":"ts,t,p,h,ct,cl",
 *   "ts":1726561234,
 *   "t":23.54,
 *   "p":1007.82,
 *   "h":45.12,
 *   "ct":51.37,
 *   "cl":0.21
 * }
 *
 * Usage:
 *   bpbme280 <destEID> <sourceEID> [-t<ttl>] [-a0x76|0x77] [-d/dev/i2c-X] [-loc<location>]
 *     -t : Bundle TTL seconds (default 300)
 *     -a : I2C address (default 0x76)
 *     -d : I2C device path (default /dev/i2c-1)
 *     -loc : Location string (optional)
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 bpbme280.c -o bpbme280 -lbp -lici -lpthread
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <bp.h>                   /* ION BP API */

/* ---------------- Run-control (like bpsource) ---------------- */
static int _running(int *newState)
{
	int state = 1;                 /* default: running */
	if (newState) { state = *newState; }
	return state;
}

static ReqAttendant *_attendant(ReqAttendant *newAttendant)
{
	static ReqAttendant *attendant = NULL;
	if (newAttendant) { attendant = newAttendant; }
	return attendant;
}

static void handleQuit(int signum)
{
	(void)signum;
	int stop = 0;
	oK(_running(&stop));
	ionPauseAttendant(_attendant(NULL));
}

/* ---------------- BME280 registers/calibration ---------------- */
#define BME280_CHIP_ID 0x60
#define REG_ID         0xD0
#define REG_RESET      0xE0
#define REG_CTRL_HUM   0xF2
#define REG_STATUS     0xF3
#define REG_CTRL_MEAS  0xF4
#define REG_CONFIG     0xF5
#define REG_PRESS_MSB  0xF7
#define REG_TEMP_MSB   0xFA
#define REG_HUM_MSB    0xFD
#define CALIB00        0x88  /* 0x88..0xA1 (26 bytes) */
#define CALIB26        0xE1  /* 0xE1..0xE7 (7 bytes)  */

typedef struct {
	/* Temperature */
	uint16_t dig_T1; int16_t dig_T2; int16_t dig_T3;
	/* Pressure */
	uint16_t dig_P1; int16_t dig_P2; int16_t dig_P3; int16_t dig_P4; int16_t dig_P5;
	int16_t dig_P6; int16_t dig_P7; int16_t dig_P8; int16_t dig_P9;
	/* Humidity */
	uint8_t  dig_H1; int16_t dig_H2; uint8_t dig_H3; int16_t dig_H4; int16_t dig_H5; int8_t dig_H6;
	/* Shared */
	int32_t t_fine;
} bme280_calib_t;

/* --------- Minimal I2C helpers (no WiringPi needed) ---------- */
static int i2c_write_reg(int fd, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	return (write(fd, buf, 2) == 2) ? 0 : -1;
}

static int i2c_read_reg(int fd, uint8_t reg, uint8_t *val)
{
	if (write(fd, &reg, 1) != 1) return -1;
	return (read(fd, val, 1) == 1) ? 0 : -1;
}

static int i2c_read_regs(int fd, uint8_t start_reg, uint8_t *buf, size_t len)
{
	if (write(fd, &start_reg, 1) != 1) return -1;
	return (read(fd, buf, len) == (ssize_t)len) ? 0 : -1;
}

/* ---------------- BME280 setup & compensation ---------------- */
static int bme280_read_calib(int fd, bme280_calib_t *c)
{
	uint8_t b1[26], b2[7];
	if (i2c_read_regs(fd, CALIB00, b1, sizeof(b1)) < 0) return -1;
	if (i2c_read_regs(fd, CALIB26, b2, sizeof(b2)) < 0) return -1;

#define U16_LE(p) ((uint16_t)((p)[0] | ((uint16_t)(p)[1] << 8)))
#define S16_LE(p) ((int16_t)((p)[0] | ((int16_t)(p)[1] << 8)))

	c->dig_T1 = U16_LE(&b1[0]);  c->dig_T2 = S16_LE(&b1[2]);  c->dig_T3 = S16_LE(&b1[4]);
	c->dig_P1 = U16_LE(&b1[6]);  c->dig_P2 = S16_LE(&b1[8]);  c->dig_P3 = S16_LE(&b1[10]);
	c->dig_P4 = S16_LE(&b1[12]); c->dig_P5 = S16_LE(&b1[14]); c->dig_P6 = S16_LE(&b1[16]);
	c->dig_P7 = S16_LE(&b1[18]); c->dig_P8 = S16_LE(&b1[20]); c->dig_P9 = S16_LE(&b1[22]);
	c->dig_H1 = b1[24];

	c->dig_H2 = S16_LE(&b2[0]);
	c->dig_H3 = b2[2];
	c->dig_H4 = (int16_t)((((int16_t)b2[3]) << 4) | (b2[4] & 0x0F));
	c->dig_H5 = (int16_t)((((int16_t)b2[5]) << 4) | (b2[4] >> 4));
	c->dig_H6 = (int8_t)b2[6];
	return 0;
}

static int bme280_configure(int fd)
{
	/* Humidity oversampling x1 */
	if (i2c_write_reg(fd, REG_CTRL_HUM, 0x01) < 0) return -1;
	/* Temp os x1, Press os x1, mode normal */
	if (i2c_write_reg(fd, REG_CTRL_MEAS, 0x27) < 0) return -1;
	/* Standby 500ms, filter off */
	if (i2c_write_reg(fd, REG_CONFIG, 0x80) < 0) return -1;
	return 0;
}

static int bme280_read_raw(int fd, int32_t *adc_T, int32_t *adc_P, int32_t *adc_H)
{
	uint8_t d[8];
	if (i2c_read_regs(fd, REG_PRESS_MSB, d, sizeof d) < 0) return -1;
	*adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
	*adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
	*adc_H = ((int32_t)d[6] << 8) | d[7];
	return 0;
}

/* Compensation as per datasheet (integer math; returns doubles) */
static double bme280_comp_T(int32_t adc_T, bme280_calib_t *c)
{
	int32_t var1 = ((((adc_T >> 3) - ((int32_t)c->dig_T1 << 1))) * ((int32_t)c->dig_T2)) >> 11;
	int32_t var2 = (((((adc_T >> 4) - ((int32_t)c->dig_T1)) * ((adc_T >> 4) - ((int32_t)c->dig_T1))) >> 12) *
	               ((int32_t)c->dig_T3)) >> 14;
	c->t_fine = var1 + var2;
	double T = (c->t_fine * 5 + 128) >> 8; /* °C * 100 */
	return T / 100.0;
}

static double bme280_comp_P(int32_t adc_P, bme280_calib_t *c)
{
	int64_t var1 = ((int64_t)c->t_fine) - 128000;
	int64_t var2 = var1 * var1 * (int64_t)c->dig_P6;
	var2 = var2 + ((var1 * (int64_t)c->dig_P5) << 17);
	var2 = var2 + (((int64_t)c->dig_P4) << 35);
	var1 = ((var1 * var1 * (int64_t)c->dig_P3) >> 8) + ((var1 * (int64_t)c->dig_P2) << 12);
	var1 = (((((int64_t)1) << 47) + var1) * ((int64_t)c->dig_P1)) >> 33;
	if (var1 == 0) return 0.0;

	int64_t p = 1048576 - adc_P;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = (((int64_t)c->dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var2 = (((int64_t)c->dig_P8) * p) >> 19;
	p = ((p + var1 + var2) >> 8) + (((int64_t)c->dig_P7) << 4);
	return ((double)p) / 25600.0; /* hPa */
}

static double bme280_comp_H(int32_t adc_H, bme280_calib_t *c)
{
	int32_t x = c->t_fine - 76800;
	int32_t v = (((((adc_H << 14) - (((int32_t)c->dig_H4) << 20) - ((int32_t)c->dig_H5 * x)) + 16384) >> 15) *
	            (((((((x * (int32_t)c->dig_H6) >> 10) * (((x * (int32_t)c->dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
	               ((int32_t)c->dig_H2 + 8192)) >> 14));
	v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->dig_H1) >> 4);
	if (v < 0) v = 0;
	if (v > 419430400) v = 419430400;
	double h = (v >> 12) / 1024.0;
	return h; /* %RH */
}

/* ---------------- CPU stats (Pi) ---------------- */
static int read_cpu_temp_c(double *outC)
{
	const char *path = "/sys/class/thermal/thermal_zone0/temp";
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	long mC = 0;
	if (fscanf(f, "%ld", &mC) != 1) { fclose(f); return -1; }
	fclose(f);
	*outC = mC / 1000.0;
	return 0;
}

static int read_cpu_load_1min(double *outLoad)
{
	FILE *f = fopen("/proc/loadavg", "r");
	if (!f) return -1;
	double l1 = 0.0;
	if (fscanf(f, "%lf", &l1) != 1) { fclose(f); return -1; }
	fclose(f);
	*outLoad = l1;
	return 0;
}

/* ------------- Compose compact JSON into buf -------------- */
/* Short header string + short keys: hdr="ts,t,p,h,ct,cl" */
static int compose_json(char *buf, size_t buflen,
                        int i2c_fd, bme280_calib_t *calib,
                        const char *location, const char *source_ipn)
{
	int32_t t_raw, p_raw, h_raw;
	if (bme280_read_raw(i2c_fd, &t_raw, &p_raw, &h_raw) < 0) return -1;

	double tC = bme280_comp_T(t_raw,  calib);
	double pH = bme280_comp_P(p_raw,  calib);
	double hR = bme280_comp_H(h_raw,  calib);

	double cpuC = 0.0, l1 = 0.0;
	(void)read_cpu_temp_c(&cpuC);      /* ignore failures (leave 0.0) */
	(void)read_cpu_load_1min(&l1);

	time_t ts = time(NULL);

	/* JSON with shorter field names, 1 decimal precision, single line */
	int n;
	if (location && location[0] != '\0') {
		n = snprintf(
			buf, buflen,
			"{\"src\":\"%s\",\"ts\":%ld,\"temp\":%.1f,\"press\":%.1f,\"humid\":%.1f,\"cpu_temp\":%.1f,\"load\":%.2f,\"loc\":\"%s\"}",
			source_ipn, (long)ts, tC, pH, hR, cpuC, l1, location
		);
	} else {
		n = snprintf(
			buf, buflen,
			"{\"src\":\"%s\",\"ts\":%ld,\"temp\":%.1f,\"press\":%.1f,\"humid\":%.1f,\"cpu_temp\":%.1f,\"load\":%.2f}",
			source_ipn, (long)ts, tC, pH, hR, cpuC, l1
		);
	}
	return (n > 0 && (size_t)n < buflen) ? n : -1;
}

/* -------------------- Main: one-shot send ------------------- */
#define DEFAULT_TTL 300
#define DEFAULT_I2C_DEV "/dev/i2c-1"

int main(int argc, char **argv)
{
	char *destEid = NULL;
	char *sourceEid = NULL;
	int ttl = DEFAULT_TTL;
	const char *i2c_dev = DEFAULT_I2C_DEV;
	int i2c_addr = 0x76;
	const char *location = NULL;

	if (argc < 3) {
		PUTS("Usage: bpbme280 <destEID> <sourceEID> [-t<ttl>] [-a0x76|0x77] [-d/dev/i2c-X] [-loc<location>]");
		return 0;
	}
	destEid = argv[1];
	sourceEid = argv[2];
	for (int i = 3; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 't') {
			ttl = atoi(argv[i] + 2);
		} else if (argv[i][0] == '-' && argv[i][1] == 'a') {
			i2c_addr = (int)strtol(argv[i] + 2, NULL, 0);
		} else if (argv[i][0] == '-' && argv[i][1] == 'd') {
			i2c_dev = argv[i] + 2;
		} else if (strncmp(argv[i], "-loc", 4) == 0) {
			location = argv[i] + 4;
		}
	}

	if (ttl <= 0) {
		PUTS("[?] ttl must be > 0");
		return 0;
	}

	/* Attach to BP & start attendant (same pattern as bpsource) */
	if (bp_attach() < 0) {
		putErrmsg("Can't attach to BP.", NULL);
		return 0;
	}
	ReqAttendant attendant;
	if (ionStartAttendant(&attendant)) {
		putErrmsg("Can't initialize blocking transmission.", NULL);
		bp_detach();
		return 0;
	}
	_attendant(&attendant);
	Sdr sdr = bp_get_sdr();

	/* Open I2C and set slave address */
	int i2c_fd = open(i2c_dev, O_RDWR);
	if (i2c_fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", i2c_dev, strerror(errno));
		goto cleanup;
	}
	if (ioctl(i2c_fd, I2C_SLAVE, i2c_addr) < 0) {
		fprintf(stderr, "Failed to set I2C addr 0x%02X: %s\n", i2c_addr, strerror(errno));
		goto cleanup;
	}

	/* Confirm BME280 presence (not fatal if mismatched; just warn) */
	uint8_t chip = 0;
	if (i2c_read_reg(i2c_fd, REG_ID, &chip) < 0 || chip != BME280_CHIP_ID) {
		fprintf(stderr, "[?] Unexpected chip-id 0x%02X (expected 0x%02X). Check wiring/address.\n",
		        chip, BME280_CHIP_ID);
	}

	/* Read calibration & configure sensor */
	bme280_calib_t calib;
	if (bme280_read_calib(i2c_fd, &calib) < 0) {
		putErrmsg("Failed to read BME280 calibration.", NULL);
		goto cleanup;
	}
	if (bme280_configure(i2c_fd) < 0) {
		putErrmsg("Failed to configure BME280.", NULL);
		goto cleanup;
	}

	/* Short delay and poll status to ensure a fresh measurement */
	usleep(100000);
	for (int i = 0; i < 5; i++) {
		uint8_t st = 0;
		if (i2c_read_reg(i2c_fd, REG_STATUS, &st) == 0 && (st & 0x08) == 0) break;
		usleep(20000);
	}

	/* Compose compact JSON payload */
	char json[256];
	int len = compose_json(json, sizeof json, i2c_fd, &calib, location, sourceEid);
	if (len < 0) {
		putErrmsg("Failed to read/compose JSON.", NULL);
		goto cleanup;
	}

	/* Print for user (keep visible output, as requested) */
	printf("JSON: %s\n", json);
	fflush(stdout);

	/* Create ZCO and send a single bundle, then exit */
	CHKZERO(sdr_begin_xn(sdr));
	Object extent = sdr_malloc(sdr, len);
	if (extent) { sdr_write(sdr, extent, json, len); }
	if (sdr_end_xn(sdr) < 0) {
		putErrmsg("No space for ZCO extent.", NULL);
		goto cleanup;
	}

	Object zco = ionCreateZco(ZcoSdrSource, extent, 0, len,
	                          BP_STD_PRIORITY, 0, ZcoOutbound, &attendant);
	if (zco == 0 || zco == (Object)ERROR) {
		putErrmsg("Can't create ZCO extent.", NULL);
		goto cleanup;
	}

	Object newBundle;
	if (bp_send(NULL, destEid, NULL, ttl, BP_STD_PRIORITY,
	            NoCustodyRequested, 0, 0, NULL, zco, &newBundle) < 1)
	{
		putErrmsg("bpbme280 can't send ADU.", NULL);
		goto cleanup;
	}

	PUTS("[i] bpbme280 sent one bundle and will exit.");

cleanup:
	if (_attendant(NULL)) { ionStopAttendant(_attendant(NULL)); }
	bp_detach();
	if (i2c_fd >= 0) close(i2c_fd);
	return 0;
}
