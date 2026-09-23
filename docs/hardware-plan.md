# Breadboard-free hardware plan

## Bill of materials

| Quantity | Component | Requirement |
|---:|---|---|
| 1 | Raspberry Pi 3 Model B+ | Soldered 40-pin GPIO header |
| 1 | Adafruit STEMMA Soil Sensor | I2C sensor with JST-PH 2 mm connector |
| 1 | Adafruit BH1750 light sensor | I2C sensor with two STEMMA QT connectors |
| 2 | Adafruit 4397 cable | JST-SH to female GPIO sockets |
| 1 | Adafruit 4424 cable | JST-SH to JST-PH adapter |
| 1 | MicroSD card | Raspberry Pi OS storage |
| 1 | Raspberry Pi power supply | Compatible 5 V supply |

No breadboard or external analog-to-digital converter is planned.

## Physical connection plan

```text
Raspberry Pi 3 Model B+ GPIO header
                |
                | Adafruit 4397
                v
      BH1750 light sensor
                |
                | Adafruit 4424
                v
       STEMMA soil sensor
```

## Pi header signals

| Signal | BCM name | Physical pin |
|---|---|---:|
| 3.3 V | 3V3 | 1 |
| SDA | GPIO 2 / SDA1 | 3 |
| SCL | GPIO 3 / SCL1 | 5 |
| Ground | GND | 6 |

Use the labels supplied with the cable and boards. Wire colors are a useful
cross-check, not proof of pin function.

## Assembly checklist

1. Shut down the Raspberry Pi and disconnect its power supply.
2. Connect the JST-SH end of cable 4397 to one BH1750 STEMMA QT port.
3. Connect the female sockets from cable 4397 to the Raspberry Pi:
   - Red to physical pin 1 for 3.3 V
   - Black to physical pin 6 for ground
   - Blue to physical pin 3 for SDA
   - Yellow to physical pin 5 for SCL
4. Connect the JST-SH end of cable 4424 to the BH1750's second port.
5. Connect the JST-PH end of cable 4424 to the soil sensor.
6. Confirm every connection using the board and cable labels.
7. Keep the Raspberry Pi and cable connections protected from water.
8. Power on the Raspberry Pi and enable I2C.
9. Run `i2cdetect -y 1` and save the actual output. The expected addresses are
   normally `0x23` for the BH1750 and `0x36` for the soil sensor, but the team
   must verify the addresses reported by the actual hardware.
10. Confirm that each sensor can be detected and read.

## Placement plan

- Fix the soil sensor at a repeatable depth and distance from the plant stem.
- Keep the BH1750 uncovered and oriented consistently toward ambient light.
- Add strain relief so moving the plant cannot pull a cable from the Pi.
- Keep the Pi and connectors protected from watering spills.
- Photograph final placement to make calibration and later reproduction honest.

## Calibration plan

### Soil sensor

The soil reading will initially remain a raw relative measurement. Record a dry
reference, a normally watered reference, and repeated readings without moving
the probe. Do not label the value as a percentage until the team can justify
that conversion for the selected plant, pot, and soil.

### Light sensor

Place the BH1750 at approximately the same height and location as the plant's
leaves. Record readings throughout several days without moving the sensor, then
repeat the test in brighter and darker locations.

The BH1750 measures illuminance in lux rather than photosynthetically active
radiation. The device will therefore report an estimate of whether the plant's
location provides enough light, not an exact biological measurement of how much
usable light the plant receives.

