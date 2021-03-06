/*
TODO
- Code cleanup
- More configratble parameters (via MQTT/Web)
- More counters
- Config/counters in flash
- Error hadnling (no wifi etc)
  */

//Libs
#include <Arduino.h>
#include <RotaryDialer.h> // https://github.com/markfickett/Rotary-Dial
#include <CircularBuffer.h> // https://github.com/rlogiacco/CircularBuffer
#include <ESP8266WiFi.h>
#include <Ticker.h> // ? https://github.com/esp8266/Arduino/tree/master/libraries/Ticker
#include <WiFiUdp.h>
#include <NTPClient.h> // https://github.com/taranais/NTPClient (is modified to return date)
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <Homie.h>

#define DEBUG
#define SERIAL_COMMANDS

#ifdef DEBUG
 #define DEBUG_PRINT(x)     Serial.print (x)
 #define DEBUG_PRINTDEC(x)     Serial.print (x, DEC)
 #define DEBUG_PRINTLN(x)  Serial.println (x)
#else
 #define DEBUG_PRINT(x)
 #define DEBUG_PRINTDEC(x)
 #define DEBUG_PRINTLN(x)
#endif


// Output PINS
#define HB_INA D3 // H-Bridge INA
#define HB_INB D4 // H-Bridge INB
#define REED_RELAY_PIN D5 //

// Input pins
#define OCTO_COUPLER_PIN D6 // Input to detect when someone ringes the doorbel
#define BUZZER_BUTTON_PIN D7 // Button to press to open the door
#define ROTARY_PULSE_PIN D1 // Input for the rotary dialer pulse pin (normally closed
#define ROTARY_READY_PIN D2 // Input pin for rotary dialer ready pin (normally open)

#define BAUDRATE 115200
#define RINGER_SLOW_PERIOD_MS 50 //  20hz = 50ms cycle, 25hz = 40ms
#define RINGER_FAST_PERIOD_MS 40 //  20hz = 50ms cycle, 25hz = 40ms
#define DOOR_UNLOCK_DURATION 150

#define CHECK_INTERVAL_MS 20
#define MIN_STABLE_VALS 6

#define MQTT_UPDATE_INTERVAL_MS 30000

void silenceBell();
void handler_ringer(void);
void buzzerButtonInterrupt();
void doorBellInterrupt();
bool test();

unsigned long prevIntCheckMillis;
char buzzerButtonStableVals, octoPinStableVals;

// Classes and things
WiFiUDP ntpUDP;
RotaryDialer dialer = RotaryDialer(ROTARY_READY_PIN, ROTARY_PULSE_PIN);

CircularBuffer<int, 10> commandBuff;
Ticker bellTimer;

enum programStates {
  STATE_RESET, // Reset
  STATE_INIT, // Initialise
  STATE_IDLE, // Idle
  STATE_ROTARY_INPUT,
  STATE_RING_BELL,
  STATE_SHORT_RING_BELL,
  STATE_UNLOCK_DOOR,
  //STATE_DOORBELL_PRESSED,
  STATE_SERIAL_INPUT,
  STATE_MQTT_PUBLISH_UPDATE,
  STATE_HANDLE_ACTION
};

programStates newState = STATE_INIT;
programStates prevState = STATE_IDLE;

struct bellConfig {
  unsigned int onTimeMs;
  unsigned int offTimeMs;
  unsigned int repeats;
};

bellConfig normalBellRing = {1000, 500, 3};
bellConfig shortBellRing = {250, 250, 1};

byte toggle = 0;
byte intToggle = 1;

unsigned long stateTime = 0;
unsigned long prevStateTime = 0;

boolean doorBellIntFlag = false;
boolean doorBellPressed = false;
String lastBellPressedDateTime = "n/a";

boolean autoUnlockFlag = false;
boolean autoUnlockEnabled = false;
unsigned int autoUnlockTimerSec = 0;
String lastUnlockDateTime = "n/a";
int unlockCount = 0;

