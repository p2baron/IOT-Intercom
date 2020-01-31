/*
TODO
- Re-arrange numbers /actions / topics
- Code cleanup
- Reset auto inlock for an hour or so


Nice to have:
- Counter to stores number of rings and door opens
- NTP Time TIme??
- MQTT configurable variabels
  */

//Libs
#include <Arduino.h>
#include <RotaryDialer.h> // https://github.com/markfickett/Rotary-Dial
#include <CircularBuffer.h> // https://github.com/rlogiacco/CircularBuffer
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <MQTT.h>
#include <IotWebConf.h> // by Prampec https://github.com/prampec/IotWebConf

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

#define MQTT_UPDATE_INTERVAL_MS 30000

void silenceBell();
void handler_ringer(void);
void buzzerButtonInterrupt();
void doorBellInterrupt();
void handleRoot();
void wifiConnected();
void configSaved();
boolean formValidator();
boolean connectMqtt();
void mqttMessageReceived(String &topic, String &payload);

unsigned long prevIntCheckMillis;
char buzzerButtonStableVals, octoPinStableVals;

// Classes and things
RotaryDialer dialer = RotaryDialer(ROTARY_READY_PIN, ROTARY_PULSE_PIN);
CircularBuffer<int, 10> serOutBuff;
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


// Wifi stuff
const char thingName[] = "iot-intercom";
const char wifiInitialApPassword[] = "10doorbell10!";
#define STRING_LEN 128
#define CONFIG_VERSION "MQTT Intercom v3.20200131 ESP "
#define STATUS_PIN LED_BUILTIN

#define MQTT_ACTION_TOPIC_PREFIX "cmnd/"
#define MQTT_STATUS_TOPIC_PREFIX "stat/"
#define MQTT_TELEMETRY_TOPIC_PREFIX "tele/"

#define ACTION_FEQ_LIMIT 7000
#define NO_ACTION -1

// -- Callback method declarations.
void wifiConnected();
void configSaved();
boolean formValidator();
void mqttMessageReceived(String &topic, String &payload);

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiClient net;
MQTTClient mqttClient;

