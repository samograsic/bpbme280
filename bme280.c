// Build:  gcc -O2 -Wall -Wextra -std=c11 bme280.c -o bme280
// Usage:  ./bme280 [/dev/i2c-1] [0x76|0x77]
//
// Example: ./bme280
//          ./bme280 /dev/i2c-1 0x77
//
// Notes:
//  - Enable I2C on the Pi (sudo raspi-config → Interface Options → I2C).
//  - Confirm the sensor and its address with: sudo i2cdetect -y 1
//  - BME280 chip-id should be 0x60.

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BME280_CHIP_ID 0x60

// Registers
#define REG_ID          0xD0
#define REG_RESET       0xE0
#define REG_CTRL_HUM    0xF2
#define REG_STATUS      0xF3
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5
#define REG_PRESS_MSB   0xF7 // F7..F9
#define REG_TEMP_MSB    0xFA // FA..FC
#define REG_HUM_MSB     0xFD // FD..FE

// Calibration regions
#define CALIB00         0x88 // 0x88..0xA1 (T & P + H1 at 0xA1)
#define CALIB26         0xE1 // 0xE1..0xE7 (H2..H6)

typedef struct {
    // Temperature
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;

    // Pressure
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;

    // Humidity
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;

    // t_fine used in compensation
    int32_t  t_fine;
} bme280_calib_t;

static int i2c_write_reg(int fd, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    if (write(fd, buf, 2) != 2) return -1;
    return 0;
}

static int i2c_write(int fd, const uint8_t *buf, size_t len) {
    ssize_t w = write(fd, buf, len);
    return (w == (ssize_t)len) ? 0 : -1;
}

static int i2c_read_reg(int fd, uint8_t reg, uint8_t *val) {
    if (write(fd, &reg, 1) != 1) return -1;
    if (read(fd, val, 1) != 1) return -1;
    return 0;
}

static int i2c_read_regs(int fd, uint8_t start_reg, uint8_t *buf, size_t len) {
    if (write(fd, &start_reg, 1) != 1) return -1;
    ssize_t r = read(fd, buf, len);
    return (r == (ssize_t)len) ? 0 : -1;
}

static int read_calibration(int fd, bme280_calib_t *c) {
    uint8_t buf1[26]; // 0x88..0xA1 inclusive is 26 bytes
    uint8_t buf2[7];  // 0xE1..0xE7 is 7 bytes

    if (i2c_read_regs(fd, CALIB00, buf1, sizeof(buf1)) < 0) return -1;
    if (i2c_read_regs(fd, CALIB26, buf2, sizeof(buf2)) < 0) return -1;

    // Little-endian helpers
    #define U16_LE(p) ((uint16_t)((p)[0] | ((uint16_t)(p)[1] << 8)))
    #define S16_LE(p) ((int16_t)((p)[0] | ((int16_t)(p)[1] << 8)))

    c->dig_T1 = U16_LE(&buf1[0]);
    c->dig_T2 = S16_LE(&buf1[2]);
    c->dig_T3 = S16_LE(&buf1[4]);

    c->dig_P1 = U16_LE(&buf1[6]);
    c->dig_P2 = S16_LE(&buf1[8]);
    c->dig_P3 = S16_LE(&buf1[10]);
    c->dig_P4 = S16_LE(&buf1[12]);
    c->dig_P5 = S16_LE(&buf1[14]);
    c->dig_P6 = S16_LE(&buf1[16]);
    c->dig_P7 = S16_LE(&buf1[18]);
    c->dig_P8 = S16_LE(&buf1[20]);
    c->dig_P9 = S16_LE(&buf1[22]);

    c->dig_H1 = buf1[24]; // 0xA1

    c->dig_H2 = S16_LE(&buf2[0]); // E1, E2
    c->dig_H3 = buf2[2];          // E3

    // H4 and H5 are packed across bytes:
    // H4: E4 << 4 | (E5 & 0x0F)
    // H5: E6 << 4 | (E5 >> 4)
    int16_t h4 = (int16_t)((((int16_t)buf2[3]) << 4) | (buf2[4] & 0x0F));
    int16_t h5 = (int16_t)((((int16_t)buf2[5]) << 4) | (buf2[4] >> 4));
    // Convert to signed properly (13-bit-ish packed signed). Casting above keeps sign for high byte path.

    c->dig_H4 = h4;
    c->dig_H5 = h5;
    c->dig_H6 = (int8_t)buf2[6]; // E7

    return 0;
}

static int configure_bme280(int fd) {
    // Humidity oversampling x1
    if (i2c_write_reg(fd, REG_CTRL_HUM, 0x01) < 0) return -1;

    // ctrl_meas: temp oversampling x1 (001), press oversampling x1 (001), mode = normal (11)
    // [osrs_t(7:5)=001][osrs_p(4:2)=001][mode(1:0)=11] => 0b00100111 = 0x27
    if (i2c_write_reg(fd, REG_CTRL_MEAS, 0x27) < 0) return -1;

    // config: standby 500ms (100), filter off (000), spi3w off (0)
    // [t_sb(7:5)=100][filter(4:2)=000][spi3w_en(0)=0] => 0b10000000 = 0x80
    if (i2c_write_reg(fd, REG_CONFIG, 0x80) < 0) return -1;

    return 0;
}

