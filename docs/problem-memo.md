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

1. **D - custom storage layer:** An append-only log with batching and an explicit
   synchronization policy will preserve sensor history while limiting SD-card
   writes. Recovery after an interrupted write will be measured.
2. **E - multi-process architecture:** Sensor capture processes will be isolated
   from an analysis hub over Unix-domain sockets. Killing one process must cause
   logged degradation and supervised recovery without silently inventing data.

## The risk

The largest risk is that raw soil-sensor readings may drift with soil placement,
salinity, and plant conditions. The team will narrow the claim to one fixed
plant and pot, record placement, calibrate wet and dry reference ranges, and
report limitations rather than presenting the reading as a universal percentage.