char mqttServerValue[STRING_LEN];
char mqttUserNameValue[STRING_LEN];
char mqttUserPasswordValue[STRING_LEN];
char mqttActionTopic[STRING_LEN];
char mqttStatusTopic[STRING_LEN];

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameter mqttServerParam = IotWebConfParameter("MQTT server", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfParameter mqttUserNameParam = IotWebConfParameter("MQTT user", "mqttUser", mqttUserNameValue, STRING_LEN);
IotWebConfParameter mqttUserPasswordParam = IotWebConfParameter("MQTT password", "mqttPass", mqttUserPasswordValue, STRING_LEN, "password");
boolean needMqttConnect = false;
boolean needReset = false;
unsigned long lastMqttConnectionAttempt = 0;
unsigned long lastMqttUpdateTimeMS = 0;
int needAction = NO_ACTION;
int state = LOW;
unsigned long lastAction = 0;


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
  // Webserver
  iotWebConf.addParameter(&mqttServerParam);
  iotWebConf.addParameter(&mqttUserNameParam);
  iotWebConf.addParameter(&mqttUserPasswordParam);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.setupUpdateServer(&httpUpdater);

  // -- Initializing the configuration.
  boolean validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
    mqttUserNameValue[0] = '\0';
    mqttUserPasswordValue[0] = '\0';
  }

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessage(mqttMessageReceived);
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
        iotWebConf.delay(1000);
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
        iotWebConf.doLoop();
        mqttClient.loop();
        if (needMqttConnect) {
          if (connectMqtt()) {
            needMqttConnect = false;
          }
        }
        else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqttClient.connected())) {
          connectMqtt();
        }

        if (mqttClient.connected()) {
            if ((stateTime - lastMqttUpdateTimeMS) > MQTT_UPDATE_INTERVAL_MS) {
              lastMqttUpdateTimeMS = stateTime;
              newState = STATE_MQTT_UPDATE;
            }
        }

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
        // Update status (announce  topics)
        commandBuff.push(19); // Buzzer
        commandBuff.push(29); // Doorbellbutton
        commandBuff.push(39); // Reset
        commandBuff.push(49); // Short ring
        commandBuff.push(59); // Unlock door
        commandBuff.push(69); // Ringer
        commandBuff.push(79); // Mute
        commandBuff.push(89); // Auto_unlocker
        commandBuff.push(90); // Rotary input
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
        if (mqttClient.connected()) {
          // -- Prepare dynamic topic names
          // /stat/iot-intercom/topic
          if (strOutcome.length() == 0) {
            strOutcome = "IDLE";
          }
          String temp = String(MQTT_STATUS_TOPIC_PREFIX);
          temp += iotWebConf.getThingName();
          temp += "/";
          temp += strTopic;
          temp.toCharArray(mqttStatusTopic, STRING_LEN);
          mqttClient.publish(mqttStatusTopic, strOutcome, true, 1);
          DEBUG_PRINT("MQTT Post to: ");
          DEBUG_PRINTLN(mqttStatusTopic);
          DEBUG_PRINT("Value: ");
          DEBUG_PRINTLN(strOutcome);
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

// Wifi and MQTT
/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = F("<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>");
  s += iotWebConf.getHtmlFormatProvider()->getStyle();
  s += "<title>IotWebConf 07 MQTT Relay</title></head><body>";
  s += iotWebConf.getThingName();
  s += "<div>State: ";
  s += (state == HIGH ? "ON" : "OFF");
  s += "</div>";
  s += "<button type='button' onclick=\"location.href='';\" >Refresh</button>";
  s += "<div>Go to <a href='config'>configure page</a> to change values.</div>";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void wifiConnected()
{
  needMqttConnect = true;
}

void configSaved()
{
  DEBUG_PRINTLN("Configuration was updated.");
  needReset = true;
}

boolean formValidator()
{
  DEBUG_PRINTLN("Validating form.");
  boolean valid = true;

  int l = server.arg(mqttServerParam.getId()).length();
  if (l < 3)
  {
    mqttServerParam.errorMessage = "Please provide at least 3 characters!";
    valid = false;
  }
  return valid;
}


boolean connectMqtt() {
  unsigned long now = millis();
  if (1000 > now - lastMqttConnectionAttempt)
  {
    // Do not repeat within 1 sec.
    return false;
  }
  DEBUG_PRINTLN("Connecting to MQTT server...");
  if (!mqttClient.connect(iotWebConf.getThingName())) {
    lastMqttConnectionAttempt = now;
    return false;
  }
  String temp = String(MQTT_ACTION_TOPIC_PREFIX);
  temp += iotWebConf.getThingName();
  temp += "/#";
  temp.toCharArray(mqttActionTopic, STRING_LEN);
  DEBUG_PRINTLN(mqttActionTopic);
  mqttClient.subscribe(mqttActionTopic);

  // Update status (announce  topics)
  newState = STATE_HANDLE_ACTION;

  DEBUG_PRINTLN("Connected!");
  return true;
}

void mqttMessageReceived(String &topic, String &payload)
{
  DEBUG_PRINTLN("Incoming: " + topic + " - " + payload);
  int iAction = 0;
  int iTopic = 0;

  if (topic.endsWith("UNLOCK")) {
    iTopic = 5;
  }

  if (topic.endsWith("SHORT_RING")) {
    iTopic = 4;
  }

  if (topic.endsWith("RINGER")) {
    iTopic = 6;
  }

  if (topic.endsWith("MUTE")) {
    iTopic = 7;
  }

  if (topic.endsWith("AUTO_UNLOCKER")) {
    iTopic = 8;
  }

  if (topic.endsWith("RESET")) {
    iTopic = 3;
  }

  if (topic.endsWith("CMND")) {
    DEBUG_PRINT("MQTT Integer command (CMND) with value: ");
    DEBUG_PRINT(payload);
    DEBUG_PRINT(" int: ");
    iAction = payload.toInt();
    DEBUG_PRINTLN(iAction);
  }

  if (iTopic >= 0) {
    if (payload.equals("OFF")) {iAction = iTopic * 10;}
    if (payload.equals("ON")) {iAction = (iTopic * 10) + 1;}
    if (payload.equals("TOGGLE")) {iAction = (iTopic * 10) + 2;}
  }
  if (iAction>=0) {
    commandBuff.push(iAction);
  }
}
