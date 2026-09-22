# Breadboard-free hardware plan

## Bill of materials

| Quantity | Component | Requirement |
|---:|---|---|
| 1 | Raspberry Pi 3 Model B+ | Soldered 40-pin male GPIO header |
| 1 | Adafruit STEMMA Soil Sensor | I2C capacitive moisture sensor |
| 1 | Adafruit BH1750 light sensor | STEMMA QT/I2C version |
| 2 | STEMMA QT cables | Board-to-board sensor connection |
| 1 | STEMMA QT-to-GPIO cable | Female sockets at Pi end |

No breadboard or external analog-to-digital converter is planned.

## Physical connection plan

```text
Raspberry Pi GPIO header
        |
        | STEMMA QT-to-GPIO cable
        v
Adafruit STEMMA Soil Sensor
        |
        | STEMMA QT cable
        v
Adafruit BH1750 light sensor
```

One of the two ordinary STEMMA QT cables is a spare/replacement unless the final
physical routing or adapter configuration needs both. Confirm the number and
type of connectors on the exact purchased revisions before ordering.

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

1. Shut down and disconnect power from the Pi.
2. Confirm both sensor boards are the I2C/STEMMA QT versions.
3. Confirm the Pi-end cable has female sockets for the male GPIO header.
4. Match 3.3 V, GND, SDA, and SCL using labels and physical pin numbers.
5. Connect the second sensor through a free STEMMA QT connector.
6. Keep the electronics outside the soil; insert only the sensing portion to
   the depth permitted by the sensor documentation.
7. Power on and enable I2C in Raspberry Pi configuration.
8. Record the real output of `i2cdetect -y 1` in the hardware bring-up notes.
9. Confirm each device independently before running the combined pipeline.

## Placement plan

- Fix the soil sensor at a repeatable depth and distance from the plant stem.
- Keep the BH1750 uncovered and oriented consistently toward ambient light.
- Add strain relief so moving the plant cannot pull a cable from the Pi.
- Keep the Pi and connectors protected from watering spills.
- Photograph final placement to make calibration and later reproduction honest.

## Calibration plan

The soil value will initially remain a raw relative measurement. Record a dry
reference, a normal-watered reference, and repeated readings without moving the
probe. Do not label the value as a percentage until the team can justify that
conversion for the selected plant and soil.

