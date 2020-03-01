# IOT Intercom

A couple of year ago I stumbled upon an old German industrial phone. This thing was manufactured by Funke & Huster, weights 17 kilograms and is supposed to be explosion proof.I live in an apartment building with a Siedle intercom system and thought this would make a nice replacement of the ugly plastic Siedle doorphone. Of course replacing the hardware provided a perfect opportunity to integrate the intercom into my Domotica system.

![Banaan](/images/siedle_to_funke.jpg)

This repo contains some software and schematics to help you do the same.


### Features
Talks MQTT (Homie convention) to interface with Domotica software so you can;
- Receive a notification when someone rings your bell
- Use voice command to unlock the door
- Automatically pause Kodi when doorbell is pressed
- Remotely unlock the door.
- Uses the original ringer for physical notifications
- The rotary input can be used to start an action or toggle settings
- Auto-unlock feature automatically unlocks the door when the doorbell button is pressed.
- Mute function for when you do not want to be disturbed..

### Hardware used
- NodeMCU v3
- Siedle HTC 711-01 (only need the print)
- 2x Boost Buck DC adjustable step up down Converter XL6009
- Funke & Huster Bunker phone
- L298N H Bridge Motor Driver Board
- 4N25 OctoCoupler
- Zenerdiode 1N4746
- Some resistors and capacitors and transistors
- sip-1a05 reed relay.


### Hardware

### Interfacing with Siedle 1+n
My apartment building has a Siedle 1+n system which uses a two wire system. One common wire and one an individual signal wire. Nfort unately the audi
T
To interface the new old phone with the original print of the Siedle HTS711-01 is needed. I managed to buy a second hand spare from ebay.


![Siedle HTS711-01 pcb](/images/HTS711-01_print.jpg)

#### Detecting when someone
The most important feature of the intercom is to let me know if somebody is paging my apartment. I have a 2-wire Siedle intercom system in my apartment building. The voltage on these two wires is 14,2 volt normally but rises to 22,9v when the doorbell is pressed.

A circuit with a Zener diode and an Octocoupler is used to catch this signal.
The 1N4746 has a Zener voltage of 18v and is connected in reverse bias to PIN 7 of the Siedle. When someone pushes the doorbell the voltage rises to 22,9v and exceeds the Zener voltage causing an Avalanche Breakdown to occur resulting in (22,9 - 18) ~4,9 volt  to be let through.

This in turn drives the input source of the Optocoupler.

![Siedle HTS711-01 pcb](/images/schem_doorbell_press_detect.png)

#### Driving the telephone bell
Like most landline phones the bunkerphone has a polarised bell which is normally powered by applying a 50 to 100 volt AC at 20 Hertz. This is not a convenient voltage to work with so I am using an H-Bridge to mimic this. A L298N H-bridge board reverse the polarity on the magnet of the bell every 50ms. I am driving it with ~30v provided by the XL6009 converter.

![Drive bell with h-bridge](/images/schem_l298n_bell.png)


#### Opening the door
To open the central door of my apartment building  (‘buzz someone in’) a small circuit is used to drive a sip-1a05 reed relay. This relays shortens the door release contacts on the HTS-711 print.

![Reed relay circuit](/images/schem_latch_unlock.png)

The relay can be activated by software but it is more convenient to have a physical button. I am using a handle on the bunkerphone to buzz someone in. In my case the switch connected to this handle triggered some unintended interrupts with fluctuations on the power grid so I added some pull-up resistors and a capacitor.


To detect if the ‘buzzer button’ is pressed I use a small debouncing circuit.
![Unlock button schematic](/images/schem_unlock_button.png)


#### Input from the rotary dialer
The phone has a nice rotary dial which I use interface.

![Unlopck button schematic](/images/schem_rotary_dailer.png)


### MQTT Configuration


#### Device attributes | homie/
Device ID | $homie | $name | $state | $nodes
----------|--------|-------|--------|-------
iot-intercom | 3.0.1 | IOT Intercom | ready | latch,bell,rotary,system


#### Device nodes | homie/device-id/
Node | CMD ID | $name | $type | $properties
-----|--------|-------|-------|------------
latch | 100 | Central Entrance Door Latch |Pulse open| unlock,auto_unlock,buzzer-button,last-unlock, unlocks
bell | 200 | Intercom bell | AC Bell | doorbell-pressed,doorbell-pressed-time,ring-bell,short-ring-bell,bell-repeats,bell-offtime-ms,bekl-on-time-ms
rotary | 300 | Rotary dialer input | Old style | rotary-input,last-rotary-input
system | 900 |System settings | command-input,force-mqtt-update, reset,factory reset


#### Latch node properties | homie/device-id/latch
Property|CMD ID|$name|$datatype|$format|$settable|$retained
--------|------|-----|---------|-------|---------|---------
unlock|140|Unlock door|Boolean||Yes|No
auto-unlock|160|Auto unlock door|Boolean|Yes|No
auto-unlock-min|170|Remaining auto unlock minutes|Integer|0:360|Yes|Yes
unlock-button|110|Buzzer button pressed|Boolean||No|No
last-unlock||Last unlock time|String|date time|No|Yes
unlocks||Number of unlocks|Integer|#|No

#### Bell node properties | homie/device-id/bell
Property|CMD ID|$name|$datatype|$format|$settable|$retained
--------|------|-----|---------|-------|---------|---------
doorbell-pressed|210|Doorbell is pressed|Boolean||No|No
doorbell-pressed-time|290|Last time doorbell was pressed|String|date time|No|Yes
ring-bell|240|Ring the bell|Boolean||Yes|No
short-ring-bell|250|Short ring the bell|Boolean||Yes|No
bell-muted|270|Mute the bell|Boolean||Yes|Yes
bell-repeats||Number of times the bell should ring|Integer|1:10|Yes|Yes
bell-off-time-ms||Bell pause duration|Integer|10:10000|Yes|Yes
bell-on-time-ms||Bell on duration|Integer|10:10000|Yes|Yes

#### Rotary node properties | homie/device-id/rotary
Property|CMD ID|$name|$datatype|$format|$settable|$retained
--------|------|-----|---------|-------|---------|---------
rotary-input|310|Rotary dialer input|Integer|0:9|No|No
rotary-input-time|390|Last rotary input time|String||No|Yes

#### System node properties | homie/device-id/SYSTEM
Property|CMD ID|$name|$datatype|$format|$settable|$retained
--------|------|-----|---------|-------|---------|---------
command-input|910|Command Input|Integer|100-999|Yes|No
force-mqtt-update|940|Force MQTT update||Boolean|Yes|No
reset|950|Force reset|Boolan||Yes|No
set-debug|960|Debug enabled|Boolean||Yes|Yes
factory-reset|999|Factory reset|String||Yes|No