boolean buzzerPressedFlag = false;

long lastMqttUpdateTimeMS = 0;

int mqttTimer = 0;
int autUnlockTimer = 0;

boolean muteBellEnabled = false;

unsigned int dialerInput = 0;
String lastRotaryInputDateTime = "n/a";

unsigned int bellRepeats = 0;
boolean bellOn = false;

boolean debugEnabled = true;

//Put ISR's in IRAM.
void ICACHE_RAM_ATTR buzzerButtonInterrupt();
void ICACHE_RAM_ATTR doorBellInterrupt();

const long utcOffsetInSeconds = 3600;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", utcOffsetInSeconds, 60000);
//WiFiClient net;

bool latchNodeInputHandler(const HomieRange & range, const String & property, const String & value) {
  boolean action = false;
  if (property == "unlock" && value == "true") {
    newState = STATE_UNLOCK_DOOR;
    action = true;
  }
  if (property == "auto-unlock") {
    if (value == "true") {
      commandBuff.push(161); // Add minutes and comm
      action = true;
    }
    else if (value == "false") {
      autoUnlockTimerSec = 0;
      action = true;
    }

  }
  if (property == "auto-unlock-min") {
    int min = value.toInt();
    if (min > 0) {
      autoUnlockTimerSec = (min * 60);
      action = true;
    }
  }
  if (action == true) {
    commandBuff.push(941); // Queue MQTT update
    return true;
  }
  return false;
}

bool bellNodeInputHandler(const HomieRange & range, const String & property, const String & value) {
  boolean action = false;
  if (property == "ring-bell") {
    if (value == "true") {
      commandBuff.push(241); //
      action = true;
    }
  }
  if (property == "short-ring-bell") {
    if (value == "true") {
      commandBuff.push(251); //
      action = true;
    }
  }
  if (property == "bell-muted") {
    if (value == "true") {
      commandBuff.push(271); // Enable mute
      action = true;
    }
    else if (value == "false") {
      commandBuff.push(270); // Disable mute
      action = true;
    }
  }
  if (property == "bell-repeats") {
    int irep = value.toInt();
    if (irep >= 0 && irep <=10) {
      normalBellRing.repeats = irep;
      action = true;
    }
  }
  if (property == "bell-offtime-ms") {
    int iOfftime = value.toInt();
    if (iOfftime >= 10 && iOfftime <=10000) {
      normalBellRing.offTimeMs = iOfftime;
      action = true;
    }
  }

  if (property == "bell-on-time-ms") {
    int iOntime = value.toInt();
    if (iOntime >= 10 && iOntime <=10000) {
      normalBellRing.onTimeMs = iOntime;
      action = true;
    }
  }
  if (action == true) {
    commandBuff.push(941); // Queue MQTT update
    return true;
  }
  return false;
}

bool systemNodeInputHandler(const HomieRange & range, const String & property, const String & value) {
  boolean action = false;
  if (property == "command-input") {
    int iValue = value.toInt();
    if (iValue >= 100 && iValue <= 999 ) {
      commandBuff.push(iValue);
      action = true;
    }
  }
  if (property == "force-mqtt-update") {
    if (value == "true") {
      commandBuff.push(941);
      action = true;
    }
  }

  if (property == "reset") {
    if (value == "true") {
      commandBuff.push(951);
      action = true;
    }
  }

  if (property == "set-debug") {
    if (value == "true") {
      commandBuff.push(961);
      action = true;
    }
    if (value == "false") {
      commandBuff.push(960);
      action = true;
    }
  }

  if (property == "factory-reset") {
    if (value == "reset") {
      commandBuff.push(999);
      action = true;
    }
  }

  if (action == true) {
    return true;
  }
  return false;
}

HomieNode bellNode("bell", "Intercom bell", "Intercom bell", false, 0 , 0, &bellNodeInputHandler);
HomieNode rotaryNode("rotary", "Rotary dialer", "Input device");
HomieNode latchNode("latch", "Central Entrance Door Latch", "Relay control", false, 0 , 0, &latchNodeInputHandler);
HomieNode systemNode("system", "System settings", "System setting", false, 0 , 0, &systemNodeInputHandler);

