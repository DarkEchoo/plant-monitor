# Problem memo -- TODO(team name) (TODO(partner 1), TODO(partner 2))

## The user

TODO(team): Name one real person or place where this device could be installed.
For example, this could be a specific person whose basement office becomes damp
and uncomfortable during long work sessions. Confirm the problem with that user
before submission; do not leave the user hypothetical.

## The problem

The user cannot easily tell when a room is becoming both uncomfortable and
moisture-prone during occupied periods. Temperature and humidity can drift over
time, and the user may notice only after discomfort, condensation, or stale-room
conditions have persisted. TODO(team): Add the user's observed frequency and
the concrete cost in discomfort, worry, or property risk.

## Why a device

The condition develops whether or not the user has a phone application open.
An always-on device in the room can observe environmental trends and occupancy
at night or during long unattended periods. A phone does not provide fixed,
continuous measurements at the location and normally has neither a room
humidity sensor nor a fixed view of occupancy.

## The sensors

A DHT22 supplies temperature and relative humidity, and a physically separate
HC-SR501 PIR sensor supplies occupancy events. The measurements cooperate in a
single pipeline: the device maintains rolling environmental trends and uses an
occupancy state machine to distinguish immediate comfort warnings from empty-
room moisture monitoring. The project will not count temperature and humidity
from the DHT22 as two separate physical sensors.

## The mechanisms

1. **D - custom storage layer:** An append-only log with batching and explicit
   synchronization will preserve trend and event history while limiting SD-card
   writes; recovery after an interrupted write will be measured.
2. **E - multi-process architecture:** Sensor capture processes will be isolated
   from a hub/supervisor over Unix-domain sockets. Killing or disconnecting one
   sensor must cause logged degradation and restart without stopping the other.

## The risk

The greatest early risk is that the proposed device solves a generic monitoring
idea rather than a costly problem for a specific user. The team will interview
the named user before finalizing M1 and narrow the device's claim to the room
conditions and actions that user actually cares about.

