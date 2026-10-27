#ifndef SENSORS_SEESAW_SOIL_H
#define SENSORS_SEESAW_SOIL_H

#include "sensors/i2c_transport.h"

/*
 * Adafruit STEMMA Soil Sensor (seesaw firmware): initialization and raw
 * capacitance acquisition only. No calibration, moisture percentage,
 * trends, fusion or advice: those are reserved for the team's own analysis.
 *
 * Sources (read as text; full SHA-256 hashes in docs/reviews/M2-step4-soil.md):
 * - adafruit/seesaw firmware @ 20e2892: boards/soil/board.mk
 *   ("CHIP_FAMILY = SAMD10", "CHIP_VARIANT = SAMD10D14AM"),
 *   boards/soil/board_config.h ("CONFIG_I2C_SLAVE_ADDR 0x36",
 *   "CONFIG_TOUCH0 1"), include/RegisterMap.h, source/Delegate.cpp,
 *   source/I2CSlave.cpp, source/AOTouch.cpp, bsp/adafruit_ptc.cpp.
 * - Adafruit_Seesaw (Arduino) 1.7.9 @ 5b3c3db and
 *   Adafruit_CircuitPython_seesaw @ 13ba8ba (register reads, delays).
 * - Adafruit STEMMA Soil Sensor guide, Pinouts ("between 0x36 and 0x39
 *   inclusive").
 * Real hardware timing and the board's actual ID are unverified.
 *
 * Stateless: every function borrows an already-open i2c_device and never
 * opens or closes it. soil_init must succeed before soil_measure_capacitance
 * is used, and again after the sensor loses power. The driver needs
 * exclusive use of the sensor's address.
 *
 * A seesaw register read is a two-byte register-select write (STOP), a
 * wait, then a separate read (STOP), as both Adafruit libraries do it.
 *
 * Each function stops at the first failed step and reports that step's
 * transport result unchanged. A reading is written only after a complete,
 * successful, in-range read; on any failure or rejection *out is left
 * untouched. Retries and backoff belong to the caller, and timestamps to
 * the capture layer.
 */

/*
 * Firmware I2CSlave.cpp starts from CONFIG_I2C_SLAVE_ADDR (0x36 for this
 * board) and adds the two ADDR pins as bits 0 and 1 ("addrmask = 0x3"); the
 * guide's "between 0x36 and 0x39 inclusive" says the same.
 */
#define SOIL_ADDRESS_MIN 0x36  /* no address jumpers */
#define SOIL_ADDRESS_MAX 0x39  /* AD0 adds 1, AD1 adds 2 */

/* RegisterMap.h */
#define SOIL_STATUS_BASE     0x00
#define SOIL_STATUS_HW_ID    0x01
#define SOIL_STATUS_SWRST    0x7f
#define SOIL_TOUCH_BASE      0x0f
#define SOIL_TOUCH_CHANNEL_0 0x10

/*
 * Data byte sent with SWRST by both libraries. The firmware ignores its
 * value but needs one: a write with no data bytes is treated as a read
 * (I2CSlave.cpp passes "bytes_received - 2" as the length, and Delegate.cpp
 * reads when "!len"), so a bare {0x00, 0x7F} would not reset the chip.
 */
#define SOIL_SWRST_VALUE 0xff

/*
 * SEESAW_STATUS_HW_ID_CODE in the firmware's RegisterMap.h. It is defined
 * once, for every SAMD build, so it identifies SAMD-family seesaw firmware,
 * not a specific chip. The soil board is a SAMD10 (board.mk). The Arduino
 * library names the same value SEESAW_HW_ID_CODE_SAMD09.
 */
#define SOIL_HW_ID_SAMD_SEESAW 0x55

/*
 * Waits, all vendor-library values, using the larger first-attempt value
 * where the libraries differ. Real hardware timing is unverified.
 */
#define SOIL_RESET_WAIT_US   500000u  /* Python sw_reset post_reset_delay=0.5 */
#define SOIL_HW_ID_WAIT_US     8000u  /* Python read() default delay=0.008 */
#define SOIL_MEASURE_WAIT_US   5000u  /* Python moisture_read 0.005; Arduino
                                         touchRead first try 3000 */

/*
 * Largest reading accepted. Adafruit_CircuitPython_seesaw treats "ret >
 * 4095" as a bad reading and retries ("retry if reading was bad"); PR #24
 * (2019-02) added it after values above 4095 were seen on a Raspberry Pi.
 * It is an empirical threshold, not a documented range.
 *
 * Current firmware cannot exceed 1023: AOTouch/adafruit_ptc return the
 * 16-bit PTC result divided by the oversample count, and the soil board
 * uses OVERSAMPLE_64 ("case OVERSAMPLE_64: return m/64;"), so the maximum is
 * 65535 / 64 = 1023. The Arduino touchRead is documented as "an integer
 * between 0 and 1023". OVERSAMPLE_64 arrived in firmware commit 3a39cd0
 * (2019-12-15); before it the soil board used OVERSAMPLE_4 (bound 16383),
 * which is the firmware PR #24 ran on.
 *
 * Limitation: on current firmware, values 1024-4095 are impossible but are
 * still accepted, so a corrupted read in that band is not detected. The
 * cutoff is kept at 4095 (approved) because the shipped firmware version is
 * unverified; tightening it is a team decision once the board's version is
 * known.
 */
#define SOIL_CAPACITANCE_MAX_VALID_COUNTS 4095u

/*
 * Stored codes: the storage record proposed in Michael's storage design
 * (PR #5, docs/design.md) writes these values to disk in byte 37, so
 * existing values never change. Add new stages only at the end, with the
 * next unused number.
 */
typedef enum {
    SOIL_STAGE_NONE = 0,             /* success */
    SOIL_STAGE_ARGUMENT = 1,         /* rejected before any I/O */
    SOIL_STAGE_RESET = 2,
    SOIL_STAGE_RESET_WAIT = 3,
    SOIL_STAGE_HW_ID_SELECT = 4,
    SOIL_STAGE_HW_ID_WAIT = 5,
    SOIL_STAGE_HW_ID_READ = 6,
    SOIL_STAGE_HW_ID = 7,            /* read succeeded, ID is not 0x55 */
    SOIL_STAGE_MEASURE_SELECT = 8,
    SOIL_STAGE_MEASURE_WAIT = 9,
    SOIL_STAGE_MEASURE_READ = 10,
    SOIL_STAGE_RANGE = 11            /* read succeeded, value above 4095 */
} soil_stage;

typedef struct {
    i2c_result io;          /* the failing step's transport result */
    soil_stage stage;       /* which step failed, or NONE */
    uint16_t rejected_value;  /* HW_ID: the ID read; RANGE: the counts */
} soil_status;

typedef struct {
    uint16_t capacitance_counts;  /* raw seesaw touch value, uncalibrated */
    int is_simulated;             /* copied from the device */
} soil_reading;

/* Software reset, then check the hardware ID. Address must be 0x36-0x39. */
soil_status soil_init(i2c_device *dev);

/* One raw capacitance reading; soil_init must have succeeded. */
soil_status soil_measure_capacitance(i2c_device *dev, soil_reading *out);

/* Stable text for logs, e.g. "HW_ID". */
const char *soil_stage_name(soil_stage stage);

#endif