void setup(void) {
  #ifdef DEBUG
    Serial.begin(BAUDRATE);
  #endif
  // PINS
  pinMode(HB_INA, OUTPUT);
  pinMode(HB_INB, OUTPUT);
  pinMode(REED_RELAY_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(OCTO_COUPLER_PIN, INPUT_PULLUP); // When low input
  pinMode(BUZZER_BUTTON_PIN, INPUT_PULLUP); // When low input
  attachInterrupt(digitalPinToInterrupt(OCTO_COUPLER_PIN), doorBellInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUZZER_BUTTON_PIN), buzzerButtonInterrupt, FALLING);

  //Latch node confi
  latchNode.advertise("unlock").setName("Unlock door").setDatatype("Boolean").setRetained(false).settable();
  latchNode.advertise("auto-unlock").setName("Auto unlock door for 30 min").setDatatype("Boolean").setRetained(false).settable();
  latchNode.advertise("auto-unlock-min").setName("Remaining auto unlock minutes").setDatatype("Integer").setFormat("0:3600").setRetained(true).settable();
  latchNode.advertise("unlock-button").setName("Unlock button pressed").setDatatype("Boolean").setRetained(false);
  latchNode.advertise("last-unlock").setName("Last unlock time").setDatatype("String").setRetained(true);
  latchNode.advertise("unlocks").setName("Number of unlocks").setDatatype("Integer").setRetained(true);

  // Bell node
  bellNode.advertise("doorbell-pressed").setName("Doorbell is pressed").setDatatype("Boolean").setRetained(false);
  bellNode.advertise("doorbell-pressed-time").setName("Last time doorbell was pressed").setDatatype("String").setRetained(true);
  bellNode.advertise("ring-bell").setName("Short ring the bell").setDatatype("Boolean").setRetained(false).settable();
  bellNode.advertise("short-ring-bell").setName("Short ring the bell").setDatatype("Boolean").setRetained(false).settable();
  bellNode.advertise("bell-muted").setName("Bell is muted").setDatatype("Boolean").setRetained(true).settable();
  bellNode.advertise("bell-repeats").setName("Bell repeats").setDatatype("Integer").setFormat("1:10").setRetained(true).settable();
  bellNode.advertise("bell-ontime-ms").setName("Bell on duration (ms)").setDatatype("Integer").setFormat("10:10000").setRetained(true).settable();
  bellNode.advertise("bell-offtime-ms").setName("Bell off duration (ms)").setDatatype("Integer").setFormat("10:10000").setRetained(true).settable();

  // Rotary node
  rotaryNode.advertise("rotary-input").setName("Rotary dialer input").setDatatype("Integer").setFormat("0:9").setRetained(false).settable();
  rotaryNode.advertise("rotary-input-time").setName("Last rotary input timer").setDatatype("String").setRetained(true);

  // System node
  systemNode.advertise("command-input").setName("MQTT input of commands").setDatatype("Integer").setFormat("100:999").setRetained(false).settable();
  systemNode.advertise("force-mqtt-update").setName("Force MQTT update").setDatatype("Boolean").setRetained(false).settable();
  systemNode.advertise("reset").setName("Force reset").setDatatype("Boolean").setRetained(false).settable();
  systemNode.advertise("set-debug").setName("Debug enabled").setDatatype("Boolean").setRetained(true).settable();
  systemNode.advertise("factory-reset").setName("Send 'reset' to clear").setDatatype("String").setRetained(false).settable();

  timeClient.begin();
//Homie.setGlobalInputHandler(globalInputHandler);
  Homie.disableResetTrigger(); // <- Because I am using D3 for something else
  //Homie.disableLedFeedback(); //
  Homie.disableLogging(); //
  Homie_setFirmware("IOT Doorbell RC1", "28.02.2020");
  Homie.setup();
  DEBUG_PRINTLN("Setup finished");
}

