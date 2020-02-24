/*
TODO
- Re-arrange numbers /actions / topics
- Code cleanup
- Reset auto inlock for an hour or so
- JSON sstructures messages
- MOre config parameters
- Counters (in flash)
- Config in flash?
- NTP time
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
#define ROTARY_PULSE_PIN D1 // Input for the rotary dailer pulse pin (normally closed
#define ROTARY_READY_PIN D2 // Input pin for rotary dailer ready pin (normally open)

#define BAUDRATE 115200
#define RINGER_SLOW_PERIOD_MS 50 //  20hz = 50ms cycle, 25hz = 40ms
#define RINGER_FAST_PERIOD_MS 40 //  20hz = 50ms cycle, 25hz = 40ms
#define DOOR_UNLOCK_DURATION 150

#define CHECK_INTERVAL_MS 20
#define MIN_STABLE_VALS 6

//#define MQTT_UPDATE_INTERVAL_MS 30000

void silenceBell();
void handler_ringer(void);
void buzzerButtonInterrupt();
void doorBellInterrupt();
//void handleRoot();
//void wifiConnected();
//void configSaved();
//boolean formValidator();
//boolean connectMqtt();
//void mqttMessageReceived(String &topic, String &payload);

unsigned long prevIntCheckMillis;
char buzzerButtonStableVals, octoPinStableVals;

// Classes and things
WiFiUDP ntpUDP;
RotaryDialer dialer = RotaryDialer(ROTARY_READY_PIN, ROTARY_PULSE_PIN);
CircularBuffer<int, 10> serOutBuff;
CircularBuffer<int, 10> commandBuff;
Ticker bellTimer;
//StaticJsonDocument<200> doc;

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
  STATE_MQTT_UPDATE,
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

byte toggle = 0;
byte intToggle = 1;

unsigned long stateTime = 0;
unsigned long prevStateTime = 0;

boolean doorBellIntFlag = false;
boolean doorBellPressed = false;

boolean autoUnlockFlag = false;
boolean autoUnlockEnabled = false;

boolean buzzerPressedFlag = false;
boolean muteBellEnabled = false;

unsigned int dialerInput = 0;
unsigned int bellRepeats = 0;
boolean bellOn = false;

//Put ISR's in IRAM.
void ICACHE_RAM_ATTR buzzerButtonInterrupt();
void ICACHE_RAM_ATTR doorBellInterrupt();

const long utcOffsetInSeconds = 3600;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", utcOffsetInSeconds, 60000);
//WiFiClient net;

boolean needReset = false;
//int state = LOW;
//unsigned long lastAction = 0;


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
  timeClient.begin();
  DEBUG_PRINTLN("Setup finished");
}

void loop(void) {
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
        serOutBuff.clear();
        commandBuff.clear();
        // Configure timer
        // https://circuits4you.com/2018/01/02/esp8266-timer-ticker-example/
        //bellTimer.attach_ms(RINGER_PERIOD_MS, handler_ringer); // 20hz = 50ms cycle, 25hz = 40ms
        silenceBell();
        dialer.setup();
      break;

      // STATE 1 - Idle
      case STATE_IDLE:

        timeClient.update();

        if (needReset) {
          newState = STATE_RESET;
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

        // Debouncing of interrupts
        if ((stateTime - prevIntCheckMillis) > CHECK_INTERVAL_MS) {
          prevIntCheckMillis = stateTime;
          // Buzzerbutton debounce
          if ((digitalRead(BUZZER_BUTTON_PIN) == LOW) && (buzzerPressedFlag == true))
          {
            buzzerButtonStableVals++;
            if (buzzerButtonStableVals >= MIN_STABLE_VALS)
            {
              commandBuff.push(11); // Notify buzzer button pressed
              buzzerPressedFlag = false;
              newState = STATE_UNLOCK_DOOR;
              buzzerButtonStableVals = 0;
            }
          }
          else {
            buzzerButtonStableVals = 0;
          }
          // Octocoupler
          if ((digitalRead(OCTO_COUPLER_PIN) == LOW) && (doorBellIntFlag == true))
          {
            octoPinStableVals++;
            if (octoPinStableVals >= MIN_STABLE_VALS)
            {
              doorBellIntFlag = false;
              doorBellPressed = true;
              if (autoUnlockEnabled == true) {
                commandBuff.push(21); // Notify doorbellbutton is pressed
                commandBuff.push(51); // Open door
                commandBuff.push(22); // Notify auto unlock
              }
              else {
                commandBuff.push(21); // Notify doorbellbutton is pressed
              }
              commandBuff.push(61); // Ring bell
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
          case 9: // Switch auto open off
            commandBuff.push(80);
            commandBuff.push(41);
          break;

          case 6: // Ring bell
            commandBuff.push(61);
          break;
          case 8: // Switch auto open on
            commandBuff.push(81);
            commandBuff.push(41);
          break;
          default:
            commandBuff.push(90 +dialerInput);
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
        }
        if((stateTime - prevStateTime) >= DOOR_UNLOCK_DURATION) {
          digitalWrite(REED_RELAY_PIN, LOW);
          newState = STATE_IDLE;
        }

        if (doorBellPressed == true && autoUnlockEnabled == false) {
          commandBuff.push(20); // Notify that we answered the doorbell and buzzed vistor in.
        }

        prevState = STATE_UNLOCK_DOOR;
      break;

      //State 4 - Ring bell
      case STATE_RING_BELL:
        normalBellRing.onTimeMs = 1000;
        normalBellRing.offTimeMs = 500;
        normalBellRing.repeats = 3;

        if(prevState!=STATE_RING_BELL) { // First time here
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
      normalBellRing.onTimeMs = 250;
      if(prevState!=STATE_SHORT_RING_BELL) { // First time here
        bellOn = true;
        bellTimer.attach_ms(RINGER_FAST_PERIOD_MS, handler_ringer);
        bellRepeats = 1;
        prevStateTime = stateTime;
      }
      if (bellOn == true) {
        if((stateTime - prevStateTime) > normalBellRing.onTimeMs) {
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


      case STATE_MQTT_UPDATE:
        prevState = STATE_HANDLE_ACTION;
        newState = STATE_IDLE; // Don't get trapped in here
      break;

      // State 7 - Handle actio
      case STATE_HANDLE_ACTION:
        prevState = STATE_HANDLE_ACTION;
        newState = STATE_IDLE; // Don't get trapped in here
        unsigned int iCommand, iTopic, iAction, iOutcome;
        String strAction = "";
        String strTopic = "";
        String strOutcome = "";
        iCommand = commandBuff.shift();

        if(iCommand<10 || iCommand>99) {break;} // We are expecting two digits
        iTopic = (iCommand/10) % 10;
        iAction = (iCommand % 10);

        if (iTopic != 9) { // 9 is reservered for rotary output
          switch(iAction) {
            case 0:
              strAction = "OFF";
              break;
            case 1:
              strAction = "ON";
              break;
            case 2:
              strAction = "TOGGLE";
              break;
            default:
              strAction = "STATUS";
              iAction = 9;
              break;
          }
        }

        switch(iTopic) {
          case 1: // Doorbell pressed (MQTT update)
            strTopic = "BUZZER";
            if (iAction == 1) {strOutcome = "PRESSED";}
          break;

          case 2: // Doorbell pressed (MQTT update)
            strTopic = "DOORBELL";
            if (iAction == 0) {strOutcome = "ANSWERED";doorBellPressed = false;}
            if (iAction == 1) {strOutcome = "PRESSED";}
            if (iAction == 2) {strOutcome = "AUTO_ANSWERED";doorBellPressed = false;}
          break;

          case 3: // Reset device (action)
            strTopic = "RESET";
            if (iAction == 1) {newState = STATE_RESET; strOutcome = "TOGGLE";}
            if (iAction == 2) {newState = STATE_RESET; strOutcome = "TOGGLE";}
          break;

          case 4: // Unlock door (action)
            strTopic = "SHORT_RING";
            if (iAction == 1) {newState = STATE_SHORT_RING_BELL; strOutcome = "TOGGLE";}
            if (iAction == 2) {newState = STATE_SHORT_RING_BELL; strOutcome = "TOGGLE";}
            break;

          case 5: // Unlock door (action)
            strTopic = "UNLOCK";
            if (iAction == 1) {newState = STATE_UNLOCK_DOOR; strOutcome = "TOGGLE";}
            if (iAction == 2) {newState = STATE_UNLOCK_DOOR; strOutcome = "TOGGLE";}
            break;

          case 6: // Ring bell (action)
            strTopic = "RINGER";
            if (iAction == 1) {newState = STATE_RING_BELL; strOutcome = "TOGGLE";}
            if (iAction == 2) {newState = STATE_RING_BELL; strOutcome = "TOGGLE";}
            break;

          case 7: // Toggle do not disturb (switch)
            strTopic = "MUTE";
            if (iAction < 2) {muteBellEnabled = iAction;}
            if (iAction == 2) {muteBellEnabled = !muteBellEnabled;}
            iOutcome = muteBellEnabled;
            if (iOutcome == 0) {strOutcome = "OFF";}
            else if(iOutcome == 1) {strOutcome = "ON";}
            break;

          case 8: // Toggle auto unlock (switch)
            strTopic = "AUTO_UNLOCKER";
            if (iAction < 2) {autoUnlockEnabled = iAction;}
            if (iAction == 2) {autoUnlockEnabled = !autoUnlockEnabled;}
            iOutcome = autoUnlockEnabled;
            if (iOutcome == 0) {strOutcome = "OFF";}
            else if(iOutcome == 1) {strOutcome = "ON";}
            break;

          case 9: // Reset to default (action)
            strTopic = "ROTARY";
            strOutcome = iAction;
            break;
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
