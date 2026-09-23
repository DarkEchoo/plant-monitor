# Problem memo -- TODO(team name) (TODO(partner 1), TODO(partner 2))

## The user

Jesse: Will provide code along with help via video to help install if needed.

Michael: Will provide code, device, and plant for testing and troubleshooting.

The user does not know whether the plant's current location provides enough
light throughout the day. A location may appear bright during one observation
while still providing too little accumulated light over several hours or days.
The device will record light exposure over time and use soil-moisture readings
as additional context about the plant's growing conditions.

TODO(team): Add the selected plant's light requirements and observations from
the real user.
Michael: Add observed frequency and concrete consequences from the user.

## Why a device

The soil and plant must be measured continuously where they are located. A phone
does not measure soil moisture or remain beside the plant overnight. The device
must observe slow changes unattended and preserve history even when no one is
checking it. Sometimes, people might be too busy to keep track of their plant, 
so this will act as a reminder if the plant is underwatered or has been without
sunlight for too long.

## The sensors

An Adafruit BH1750 measures ambient illuminance, while an Adafruit STEMMA
capacitive soil sensor measures relative soil moisture. The system records
light readings over time to estimate daily light exposure and compares that
exposure with the selected plant's documented needs. Soil moisture provides
additional context and helps identify possible watering problems. The project
will be calibrated for one plant, pot, and location rather than claiming
universal plant-care accuracy.

## The mechanisms

1. **D - custom storage layer:** We’ll save sensor readings in an append-only log
   and write them to the SD card in batches. We’ll also test what happens if power
   is lost during a write and check whether the log can recover.
4. **E - multi-process architecture:** Separate processes will collect sensor data
   and send it to an analysis process through Unix-domain sockets. If one process
   crashes, the system should log the gap and restart it without making up missing
   readings.

## The risk

The largest risk is that raw soil-sensor readings may drift with plant placement,
artificial light, different plants, different pots (if it outgrows its current pot)
and plant conditions. We will keep the plant in one fixed pot, record
placement, calibrate wet and dry reference ranges, and
report limitations rather than presenting the reading as a universal percentage.

