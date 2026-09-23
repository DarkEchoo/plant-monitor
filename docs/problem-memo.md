# Problem memo -- TODO(team name) (TODO(partner 1), TODO(partner 2))

## The user

Jesse: Will provide code along with help via video to help install if needed.

Michael: Will provide code, device, and plant for testing and troubleshooting.

## The problem

The user does not know when the plant will need water until visible symptoms or
very dry soil appear. A fixed watering schedule ignores changes in light and the
resulting drying rate, potentially causing overwatering or underwatering.
Michael: Add observed frequency and concrete consequences from the user.

## Why a device

The soil and plant must be measured continuously where they are located. A phone
does not measure soil moisture or remain beside the plant overnight. The device
must observe slow changes unattended and preserve history even when no one is
checking it. Sometimes, people might be too busy to keep track of their plant, 
so this will act as a reminder if the plant is underwatered or has been without
sunlight for too long.

## The sensors

An Adafruit STEMMA capacitive soil sensor measures relative soil moisture, while
a physically separate BH1750 measures ambient illuminance (sunlight). They feed one shared
pipeline: the system calculates moisture-loss rate and compares it with recent
accumulated light exposure. It also checks whether watering produces a plausible
moisture increase. The project is calibrated for one plant-and-soil setup rather
than claiming universal plant-care accuracy.

## The mechanisms

1. **D - custom storage layer:** We’ll save sensor readings in an append-only log
   and write them to the SD card in batches. We’ll also test what happens if power
   is lost during a write and check whether the log can recover.
4. **E - multi-process architecture:** Separate processes will collect sensor data
   and send it to an analysis process through Unix-domain sockets. If one process
   crashes, the system should log the gap and restart it without making up missing
   readings.

## The risk

The largest risk is that raw soil-sensor readings may drift with soil placement,
salinity, and plant conditions. The team will narrow the claim to one fixed
plant and pot, record placement, calibrate wet and dry reference ranges, and
report limitations rather than presenting the reading as a universal percentage.

