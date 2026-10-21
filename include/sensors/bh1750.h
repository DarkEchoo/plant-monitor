#ifndef SENSORS_BH1750_H
#define SENSORS_BH1750_H

#include "sensors/i2c_transport.h"

/*
 * ROHM BH1750FVI ambient light sensor: one-time H-resolution measurements.
 *
 * Source: ROHM "BH1750FVI Technical Note", Rev.C, 2010.04 (17 pages). Rev.D
 * (2011.11) exists but was not read, so nothing here is taken from it.
 *
 * Stateless: every function borrows an already-open i2c_device and never
 * opens or closes it. The driver needs exclusive use of the sensor's
 * address: if anything else sends it commands (another process, or a
 * second driver on the same address), the measurement-time register and
 * the measurement sequence below are no longer guaranteed.
 *
 * bh1750_init must succeed before bh1750_measure_once_h is used, and again
 * after the sensor loses power. The note requires one opcode per
 * transaction ("Please insert SP every 1 Opecode"), so it sends three
 * STOP-terminated writes:
 *   Power On (0x01), then MTreg high bits (0x42), then MTreg low bits
 *   (0x65), setting the measurement-time register to its documented default
 *   69 ("0100_0101") explicitly, because the note does not say Power On
 *   restores it. The wait and the conversion below assume MTreg = 69.
 *   Rev.C does not say whether MTreg survives the automatic Power Down after
 *   a one-time measurement; that is assumed and must be confirmed on the
 *   hardware.
 *
 * bh1750_measure_once_h sends One Time H-Resolution Mode (0x20), waits the
 * documented maximum measurement time (180 ms), and reads two bytes, high
 * byte first. The sensor powers itself down after the measurement.
 *
 * Every function stops at the first failed step and reports that step's
 * transport result unchanged. A reading is written only after a complete,
 * successful two-byte read; on any failure *out is left untouched.
 * Timestamps are not taken here: the capture layer stamps each reading.
 */

#define BH1750_ADDRESS_ADDR_LOW  0x23  /* ADDR <= 0.3 VCC: "0100011" */
#define BH1750_ADDRESS_ADDR_HIGH 0x5c  /* ADDR >= 0.7 VCC: "1011100" */

#define BH1750_CMD_POWER_ON         0x01  /* "0000_0001" */
#define BH1750_CMD_MTREG_HIGH_69    0x42  /* "01000_MT[7,6,5]", MT = 69 */
#define BH1750_CMD_MTREG_LOW_69     0x65  /* "011_MT[4,3,2,1,0]", MT = 69 */
#define BH1750_CMD_ONE_TIME_H_RES   0x20  /* "0010_0000" */

/*
 * H-Resolution Mode Measurement Time, Electrical Characteristics: tHR
 * min -, typ 120, max 180 ms, specified at VCC = 3.0V, DVI = 3.0V,
 * Ta = 25 C (symbols transliterated to ASCII). The board runs at 3.3 V and
 * may see other temperatures; the note gives no figure for those.
 */
#define BH1750_H_RES_MAX_WAIT_US 180000u

typedef enum {
    BH1750_STAGE_NONE = 0,       /* success */
    BH1750_STAGE_ARGUMENT,       /* rejected before any I/O */
    BH1750_STAGE_POWER_ON,
    BH1750_STAGE_MTREG_HIGH,
    BH1750_STAGE_MTREG_LOW,
    BH1750_STAGE_MEASURE_COMMAND,
    BH1750_STAGE_WAIT,
    BH1750_STAGE_READ
} bh1750_stage;

typedef struct {
    i2c_result io;       /* the failing step's transport result, unchanged */
    bh1750_stage stage;  /* which step failed, or NONE */
} bh1750_status;

typedef struct {
    uint16_t raw_count;        /* sensor counts, high byte first */
    uint32_t illuminance_mlx;  /* millilux; see bh1750_raw_to_mlx */
    int is_simulated;          /* copied from the device */
} bh1750_reading;

/* Power On and set MTreg = 69. dev must be open at 0x23 or 0x5c. */
bh1750_status bh1750_init(i2c_device *dev);

/* One H-resolution measurement; bh1750_init must have succeeded. */
bh1750_status bh1750_measure_once_h(i2c_device *dev, bh1750_reading *out);

/*
 * Counts to millilux at MTreg 69, using the note's typical factor:
 * lx = counts / 1.2, so mlx = counts * 2500 / 3, rounded to the nearest
 * integer. The remainder of a division by 3 is 0, 1 or 2 (a fraction of
 * 0, 1/3 or 2/3), so there is never a tie. Computed as
 * (counts * 2500 + 1) / 3 in uint32_t: the largest intermediate is
 * 65535 * 2500 + 1 = 163837501, below 2^32. The factor 1.2 is the typical
 * "Measurement Accuracy" (min 0.96, max 1.44); this is not a calibration.
 */
uint32_t bh1750_raw_to_mlx(uint16_t raw_count);

/* Stable text for logs, e.g. "MTREG_HIGH". */
const char *bh1750_stage_name(bh1750_stage stage);

#endif