void loop(void) {
  Homie.loop();
  stateTime = millis();
  if (newState != prevState) {
    DEBUG_PRINT(prevState);
    DEBUG_PRINT(" to: ");
    DEBUG_PRINTLN(newState);
  }
    switch(newState) { // Initialisation
      // STATE 0 - RESET
      case STATE_RESET:
        prevState = STATE_RESET;
        newState = STATE_IDLE; //
        DEBUG_PRINTLN("Rebooting after 1 second.");
        ESP.restart();
        // Save data?
      break;

      // STATE 0 - Init
      case STATE_INIT: // Basically the same as setup.
        prevState = STATE_INIT;
        newState = STATE_IDLE;
        //serOutBuff.clear();
        commandBuff.clear();
        // Configure timer
        // https://circuits4you.com/2018/01/02/esp8266-timer-ticker-example/
        //bellTimer.attach_ms(RINGER_PERIOD_MS, handler_ringer); // 20hz = 50ms cycle, 25hz = 40ms
        silenceBell();
        dialer.setup();
      break;

      // STATE 1 - Idle
      case STATE_IDLE:
        if (Homie.isConfigured()) {
          // The device is configured, in normal mode
          if (Homie.isConnected()) { // Device is connected
            if ((stateTime - lastMqttUpdateTimeMS) > MQTT_UPDATE_INTERVAL_MS) {
              timeClient.update(); // Update NTP time
              lastMqttUpdateTimeMS = stateTime;
              commandBuff.push(941); // Queue MQTT update
              DEBUG_PRINTLN("UPDATE MQTT");
            }

          } else {
            // The device is not connected
          }
        } else {
          // The device is not configured, in either configuration or standalone mode
        }

        // Handle auto unlcotimer
        if (autoUnlockTimerSec > 0) { // Auto unlock is engaged
          if (autoUnlockEnabled == false) { // Just enbabled
            autoUnlockEnabled = true;
          }
        }
        if (autoUnlockTimerSec == 0) { // Auto unlock is diabled
          if (autoUnlockEnabled == true) { // Just disabled
            commandBuff.push(170); // Announce disabled
            autoUnlockEnabled = false;
          }
        }

        if (dialer.update()) {
          newState = STATE_ROTARY_INPUT;
        }

        if (!commandBuff.isEmpty()) { // We have actions to do
          newState = STATE_HANDLE_ACTION;
        }

        #ifdef DEBUG
          if (Serial.available() > 0) {
            newState = STATE_SERIAL_INPUT;
          }
        #endif

        // Debouncing of buzzet button pin
        if ((stateTime - prevIntCheckMillis) > CHECK_INTERVAL_MS) {
            autUnlockTimer = autUnlockTimer + CHECK_INTERVAL_MS;
            if (autUnlockTimer >= 1000) {
              if(autoUnlockTimerSec > 0) {autoUnlockTimerSec--;}
              autUnlockTimer = 0;
            }
          prevIntCheckMillis = stateTime;
          // Buzzerbutton debounce
          if ((digitalRead(BUZZER_BUTTON_PIN) == LOW) && (buzzerPressedFlag == true))
          {
            buzzerButtonStableVals++;
            if (buzzerButtonStableVals >= MIN_STABLE_VALS)
            {
              commandBuff.push(111); // Notify buzzer button pressed
              unlockCount++;
              buzzerPressedFlag = false;
              newState = STATE_UNLOCK_DOOR;
              buzzerButtonStableVals = 0;
            }
          }
          else {
            buzzerButtonStableVals = 0;
          }
          // Octocoupler (when the doorbell is pressed)
          if ((digitalRead(OCTO_COUPLER_PIN) == LOW) && (doorBellIntFlag == true))
          {
            octoPinStableVals++;
            if (octoPinStableVals >= MIN_STABLE_VALS)
            {
              doorBellIntFlag = false;
              doorBellPressed = true;
              if (autoUnlockEnabled == true) {
                commandBuff.push(211); // Notify doorbellbutton is pressed
                commandBuff.push(141); // Unlock door
                //commandBuff.push(22); // Notify auto unlock
              }
              else {
                commandBuff.push(211); // Notify doorbellbutton is pressed
              }
              commandBuff.push(241); // Ring bell
              octoPinStableVals = 0;
            }
          }
          else {
            octoPinStableVals = 0;
          }
        }
        prevState = STATE_IDLE;
      break;

      //State 2 - Rotary input
      case STATE_ROTARY_INPUT:
        prevState = STATE_ROTARY_INPUT;
        newState = STATE_IDLE;
        dialerInput = dialer.getNextNumber();

        switch(dialerInput){
          /*
          case 1:
            commandBuff.push(1);
          break;

          case 7:
            commandBuff.push(7);
          break;
          */
          case 6: // Ring bell
            commandBuff.push(241);
          break;
          case 8: // Add 30 min to auto open timer + double short ring to cofirm on
            commandBuff.push(161);
            commandBuff.push(251);
            commandBuff.push(251);
          break;
          case 9: // Switch auto open off + short ring to confirm off
            commandBuff.push(170);
            commandBuff.push(250);
          break;

          default:
            commandBuff.push(310 + dialerInput);
          break;
        }
      break;

      //State 3 - Unlock door
      case STATE_UNLOCK_DOOR: // Activate relay to open door
        if (prevState != STATE_UNLOCK_DOOR) {
          DEBUG_PRINTLN("STATE_UNLOCK_DOOR");
          silenceBell();
          prevStateTime = stateTime;
          digitalWrite(REED_RELAY_PIN, HIGH);
          lastUnlockDateTime = timeClient.getFormattedDate();
          //lastRotaryInputDateTime
        }
        if((stateTime - prevStateTime) >= DOOR_UNLOCK_DURATION) {
          digitalWrite(REED_RELAY_PIN, LOW);
          newState = STATE_IDLE;
        }

        if (doorBellPressed == true && autoUnlockEnabled == false) {
          //commandBuff.push(20); // Notify that we answered the doorbell and buzzed vistor in.
        }

        prevState = STATE_UNLOCK_DOOR;
      break;

      //State 4 - Ring bell
      case STATE_RING_BELL:
        if(prevState!=STATE_RING_BELL) { // First time here
          DEBUG_PRINTLN("Entering STATE_RING_BELL");
          bellOn = true;
          bellTimer.attach_ms(RINGER_SLOW_PERIOD_MS, handler_ringer);
          bellRepeats = 1;
          prevStateTime = stateTime;
        }
        if (bellOn == true) {
          if((stateTime - prevStateTime) > normalBellRing.onTimeMs) {
            silenceBell();
            prevStateTime = stateTime;
            bellOn = false;
            prevStateTime = stateTime;
            if (bellRepeats >= normalBellRing.repeats) {
                newState = STATE_IDLE;
              }
          }
        }
        else if((stateTime  - prevStateTime) > normalBellRing.offTimeMs) {
            DEBUG_PRINT("Bell on: ");
            DEBUG_PRINTLN(bellRepeats);
            bellRepeats++;
            bellTimer.attach_ms(RINGER_SLOW_PERIOD_MS, handler_ringer);
            prevStateTime = stateTime;
            bellOn = true;
          }
        prevState = STATE_RING_BELL;
      break;

      case STATE_SHORT_RING_BELL:
      if(prevState!=STATE_SHORT_RING_BELL) { // First time here
        DEBUG_PRINTLN("Entering STATE_SHORT_RING_BELL");
        bellOn = true;
        bellTimer.attach_ms(RINGER_FAST_PERIOD_MS, handler_ringer);
        bellRepeats = 1;
        prevStateTime = stateTime;
      }
      if (bellOn == true) {
        if((stateTime - prevStateTime) > shortBellRing.onTimeMs) {
          silenceBell();
          prevStateTime = stateTime;
          bellOn = false;
          prevStateTime = stateTime;
          newState = STATE_IDLE;
        }
      }
      prevState = STATE_SHORT_RING_BELL;
      break;

      // State 5 - Serial IN
      case STATE_SERIAL_INPUT:
        int input;
        input = Serial.parseInt();
        if (input > 0) {
          commandBuff.push(input);
          DEBUG_PRINT("Serial input: ");
          DEBUG_PRINTLN(input);
        }
        prevState = STATE_SERIAL_INPUT;
        newState = STATE_IDLE;
      break;

      case STATE_MQTT_PUBLISH_UPDATE:
        prevState = STATE_MQTT_PUBLISH_UPDATE;
        newState = STATE_IDLE; // Don't get trapped in here
        // Updates all reatined values
        latchNode.setProperty("auto-unlock").send((autoUnlockTimerSec > 0) ? "true" : "false");
        latchNode.setProperty("auto-unlock-min").send(String(autoUnlockTimerSec/60));

        latchNode.setProperty("last-unlock").send(lastUnlockDateTime);
        latchNode.setProperty("unlocks").send(String(unlockCount));

        bellNode.setProperty("bell-muted").send((muteBellEnabled == true) ? "true" : "false");
        bellNode.setProperty("doorbell-pressed-time").send(lastRotaryInputDateTime);
        bellNode.setProperty("bell-repeats").send(String(normalBellRing.repeats));
        bellNode.setProperty("bell-ontime-ms").send(String(normalBellRing.onTimeMs));
        bellNode.setProperty("bell-offtime-ms").send(String(normalBellRing.offTimeMs));

        rotaryNode.setProperty("rotary-input-time").send(lastRotaryInputDateTime);
      break;

      // State 7 - Handle actio
      case STATE_HANDLE_ACTION:
        prevState = STATE_HANDLE_ACTION;
        newState = STATE_IDLE; // Don't get trapped in here
        unsigned int iCommand, iAction, iProperty, iNode;
        String strDateTime = "n/a";
        String strAction = "";
        String strProperty = "";
        String strNode ="";
        String strOutcome = "";
        iCommand = commandBuff.shift();

        if(iCommand<100 || iCommand>999) {break;} // We are expecting three digits
        iNode = (iCommand/100) % 10; // latch (1)00, bell (2)00, rotary (3)00
        iProperty = (iCommand/10) % 10;
        iAction = (iCommand % 10);

        if (Homie.isConnected()) {
          strDateTime = timeClient.getFormattedDate();
        }

        switch(iAction) {
          case 0:
            strAction = "false";
            break;
          case 1:
            strAction = "true";
            break;
          case 2:
            strAction = "TOGGLE";
            break;
            case 3:
              strAction = "update";
              break;
          default:
            break;
        }

        switch(iNode) {
          case 1: // Latch
          if (iProperty == 1){
            strProperty = "unlock-button";
            if(iAction == 1) {strOutcome = "true";}

          }
          if (iProperty == 4){
            strProperty ="unlock";
            if (iAction == 1) {newState = STATE_UNLOCK_DOOR; strOutcome = "true";}
            if (iAction == 2) {newState = STATE_UNLOCK_DOOR; strOutcome = "true";}

          }
          if (iProperty == 6){
            strProperty ="auto-unlock";
            if (iAction == 0) { // Reset timer
              autoUnlockTimerSec = 0;
            }
            if (iAction == 1) { // Enabel add seconds to time
              autoUnlockTimerSec = autoUnlockTimerSec + 1800; // Add 30 minitues to unlock timer
              strOutcome = strAction;
            }

            if (iAction == 2) {
              autoUnlockTimerSec = (autoUnlockTimerSec > 0) ? 0 : autoUnlockTimerSec;
              strOutcome = (autoUnlockTimerSec > 0) ? "true" : "false";
            }
            if (iAction == 3) { // update timer
              strProperty = "auto-unlock-min";
              strOutcome = String(autoUnlockTimerSec/60);
            }
            if (iAction < 3) {commandBuff.push(163);} // Force update of timer
          }
          if (Homie.isConnected() && !strOutcome.isEmpty()) {latchNode.setProperty(strProperty).send(strOutcome);}
          break;

          case 2: // bell
          if (iProperty == 1){ //Notify doorbell pressed
            strProperty = "doorbell-pressed";
            strOutcome = "true";
            commandBuff.push(293); // Update datetime
          }
          if (iProperty == 4){ //Ring bell
            strProperty = "ring-bell";
            if (iAction == 1) {newState = STATE_RING_BELL; strOutcome = "true";}
            if (iAction == 2) {newState = STATE_RING_BELL; strOutcome = "true";}
          }
          if (iProperty == 5){ //Short ring bell
            strProperty = "short-ring-bell";
            if (iAction == 1) {newState = STATE_SHORT_RING_BELL; strOutcome = "true";}
            if (iAction == 2) {newState = STATE_SHORT_RING_BELL; strOutcome = "true";}
          }
          if (iProperty == 7){ //Do not disturb
            strProperty = "bell-muted";
            if (iAction < 2) {muteBellEnabled = iAction;}
            if (iAction == 2) {muteBellEnabled = !muteBellEnabled;}
            strOutcome = (muteBellEnabled == true) ? "true" : "false";
          }
          if (iProperty == 9){ //Notify doorbell pressed
            strProperty = "doorbell-pressed-time";
            lastBellPressedDateTime = strDateTime;
            strProperty = lastRotaryInputDateTime;
          }

          if (Homie.isConnected()) {bellNode.setProperty(strProperty).send(strOutcome);}
          break;

          case 3: // rotary
            if (iProperty == 1){ // Rotary inpuy
              commandBuff.push(393); // Dorce update time
              strProperty = "rotary-input";
              strOutcome = iAction;
            }
            if (iProperty == 9){
              strProperty = "rotary-input-time";
              lastRotaryInputDateTime = strDateTime;
              strOutcome = lastRotaryInputDateTime;
            }
            if (Homie.isConnected()) {rotaryNode.setProperty(strProperty).send(strOutcome);}
          break;

          case 9: // System
          if (iProperty == 4){
            strProperty = "force-mqtt-update";
            if (iAction > 0) {
              strOutcome = "updating MQTT";
              newState = STATE_MQTT_PUBLISH_UPDATE;
            }
          }
          if (iProperty == 5){ // STATE_RESET
            strProperty = "set-debug";
            if (iAction > 0) {
              strOutcome = "Resetting now";
              newState = STATE_RESET;}
          }
          if (iProperty == 6){ // STATE_RESET
            strProperty = "set-debug";
            if (iAction < 2) {
              strOutcome = (debugEnabled == true ? "true" : "false");
              debugEnabled = iAction;
            }
          }

          if (iProperty == 9){ // Factory reset
            strProperty = "factory-reset";
            if (iAction == 9) {
              strOutcome = "Factory reset now";
              Homie.reset();
            }
          }
          if (Homie.isConnected()) {systemNode.setProperty(strProperty).send(strOutcome);}
        }
        break;
    }
}

void silenceBell() {
  bellTimer.detach();
  digitalWrite(HB_INA, LOW);
  digitalWrite(HB_INB, LOW);
}

void handler_ringer(void) {
  // Only do this when mute is disabled OR mute is enabled but auto open is engaged.
  if (muteBellEnabled == false || (muteBellEnabled == true && autoUnlockEnabled == true)) {
    toggle ^= 1;
    digitalWrite(LED_BUILTIN, toggle);
    digitalWrite(HB_INA, toggle);
    digitalWrite(HB_INB, !toggle);
  }
}
// Interrupt routines
void buzzerButtonInterrupt() { // Interrupt when button to open door is pressed
  buzzerPressedFlag = true;
}

void doorBellInterrupt() { // Interrupt when doorbel is pressed
    doorBellIntFlag = true;
}
