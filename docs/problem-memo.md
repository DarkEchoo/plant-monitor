# Problem memo -- Plant Monitor (Jesse and Michael)

## The user

Michael is the primary user. He owns a plant that will be kept in a consistent
indoor location during testing. He will provide the plant and select the
location where the device will be tested. Jesse and Michael will install,
program, and troubleshoot the device together.

## The problem

Michael cannot easily determine whether his plant receives enough light
throughout the day. The location may appear bright when he checks it but still
receive too little accumulated light over several hours or days. This makes it
difficult to decide whether the plant should be moved to a brighter location.

He currently checks the plant once a week, but these brief observations do not
represent its total daily light exposure. Insufficient light could result in
poor growth or declining plant health. Soil moisture will also be monitored so
that a watering problem is not mistaken for a lighting problem.

## Why a device

The plant’s light and soil conditions must be measured throughout the day,
including when nobody is present. A phone does not measure soil moisture, is not
kept beside the plant continuously, and would not provide consistent readings
from the same position.

An always-on Raspberry Pi can collect measurements over several hours or days,
preserve their history, and report whether the plant’s current location appears
to provide enough light.

## The sensors

An Adafruit BH1750 will measure ambient illuminance in lux. An Adafruit STEMMA
capacitive soil sensor will measure relative soil moisture.

The sensors will cooperate via a single plant-care analysis pipeline. The system
will calculate accumulated light exposure over time and compare it with the
selected plant’s documented light needs. Soil-moisture trends will provide
additional context, allowing the system to distinguish a possible lighting
problem from a possible watering problem.

## The mechanisms

1. **D — Custom storage layer:** The system will store timestamped sensor
   readings in an append-only log using batched writes. Recovery testing will
   determine whether valid records can be preserved when power is lost during a
   write.

2. **E — Multi-process architecture:** Separate processes will collect light
   and soil-moisture readings and send them to an analysis process through
   Unix-domain sockets. A supervisor will detect and restart a failed process
   while allowing the healthy sensor process to continue.

## The risk

The largest risk is that the BH1750 measures visible illuminance in lux rather
than photosynthetically active radiation, so its readings are only an estimate
of the light useful to the plant. Sensor position, artificial lighting, and an
incorrectly selected light requirement could produce misleading advice.

To reduce this risk, the team will select one plant species, document its light
requirements, place the sensor near the plant’s leaves, keep its orientation
consistent, and compare measurements from brighter and darker locations. The
soil sensor will also be calibrated for one fixed plant, pot, and soil mixture
rather than reported as a universal moisture percentage.