static int read_raw_data(int fd, int32_t *adc_T, int32_t *adc_P, int32_t *adc_H) {
    uint8_t data[8]; // F7..FE: press(3), temp(3), hum(2)
    if (i2c_read_regs(fd, REG_PRESS_MSB, data, sizeof(data)) < 0) return -1;

    *adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    *adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    *adc_H = ((int32_t)data[6] << 8) | data[7];

    return 0;
}

// Compensation formulas from BME280 datasheet (integer fixed-point style)
static double compensate_T(int32_t adc_T, bme280_calib_t *c) {
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)c->dig_T1 << 1))) * ((int32_t)c->dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)c->dig_T1)) * ((adc_T >> 4) - ((int32_t)c->dig_T1))) >> 12) *
            ((int32_t)c->dig_T3)) >> 14;
    c->t_fine = var1 + var2;
    double T = (c->t_fine * 5 + 128) >> 8; // °C * 100
    return T / 100.0;
}

static double compensate_P(int32_t adc_P, bme280_calib_t *c) {
    int64_t var1, var2, p;
    var1 = ((int64_t)c->t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)c->dig_P6;
    var2 = var2 + ((var1 * (int64_t)c->dig_P5) << 17);
    var2 = var2 + (((int64_t)c->dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)c->dig_P3) >> 8) + ((var1 * (int64_t)c->dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)c->dig_P1) >> 33;

    if (var1 == 0) return 0; // avoid div by zero

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)c->dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)c->dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)c->dig_P7) << 4);

    // p is in Q4.4 Pascals. Convert to hPa:
    return ((double)p) / 25600.0; // 1 hPa = 100 Pa; p/256 -> Pa, then /100 => /25600
}

static double compensate_H(int32_t adc_H, bme280_calib_t *c) {
    int32_t v_x1_u32r;
    v_x1_u32r = (c->t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)c->dig_H4) << 20) - (((int32_t)c->dig_H5) * v_x1_u32r)) + ((int32_t)16384)) >> 15) *
                  (((((((v_x1_u32r * ((int32_t)c->dig_H6)) >> 10) * (((v_x1_u32r * ((int32_t)c->dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) *
                      ((int32_t)c->dig_H2) + 8192) >> 14));
    v_x1_u32r = v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)c->dig_H1)) >> 4);
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    double h = (v_x1_u32r >> 12); // Q20.12 -> divide by 4096
    return h / 1024.0; // gives %RH
}

int main(int argc, char **argv) {
    const char *i2c_dev = "/dev/i2c-1";
    int addr = 0x76;

    if (argc >= 2) i2c_dev = argv[1];
    if (argc >= 3) addr = (int)strtol(argv[2], NULL, 0);

    int fd = open(i2c_dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", i2c_dev, strerror(errno));
        return 1;
    }
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        fprintf(stderr, "Failed to set I2C address 0x%02X: %s\n", addr, strerror(errno));
        close(fd);
        return 1;
    }

    uint8_t id = 0;
    if (i2c_read_reg(fd, REG_ID, &id) < 0) {
        fprintf(stderr, "Failed to read chip ID\n");
        close(fd);
        return 1;
    }
    if (id != BME280_CHIP_ID) {
        fprintf(stderr, "Unexpected chip ID: 0x%02X (expected 0x%02X). Is this a BME280? Address correct?\n",
                id, BME280_CHIP_ID);
        // Not exiting immediately—some clones still report 0x60, others may differ.
    } else {
        printf("BME280 detected (chip-id 0x%02X) at 0x%02X on %s\n", id, addr, i2c_dev);
    }

    // Soft reset (optional)
    // i2c_write_reg(fd, REG_RESET, 0xB6); usleep(3000);

    bme280_calib_t calib;
    if (read_calibration(fd, &calib) < 0) {
        fprintf(stderr, "Failed to read calibration data\n");
        close(fd);
        return 1;
    }

    if (configure_bme280(fd) < 0) {
        fprintf(stderr, "Failed to configure sensor\n");
        close(fd);
        return 1;
    }

    // Wait for a fresh measurement (normal mode + 500ms standby; oversampling x1 is quick, but guard anyway)
    usleep(100000); // 100 ms

    // Optionally poll STATUS[3] measuring bit
    for (int i = 0; i < 10; i++) {
        uint8_t st = 0;
        if (i2c_read_reg(fd, REG_STATUS, &st) == 0) {
            if ((st & 0x08) == 0) break; // measuring == 0
        }
        usleep(20000);
    }

    int32_t adc_T, adc_P, adc_H;
    if (read_raw_data(fd, &adc_T, &adc_P, &adc_H) < 0) {
        fprintf(stderr, "Failed to read raw measurement data\n");
        close(fd);
        return 1;
    }

    double temp_c = compensate_T(adc_T, &calib);
    double pres_hpa = compensate_P(adc_P, &calib);
    double hum_rh = compensate_H(adc_H, &calib);

    printf("Temperature: %.2f °C\n", temp_c);
    printf("Pressure:    %.2f hPa\n", pres_hpa);
    printf("Humidity:    %.2f %%RH\n", hum_rh);

    close(fd);
    return 0;
}
