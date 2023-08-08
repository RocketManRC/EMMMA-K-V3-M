/* This sketch is enumerated as USB MIDI device. 
 * Following library is required
 * - MIDI Library by Forty Seven Effects
 *   https://github.com/FortySevenEffects/arduino_midi_library
 */

/*
  Have to remove this file from the project to get it to link:
    .pio/libdeps/ESP32-S3-DevKitC/Adafruit TinyUSB Library/src/portable/espressif/esp32sx/dcd_esp32sx.c

  Have to use these build flags in platformio.ini to get it to compile:
    build_flags =
     -DUSE_TINYUSB
     '-DCFG_TUSB_CONFIG_FILE="/Users/rick/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s23include/arduino_tinyusb/include/tusb_config.h"'

  This comes from this forum:
    https://community.platformio.org/t/tinyusb-definition-errors-on-esp32s3/29382

  There is a bug in the Adafruit library for MIDI pitch bend where it will only bend up. Need to 
  change line 343 in MIDI.hpp to:

  const int value = int(fabs(inPitchValue) * double(scale));

*/

/*
 Note about Serial0 - the pins for this port are connected to the CP2102
 USB to UART adapter via 0 ohm resistors. According to the datasheet
 for the CP2102 it is supposed to put it's RX and TX pins to high impedence
 if the USB isn't connected to anything. I found that I had to remove the
 resistors to get it to work. Also the system seems to be sending debug info
 to this port so you have to call Serial0.setDebugOutput(false); to stop that.
*/

#include <Arduino.h>
#include <LittleFS.h>
#include "driver/touch_pad.h"
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ArduinoJson.h>  

// forward references
void handleChangeRequest(uint8_t type, uint8_t data1, uint8_t data2);
void MPU6050Setup();
void MPU6050Loop();
void displayRefresh(); // Should displayMode() be used instead???
void displayMode();

// config default values
// to save the config update one or more of these values and call saveConfig();
String configInit = "3.1415926535"; // if this is changed config will be initialized (default 3.14159265359)
bool useBluetooth = false;
int scaleIndex = 3; // default is minor pentatonic
uint8_t midiChannel = 1;
int masterVolume = 127; 
bool adjacentPinsFilter = true;
bool dissonantNotesFilter = true;
uint8_t ccForModwheel = 1;
String broadcastAddressMidiHub = "123456";

bool optionsMode = true; // if true UI changes options (scale, key, etc), else UI changes config
bool wirelessChanged = false; // this will be set when the wireless mode changed causing a restart
bool enableAdjacentPins = false;
bool enableDissonantNotes = false;

float ypr[3];  // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

#define SCREEN_WIDTH 64  // OLED display width, in pixels
#define SCREEN_HEIGHT 128 // OLED display height, in pixels
#define OLED_RESET -1     // can set an oled reset pin if desired
Adafruit_SH1107 display = Adafruit_SH1107(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET, 1000000, 100000);

// ************ The following define should normally be 1! ************
#define USEMIDI 1 // set to 0 to force remote via ESP-Now

#define SERIALSLAVE Serial1
//#define SERIALEXTRA Serial0

//uint8_t broadcastAddressMidiHub[] = {0xDC, 0x54, 0x75, 0xC8, 0xED, 0xFC}; // (AtomS3Lite#3) 
//uint8_t broadcastAddressMidiHub[] = {0xB8, 0xD6, 0x1A, 0x5A, 0xC4, 0xFC}; // (Simple Synth) 
//uint8_t broadcastAddressMidiHub[] = {0x84, 0xF7, 0x03, 0xDC, 0xF6, 0xF8}; // (Green rocket window) MAC address of the "midi hub" for ESP-Now comms
//uint8_t broadcastAddressMidiHub[] = {0x84, 0xF7, 0x03, 0xDD, 0x36, 0x7A}; // (Blue rocket window) 84:F7:03:DD:36:7A
//uint8_t broadcastAddressMidiHub[] = {0x84, 0xF7, 0x03, 0xDD, 0x36, 0x54}; // (mine) 84:F7:03:DD:36:54
//uint8_t broadcastAddressMidiHub[] = {0xDC, 0x54, 0x75, 0xC8, 0xFA, 0x58}; // (AtomS3Lite#1) 
//uint8_t broadcastAddressMidiHub[] = {0xDC, 0x54, 0x75, 0xC8, 0x45, 0x6C}; // (AtomS3Lite#2) 
//uint8_t broadcastAddressMidiHub[] = {0x4C, 0x75, 0x25, 0xA6, 0xCA, 0xF8}; // (Atom Lite ESP-Now to serial MIDI) 
//uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Any device on this channel...
uint8_t broadcastAddressRgbMatrix[] = {0x4C, 0x75, 0x25, 0xA6, 0xD6, 0x34};   // The Atom Lite with RGB LED matrix

bool binding = false; // this will be set to true if middle not pin touched during startup

// I wanted to use the RGB LED to show note colours using Scriabin's Colour Sequence 
// but the Neopixel update is too slow and it adds random latency into the note generation.
// For now I just use the LED for status and error indications:
// GREEN - USB MIDI has successfully started.
// RED - ESP-Now fails initialization or packet failed to be delivered.
// BLUE - Using ESP-Now.

uint32_t espNowMicrosAtSend = 0;
uint32_t espNowReturnTime = 0;  // this will hold the return time of Esp_now in microseconds
uint8_t espNowDeliveryStatus = 0xFF;

bool bluetoothConnected = false; // will be set when bluetooth connected

#define RGBLED 0 // set to 1 to show note colours 

//Adafruit_NeoPixel pixels(1, 18, NEO_GRB + NEO_KHZ800); // ESP32-S2 DevKitC
Adafruit_NeoPixel pixels(1, 48, NEO_GRB + NEO_KHZ800); // ESP32-S3 DevKitC

const int MPU_addr = 0x68; // for the 6050

uint32_t scriabinColourSequence[12] = {0xFF0000, 0xCD9AFF, 0xFFFF00, 0x656599, 0xE3FBFF, 0xAC1C02, 0x00CCFF,
  0xFF6501, 0xFF00FF, 0x33CC33, 0x8C8A8C, 0x0000FE};

void handleNoteOn(byte channel, byte pitch, byte velocity);
void handleNoteOff(byte channel, byte pitch, byte velocity);

// USB MIDI object
Adafruit_USBD_MIDI usb_midi;

// BLE MIDI
BLEMIDI_CREATE_INSTANCE("EMMMA-K", MIDI)    

// Create a new instance of the Arduino MIDI Library,
// and attach usb_midi as the transport.
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, USBMIDI);

bool midiOn = false;

int key  = 0;
int octave = -1;
int scale = 0;

const uint8_t numPins = 14;  // The MCU has 14 touch pins
const uint8_t notePins = 9;  // and the master has 9 note pins

static const touch_pad_t pins[numPins] = 
{
    TOUCH_PAD_NUM4,
    TOUCH_PAD_NUM5,
    TOUCH_PAD_NUM6,
    TOUCH_PAD_NUM9,
    TOUCH_PAD_NUM10,
    TOUCH_PAD_NUM11,
    TOUCH_PAD_NUM12,
    TOUCH_PAD_NUM13,
    TOUCH_PAD_NUM14,
    TOUCH_PAD_NUM7,
    TOUCH_PAD_NUM8,
    TOUCH_PAD_NUM3,
    TOUCH_PAD_NUM1,
    TOUCH_PAD_NUM2
};

static uint32_t benchmark[numPins]; // to store the initial touch values of the pins

// notes (17 total) and scales (choose one)
uint8_t majorscale[] = {2, 2, 1, 2, 2, 2, 1}; // case 1
uint8_t minorscale[] = {2, 1, 2, 2, 1, 2, 2}; // case 2
uint8_t pentascale[] = {2, 2, 3, 2, 3}; // case 3
uint8_t minorpentascale[] = {3, 2, 2, 3, 2};  // case 4
uint8_t minorbluesscale[] = {3, 2, 1, 1, 3, 2}; // case 5

uint8_t majorbluesscale[] = {2, 1, 1, 3, 2, 3}; // case 6
uint8_t minorharmonic[] = {2, 1, 2, 2, 1, 3, 1};  // case 7
uint8_t minormelodic[] = {2, 1, 2, 2, 2, 2, 1}; // case 8
uint8_t minorpo33[] = {2, 1, 2, 2, 1, 2, 1, 1}; // case 9

uint8_t dorian[] = {2, 1, 2, 2, 2, 1, 2}; // case 10
uint8_t phrygian[] = {1, 2, 2, 2, 1, 2, 2}; // case 11
uint8_t lydian[] = {2, 2, 2, 1, 2, 2, 1};  // case 12
uint8_t mixolydian[] = {2, 2, 1, 2, 2, 1, 2}; // case 13
uint8_t aeolian[] = {2, 1, 2, 2, 1, 2, 2};  // case 14
uint8_t locrian[] = {1, 2, 2, 1, 2, 2, 2};  // case 15
uint8_t lydiandomiant[] = {2, 2, 2, 1, 2, 1, 2};  // case 16
uint8_t superlocrian[] = {1, 2, 1, 2, 2, 2, 2}; // case 17

uint8_t wholehalfdiminished[] = {2, 1, 2, 1, 2, 1, 2, 1}; // case 18
uint8_t halfwholediminished[] = {1, 2, 1, 2, 1, 2, 1, 2}; // case 19

const int totalNotePins = 17; // 17 note pins on the keyboard itself 
const int totalNotes = 24;    // 17 notes plus room for chord notes above the last one including inversions

bool notePinsOn[totalNotePins] = {false};
uint8_t midiValues[totalNotes]; 

#define OCTAVE 12
#define KEY G - OCTAVE // choose desired key and octave offset here

bool option1 = false; // right top (on PCB) option pin (pitchbend)
bool option2 = false; // right middle (on PCB) option pin (mode value up)
bool option3 = false; // right bottom (on PCB) option pin (mode value down)
bool option4 = false; // left top (on PCB) option pin (3 functions: change relative scale, enable/disable chords, modwheel)
bool option5 = false; // left middle (on PCB) option pin (not used)
bool option6 = false; // left bottom (on PCB) option pin (2 functions: change mode and change config)

void displayValue(String title, String value);
String keyNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

String mode = "Scale";
String scales[] = {"Major", "Minor", "Major Pentatonic", "Minor Pentatonic", 
  "Major Blues", "Minor Blues", "Minor Harmonic", "Minor Melodic", "Minor PO-33", "Dorian",
  "Phrygian", "Lydian", "Mixolydian", "Aeolian", "Locrian",
  "Lydian Dominished", "Super Locrian", "Whole Half Dim", "Half Whole Dim"};

int scaleCount = sizeof(scales)/sizeof(scales[0]); 

bool playChords = false;

bool notePlayedWhileOption4Touched = false;

//String config = "Adjacent Key Filt";
uint8_t config = 0;
String configs[] = {"Adjacent Pin Filt", "Dissnt Notes Filt", "MIDI Channel", "Master Volume",
  "CC for Modwheel", "Wireless Mode", "Save & Exit", "Exit NO Save"};
uint8_t numberOfConfigItems = sizeof(configs)/sizeof(configs[0]);
void displayAdjacentPinFilt();
void displayDissonantNotesFilt();
void displayMidiChannel();
void displayMasterVolume();
void displayCcForModwheel();
void displayWirelessMode();
void displaySaveExitPrompt();
void displayExitNoSavePrompt();
void (*configDisplayFunctions[])() = {displayAdjacentPinFilt, displayDissonantNotesFilt, displayMidiChannel, displayMasterVolume,
  displayCcForModwheel, displayWirelessMode, displaySaveExitPrompt, displayExitNoSavePrompt};
void changeAdjacentPinFilt(bool up);
void changeDissonantNotesFilt(bool up);
void changeMidiChannel(bool up);
void changeMasterVolume(bool up);
void changeCcForModwheel(bool up);
void changeWirelessMode(bool up);
void saveExitConfig(bool up);
void exitNoSaveConfig(bool up);
void saveConfig();
void (*configChangeFunctions[])(bool) = {changeAdjacentPinFilt, changeDissonantNotesFilt, changeMidiChannel, changeMasterVolume,
  changeCcForModwheel, changeWirelessMode, saveExitConfig, exitNoSaveConfig};


void displayValue(String title, String value)
{
  display.clearDisplay();
  display.setCursor(15,20);
  display.print(title + ":");
  if(value.length() > 15)
    display.setCursor(15, 40);
  else
    display.setCursor(30, 40);
  display.print(value);
  display.display();
}

void displayScale()     
{
  displayValue(String("SCALE") + " " + String(scaleIndex + 1) + "/" + String(scaleCount), String(scales[scaleIndex]));
}

void displayKey()
{
  displayValue(String("KEY"), keyNames[key]);
}

void displayOctave()
{
  displayValue(String("OCTAVE"), String(octave));
}

void displayNotes(bool init)
{
  if(init)
  {
    displayValue(String("NOTES"), "none");
  }
  else if(mode == "Note")
  {
    String noteNames = "";

    for(int i = 0; i < totalNotePins; i++)
    {
      if(notePinsOn[i])
      {
        uint8_t midiValue = midiValues[i] + octave * 12 + key;
        uint8_t idx = midiValue % 60;
        noteNames += String(keyNames[idx % 12]) + String(midiValue/12 - 2);

        if(i < totalNotePins - 1)
          noteNames += " ";
      }
    }

    if(noteNames != "") // Only display elapsed time if notes to display
    {
      if(espNowReturnTime) // are we using wireless (espNowReturnTime is nonzero)?
      {
        if(espNowReturnTime == 0xFFFFFFFF)
          noteNames += " Fail!";
        else
          noteNames += " " + String(espNowReturnTime);
      }
    }

    displayValue(String("NOTES"), noteNames);
    //Serial.println(espNowReturnTime);
  } 
}

//void displayConfigPrompt()
//{
//  displayValue(String("Enter Config"), String("->"));
//}  

void displayAdjacentPinFilt()     
{
  if(adjacentPinsFilter)
    displayValue(String(configs[config]), String(" On"));
  else
    displayValue(String(configs[config]), String(" Off"));
}

void displayDissonantNotesFilt()     
{
  if(dissonantNotesFilter)
    displayValue(String(configs[config]), String(" On"));
  else
    displayValue(String(configs[config]), String(" Off"));
}

void displayMidiChannel()     
{
  displayValue(String(configs[config]), String(" ") + String(midiChannel));
}

void displayMasterVolume()     
{
  displayValue(String(configs[config]), String(" ") + String(masterVolume));
}

void displayCcForModwheel()     
{
  displayValue(String(configs[config]), String(" ") + String(ccForModwheel));
}

void displayWirelessMode()     
{
  if(useBluetooth)
    displayValue(String(configs[config]), String(" ") + "BLE");
  else
    displayValue(String(configs[config]), String(" ") + "ESP-Now");
}

void displaySaveExitPrompt()
{
  displayValue(String("Save & Exit"), String("->"));
}  

void displayExitNoSavePrompt()
{
  displayValue(String("Exit NO Save"), String("->"));
}  

bool chordSupported()
{
  String s = String(scales[scaleIndex]);

  bool result = (s == "Major") || (s == "Minor") || (s == "Major Pentatonic") ||
    (s == "Minor Pentatonic") || (s == "Minor Blues");

  return result;
}

void displayChords(bool init)
{
  if(init)
  {
    displayValue(String("CHORDS"), "none");
  }
  else if(mode == "Note")
  {
    String keyName = keyNames[key];
  
    String chordName = scales[scaleIndex];

    if(chordName == "Major Pentatonic") // Shorten names that are too long...
      chordName = "Major Penta";
    else if(chordName == "Minor Pentatonic")
      chordName = "Minor Penta";

    chordName = keyName + chordName + " ";

    String chordNames = "";

    for(int i = 0; i < totalNotePins; i++)
    {
      if(notePinsOn[i])
      {
        uint8_t midiValue = midiValues[i] + octave * 12 + key;
        uint8_t idx = midiValue % 60;
        chordNames += String(keyNames[idx % 12]) + String(midiValue/12 - 2);

        if(i < totalNotePins - 1)
          chordNames += " ";
      }
    }

    if(chordNames != "")
      displayValue(String("CHORDS"), chordName + chordNames);
    else
     displayValue(String("CHORDS"), "");
   }
}

const uint16_t displaySize =  8 * SCREEN_WIDTH * ((SCREEN_HEIGHT + 7) / 8);
uint8_t displaySaveBuffer[displaySize];
uint8_t messageSaveBuffer[displaySize];

void messageUpdate(bool init) // call this with init=false from loop()...
{
  static bool displaying = false;
  static uint32_t delayTime;
  static uint8_t saveBuffer[displaySize];

  if(init)
  {
    displaying = true;
    delayTime = millis();
  }
  else if(displaying)
  {
    if(millis() - delayTime > 2000)
    {
      uint8_t *displayBuffer = (uint8_t *)display.getBuffer();

      // check and see if display buffer has changed (user may have done something)
      memcpy(saveBuffer, displayBuffer, displaySize);

      bool changed = false;

      for(int i = 0; i < displaySize; i++)
      {
        if(messageSaveBuffer[i] != saveBuffer[i])
        {
          changed = true;
        }
      }

      // only restore the old display if it hasn't changed
      if(!changed)
      {
        display.clearDisplay(); // need to do this to mark display dirty
        memcpy(displayBuffer, displaySaveBuffer, displaySize);
        display.display();
      }

      displaying = false;
    }
  }
}

void displayMessage(String message)
{
  uint8_t *displayBuffer = (uint8_t *)display.getBuffer();
  memcpy(displaySaveBuffer, displayBuffer, displaySize);
  
  display.clearDisplay();
  display.setCursor(10,20);
  display.print(message);
  display.display();
  
  memcpy(messageSaveBuffer, displayBuffer, displaySize); // so we see if user changed the display

  messageUpdate(true);
}

void displayMode()
{
  if(mode == "Key")
  {
    displayKey();
  }
  else if(mode == "Octave")
  {
    displayOctave();
  }
  else if(mode == "Note")
  {
    if(playChords && chordSupported())
      displayChords(true);
    else
      displayNotes(true);
  }
  //else if(mode == "Config")
  //{
  //  displayConfigPrompt();
  //}
  else if(mode == "Scale")
  {
    displayScale();
  }
}

void changeMode()
{
  if(mode == "Scale")
  {
    mode = "Key";
  }
  else if(mode == "Key")
  {
    mode = "Octave";
  }
  else if(mode == "Octave")
  {
    mode = "Note";
  }
  else if(mode == "Note")
  {
    mode = "Scale";
  }
  //else if(mode == "Config")
  //{
  //  mode = "Scale";
  //}

  displayMode();
}

void changeScale(bool up)
{
  if(up)
  {
    if(scaleIndex == scaleCount - 1)
      scaleIndex = 0;
    else
      scaleIndex++;
  }
  else
  {
    if(scaleIndex == 0)
      scaleIndex = scaleCount - 1;
    else
      scaleIndex--;
  }
  
  handleChangeRequest(176, 68, scaleIndex + 1);
}

void changeKey(bool up)
{
  if(up)
  {
    if(key == 11)
      key = 0;
    else
      key++;
  }
  else
  {
    if(key == 0)
      key = 11;
    else
      key--;
  }
}

void changeOctave(bool up)
{
  if(up)
  {
    if(octave == 5)
      octave = -5;
    else
      octave++;
  }
  else
  {
    if(octave == -5)
      octave = 5;
    else
      octave--;
  }
}

void displayConfig()
{
#if 0
  if(config == "Adjacent Key Filt")
    displayAdjacentPinFilt();
  else if(config == "Dissnt Notes Filt")
    displayDissonantNotesFilt();
  else if(config == "MIDI Channel")
    displayMidiChannel();
  else if(config == "Exit")
    displayExitPrompt();
#endif

  (*configDisplayFunctions[config])();
}

void changeConfig()
{
#if 0
  if(config == "Adjacent Key Filt")
    config = "Dissnt Notes Filt";
  else if(config == "Dissnt Notes Filt")
    config = "MIDI Channel";
  else if(config == "MIDI Channel")
    config = "Exit";
  else if(config == "Exit")
    config = "Adjacent Key Filt";
#endif
  if(config >= numberOfConfigItems - 1)
    config = 0;
  else
    config++;

  displayConfig();
}

void changeAdjacentPinFilt(bool up)
{
  if(adjacentPinsFilter)
    adjacentPinsFilter = false;
  else
    adjacentPinsFilter = true;
}

void changeDissonantNotesFilt(bool up)
{
  if(dissonantNotesFilter)
    dissonantNotesFilter = false;
  else
    dissonantNotesFilter = true;
}

void changeMidiChannel(bool up)
{
  if(up)
  {
    if(midiChannel == 16)
      midiChannel = 1;
    else
      midiChannel++;
  }
  else
  {
    if(midiChannel == 1)
      midiChannel = 16;
    else
      midiChannel--;
  }
}

void changeMasterVolume(bool up)
{
  //Serial.println(up);
  //Serial.println(masterVolume);

  if(up)
  {
    if(masterVolume >= 127)
      masterVolume = 7;
    else
      masterVolume += 5;
  }
  else
  {
    if(masterVolume < 7)
      masterVolume = 127;
    else
      masterVolume -= 5;
  }
}

void changeCcForModwheel(bool up)
{
  if(up)
  {
    if(ccForModwheel >= 127)
      ccForModwheel = 1;
    else
      ccForModwheel++;
  }
  else
  {
    if(ccForModwheel <= 1)
      ccForModwheel = 127;
    else
      ccForModwheel--;
  }
}

void changeWirelessMode(bool up)
{
  if(useBluetooth)
    useBluetooth = false;
  else
    useBluetooth = true;

  wirelessChanged = true; // this is going to cause a reset whether or not the config is saved!
}

void saveExitConfig(bool up)
{
  // save config here
  saveConfig();

  // exit
  optionsMode = true;

  config = 0; // so we get the start of config next time...

  displayMode();

  if(wirelessChanged) // need to reboot if wireless was changed whether or not the config was saved
    ESP.restart();
}

void exitNoSaveConfig(bool up)
{
  // exit without saving
  // This still changes the config but doesn't save it in flash
  optionsMode = true;

  config = 0; // so we get the start of config next time...

  displayMode();

  if(wirelessChanged) // need to reboot if wireless was changed whether or not the config was saved
    ESP.restart();
}

void scaleToMidiValues(uint8_t *scale, uint8_t size)
{
  midiValues[0] = 60; // Scales tables always start at middle C thus the first value is 60

  for(int i = 1;  i < totalNotes; i++)
    midiValues[i] = midiValues[i - 1] + scale[(i - 1) % size];
}

void playMidiValues()
{
  for(int i = 0;  i < 17; i++)
  {
    USBMIDI.sendNoteOn(midiValues[i] +  key + octave * 12, 0, 1); 
    delay(60);  
    USBMIDI.sendNoteOff(midiValues[i] +  key + octave * 12, 0, 1); 
    delay(60);
  }
}

void changeKey(int value)
{
  key = value; 
}

void changeOctave(int value)
{
  octave = value; 
}

int wirelessSend(uint8_t *incomingData, int len)
{
  if(useBluetooth)
  {
    //Serial.println("Bluetooth send...");

    // The first type of packet is 4, 8 or 12 bytes for notes and chords. The first is the MIDI note value, the second is a flag 
    // for note on or off, the third is the volume and the fourth is the MIDI channel.
    // If it is a 9 byte packet (for a double) it is a pitch bend with the 9th byte being MIDI channel.
    // A 3 byte packet is for CCs with the first being the CC number, the second the value
    // and the third the MIDI channel

    if(bluetoothConnected)
    {
      if(len == 4 || len == 8 || len == 12) // Notes
      {
        if(incomingData[1]) // first note
        {
          MIDI.sendNoteOn(incomingData[0], incomingData[2], incomingData[3]);
        }
        else
        {
          MIDI.sendNoteOff(incomingData[0], incomingData[2], incomingData[3]);
        }
        
        if(len == 8 || len == 12) // second note if any
        {
          if(incomingData[5])
          {
            MIDI.sendNoteOn(incomingData[4], incomingData[6], incomingData[7]);
          }
          else
          {
            MIDI.sendNoteOff(incomingData[4], incomingData[6], incomingData[7]);
          }
        }
        
        if(len == 12) // third note if any
        {
          if(incomingData[9])
          {
            MIDI.sendNoteOn(incomingData[8], incomingData[10], incomingData[11]);
          }
          else
          {
            MIDI.sendNoteOff(incomingData[8], incomingData[10], incomingData[11]);
          }
        }
      }
      else if(len == 9) // Pitch bend
      {
        double bendData;
        uint8_t *dp = (uint8_t *)&bendData;
        for(int i = 0; i < 8; i++)
        {
          dp[i] = incomingData[i];
        }

        MIDI.sendPitchBend(bendData, incomingData[8]);
      }
      else if(len == 3) // CC
      {
        MIDI.sendControlChange(incomingData[0], incomingData[1], incomingData[2]);
      }
    }

    return 0;
  }
  else
  {
    // Use esp_now...
    return esp_now_send(0, incomingData, len);
  }
}

void pitchBend(double bendX)
{
  static bool bendActive = false;

  if(midiOn)
  {
    if(option1)
    {
      //Serial.print(bendX);
      //Serial.print(" ");
    
      USBMIDI.sendPitchBend(bendX, midiChannel);
      //if(bendX >= 0)
      //  USBMIDI.sendControlChange(1, bendX * 127, midiChannel); // test CC, must be 0 - 127
      bendActive = true;
    }
    else if(bendActive)
    {
      // Send pitchbend of 0 once when option1 removed
      USBMIDI.sendPitchBend(0.0, midiChannel);
      //USBMIDI.sendControlChange(1, 0, midiChannel); // test CC
      bendActive = false;
    }
  }
  else
  {
    if(option1)
    {
      //Serial.printf("%f \n", bendX);

      uint8_t *p =  (uint8_t *)&bendX; 
      uint8_t msgPitchbend[9];

      for(int i = 0; i < 8; i++)
      {
        msgPitchbend[i] = p[i];
      }

      msgPitchbend[8] = midiChannel;  // MIDI channel

      espNowMicrosAtSend = micros();
      esp_err_t outcome = wirelessSend((uint8_t *) &msgPitchbend, sizeof(msgPitchbend));  
      bendActive = true;
    }
    else if(bendActive)
    {
      // Send pitchbend of 0 once when option1 removed
      double zero = 0.0;
      uint8_t *p =  (uint8_t *)&zero; 
      uint8_t msgPitchbend[9];

      for(int i = 0; i < 8; i++)
        msgPitchbend[i] = p[i];
        
      msgPitchbend[8] = midiChannel;  // MIDI channel

      esp_err_t outcome = wirelessSend((uint8_t *) &msgPitchbend, sizeof(msgPitchbend));  
      bendActive = false;
    }
  }
}

void modwheel(uint8_t modX)
{
  static bool modActive = false;

  if(midiOn)
  {
    if(option1)
    {
      //Serial.print(bendX);
      //Serial.print(" ");
    
      if(modX >= 0)
      {
        USBMIDI.sendControlChange(ccForModwheel, modX, midiChannel); // CC, must be 0 - 127

        modActive = true;
      }
    }
    else if(modActive)
    {
      // Send modwheel of 0 once when option4 removed
      USBMIDI.sendControlChange(ccForModwheel, 0, midiChannel); 

      modActive = false;
    }
  }
  else
  {
    uint8_t msgModwheel[3];

    if(option1)
    {
      //Serial.printf("%f \n", bendX);

      uint8_t mod = modX;

      msgModwheel[0] = ccForModwheel;

      msgModwheel[1] = mod;

      msgModwheel[2] = midiChannel;  // MIDI channel

      espNowMicrosAtSend = micros();
      esp_err_t outcome = wirelessSend((uint8_t *) &msgModwheel, sizeof(msgModwheel));  
      modActive = true;
    }
    else if(modActive)
    {
      // Send modwheel of 0 once when option1 removed
        
      msgModwheel[0] = ccForModwheel;

      msgModwheel[1] = 0;

      msgModwheel[2] = midiChannel;  // MIDI channel

      esp_err_t outcome = wirelessSend((uint8_t *) &msgModwheel, sizeof(msgModwheel));  
      modActive = false;
    }
  }
}

void setVolume(uint8_t data1)
{
  //usbMIDI.sendAfterTouch(data1, 1);
  masterVolume = data1;
}

void handleChangeRequest(uint8_t type, uint8_t data1, uint8_t data2)
{
  //Serial.printf("%d %d %d\n", type, data1, data2);

  if(type == 176 && data1 == 68) // is this control change #68?
  {
    // if so data2 contains the scale: 1 to number of scales 
    scaleIndex = data2 - 1; // scaleIndex won't be set properly if called from external MIDI or ESP-Now...

    switch(data2)
    {
      case 1:
      scaleToMidiValues(majorscale, sizeof(majorscale));
      //Serial3.printf("Change scale to: %s\n", "majorscale");
      break;
        
      case 2:
      scaleToMidiValues(minorscale, sizeof(minorscale));
      //Serial3.printf("Change scale to: %s\n", "minorscale");
      break;
        
      case 3:
      scaleToMidiValues(pentascale, sizeof(pentascale));
      //Serial3.printf("Change scale to: %s\n", "pentascale");
      break;
        
      case 4:
      scaleToMidiValues(minorpentascale, sizeof(minorpentascale));
      //Serial3.printf("Change scale to: %s\n", "minorpentascale");
      break;
        
      case 5:
      scaleToMidiValues(majorbluesscale, sizeof(majorbluesscale));
      //Serial3.printf("Change scale to: %s\n", "minorbluesscale");
      break;        

      case 6:
      scaleToMidiValues(minorbluesscale, sizeof(minorbluesscale));
      //Serial3.printf("Change scale to: %s\n", "majorbluesscale");
      break;        

      case 7:
      scaleToMidiValues(minorharmonic, sizeof(minorharmonic));
      //Serial3.printf("Change scale to: %s\n", "minorharmonic");
      break;        

      case 8:
      scaleToMidiValues(minormelodic, sizeof(minormelodic));
      //Serial3.printf("Change scale to: %s\n", "minormelodic");
      break;        

      case 9:
      scaleToMidiValues(minorpo33, sizeof(minorpo33));
      //Serial3.printf("Change scale to: %s\n", "minorpo33");
      break;        

      case 10:
      scaleToMidiValues(dorian, sizeof(dorian));
      //Serial3.printf("Change scale to: %s\n", "dorian");
      break;        

      case 11:
      scaleToMidiValues(phrygian, sizeof(phrygian));
      //Serial3.printf("Change scale to: %s\n", "phrygian");
      break;        

      case 12:
      scaleToMidiValues(lydian, sizeof(lydian));
      //Serial3.printf("Change scale to: %s\n", "lydian");
      break;        

      case 13:
      scaleToMidiValues(mixolydian, sizeof(mixolydian));
      //Serial3.printf("Change scale to: %s\n", "mixolydian");
      break;        

      case 14:
      scaleToMidiValues(aeolian, sizeof(aeolian));
      //Serial3.printf("Change scale to: %s\n", "aeolian");
      break;        

      case 15:
      scaleToMidiValues(locrian, sizeof(locrian));
      //Serial3.printf("Change scale to: %s\n", "locrian");
      break;        

      case 16:
      scaleToMidiValues(lydiandomiant, sizeof(lydiandomiant));
      //Serial3.printf("Change scale to: %s\n", "lydiandomiant");
      break;        

      case 17:
      scaleToMidiValues(superlocrian, sizeof(superlocrian));
      //Serial3.printf("Change scale to: %s\n", "superlocrian");
      break;        

      case 18:
      scaleToMidiValues(wholehalfdiminished, sizeof(wholehalfdiminished));
      //Serial3.printf("Change scale to: %s\n", "wholehalfdiminished");
      break;        

      case 19:
      scaleToMidiValues(halfwholediminished, sizeof(halfwholediminished));
      //Serial3.printf("Change scale to: %s\n", "halfwholediminished");
      break;        

      default:
        break;  
    }
  }
  else if(type == 176 && data1 == 69) // is this control change #69?
  {
    playMidiValues(); // If so play the current scale
  }
  else if(type == 176 && data1 == 70) // is this control change #70?
  {
    changeKey(data2); // If so change the key
  }
  else if(type == 176 && data1 == 71) // is this control change #71?
  {
    changeOctave(data2); // If so play change the octaveo
  }
#if 0 // Don't allow external pitch bend or volume change for now
  else if(type == 224) // is this a pitchbend message?
  {
    pitchBend(data1, data2); // If so do a pitch bend
  }
  else if(type == 208) // is this an aftertouch message?
  {
    setVolume(data1);
  }    
#endif
}

// This is the callback for ESP-Now success/failure
void data_sent(const uint8_t *mac_addr, esp_now_send_status_t status) 
{
  uint32_t m = micros();
  espNowReturnTime = m - espNowMicrosAtSend; // this is how many us it takes to get back the Esp-Now reply
  //Serial.println(m - espNowMicrosAtSend);

  //Serial.print("\r\nStatus of Last Message Sent:\t");
  //Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");

  if(status) // did delivery fail?
  {
    // check to see if this is the rgb matrix
    bool isRbgMatrix = true;
    for(int i = 0; i < 6; i++)
    {
      if(mac_addr[i] != broadcastAddressRgbMatrix[i])
        isRbgMatrix = false;
    }

    if(isRbgMatrix)
    {
      // remove the rgb matrix from the sender list
      esp_now_del_peer(broadcastAddressRgbMatrix);
    }
    else
    {
      pixels.setPixelColor(0, 0xFF0000); // set LED to red
      pixels.show(); 
      espNowReturnTime = 0xFFFFFFFF; // to flag an error on the note display
    }
  }
}

void data_received(const uint8_t *mac_addr, const uint8_t *data, int data_len)
{
  //for(int i = 0; i < data_len; i++)
  //{
    //Serial.print(data[i]);
    //Serial.print(" ");
  //}

  //Serial.println();

  //Serial.printf("%X %X %X %X %X %X\n", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

  if(binding) // are we waiting for the hub to bind?
  //if(memcmp(mac_addr, broadcastAddressMidiHub.c_str(), 6)) // are we potentially getting a broadcast from a new hub?
  {
    // check if there is a valid id in the data
    const uint8_t id[] = {'E', 'M', 'M', 'M', 'A', '-', 'K'};

    if(data_len == sizeof(id) && !memcmp(id, data, data_len))
    {
      // initialize hub address, save config and reboot...
      memcpy((void *)broadcastAddressMidiHub.c_str(), mac_addr, 6);

      saveConfig();

      ESP.restart();
    }
  }

  handleChangeRequest(data[0], data[1], data[2]);

  displayRefresh();
}

#if 0 // choose I2C channel, 1 = Wire, 0 = Wire1
void init6050()
{
  Wire.setPins(35, 36); // SDA, SCL
  Wire.begin(); 
  Wire.beginTransmission(MPU_addr);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);
}

void read6050(double *x, double *y, double *z)
{
  int16_t AcX, AcY, AcZ, Tmp, GyX, GyY, GyZ;
  
  int minVal=265;
  int maxVal=402;

  Wire.beginTransmission(MPU_addr);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_addr, 14, (int)true);

  AcX = Wire.read()<<8|Wire.read();
  AcY = Wire.read()<<8|Wire.read();
  AcZ = Wire.read()<<8|Wire.read();

  int xAng = map(AcX,minVal,maxVal,-90,90);
  int yAng = map(AcY,minVal,maxVal,-90,90);
  int zAng = map(AcZ,minVal,maxVal,-90,90);
  
  *x = RAD_TO_DEG * (atan2(-yAng, -zAng)+PI);
  *y = RAD_TO_DEG * (atan2(-xAng, -zAng)+PI);
  *z = RAD_TO_DEG * (atan2(-yAng, -xAng)+PI);  
}
#endif

#if 0
void init6050()
{
  Wire1.setPins(41, 42); // SDA, SCL
  Wire1.begin(); 
  Wire1.beginTransmission(MPU_addr);
  Wire1.write(0x6B);
  Wire1.write(0);
  Wire1.endTransmission(true);
}

void read6050(double *x, double *y, double *z)
{
  int16_t AcX, AcY, AcZ, Tmp, GyX, GyY, GyZ;
  
  int minVal=265;
  int maxVal=402;

  Wire1.beginTransmission(MPU_addr);
  Wire1.write(0x3B);
  Wire1.endTransmission(false);
  Wire1.requestFrom(MPU_addr, 14, (int)true);

  AcX = Wire1.read()<<8|Wire1.read();
  AcY = Wire1.read()<<8|Wire1.read();
  AcZ = Wire1.read()<<8|Wire1.read();

  int xAng = map(AcX,minVal,maxVal,-90,90);
  int yAng = map(AcY,minVal,maxVal,-90,90);
  int zAng = map(AcZ,minVal,maxVal,-90,90);
  
  *x = RAD_TO_DEG * (atan2(-yAng, -zAng)+PI);
  *y = RAD_TO_DEG * (atan2(-xAng, -zAng)+PI);
  *z = RAD_TO_DEG * (atan2(-yAng, -xAng)+PI);  
}
#endif

// LittleFS calls for config
void writeFile(String filename, String message)
{
    File file = LittleFS.open(filename, "w");

    if(!file)
    {
      Serial.println("writeFile -> failed to open file for writing");
      return;
    }

    if(file.print(message))
    {
      Serial.println("File written");
    } 
    else 
    {
      Serial.println("Write failed");
    }

    file.close();
}

String readFile(String filename)
{
  File file = LittleFS.open(filename);

  if(!file)
  {
    Serial.println("Failed to open file for reading");
    return "";
  }
  
  String fileText = "";
  while(file.available())
  {
    fileText = file.readString();
  }

  file.close();

  return fileText;
}

const String config_filename = "/config.json";

void saveConfig() 
{
  StaticJsonDocument<1024> doc;

  // write variables to JSON file
  doc["configInit"] = configInit;
  doc["useBluetooth"] = useBluetooth;
  doc["scaleIndex"] = scaleIndex;
  doc["midiChannel"] = midiChannel;
  doc["masterVolume"] = masterVolume;
  doc["adjacentPinsFilter"] = adjacentPinsFilter;
  doc["dissonantNotesFilter"] = dissonantNotesFilter;
  doc["ccForModwheel"] = ccForModwheel;
  doc["broadcastAddressMidiHub"] = broadcastAddressMidiHub;
  
  // write config file
  String tmp = "";
  serializeJson(doc, tmp);
  writeFile(config_filename, tmp);
}

bool readConfig() 
{
  String file_content = readFile(config_filename);

  int config_file_size = file_content.length();
  Serial.println("Config file size: " + String(config_file_size));

  if(config_file_size == 0)
  {
    Serial.println("Initializing config with defaults...");
    saveConfig();

    return false;
  }

  if(config_file_size > 1024) 
  {
    Serial.println("Config file too large");
    return false;
  }

  StaticJsonDocument<1024> doc;

  auto error = deserializeJson(doc, file_content);

  if(error) 
  { 
    Serial.println("Error interpreting config file");
    return false;
  }

  const String _configInit = doc["configInit"];
  const bool _useBluetooth = doc["useBluetooth"];
  const int _scaleIndex = doc["scaleIndex"];
  const int _midiChannel = doc["midiChannel"];
  const int _masterVolume = doc["masterVolume"];
  const int _adjacentPinsFilter = doc["adjacentPinsFilter"];
  const int _dissonantNotesFilter = doc["dissonantNotesFilter"];
  const int _ccForModwheel = doc["ccForModwheel"];
  const String _broadcastAddressMidiHub = doc["broadcastAddressMidiHub"];
  

  Serial.print("_configInit: ");
  Serial.println(_configInit);

  if(_configInit != configInit) // Have we initialized the config yet?
  {
    Serial.println("Initializing config to default values...");
    saveConfig(); // init with default values
  }
  else
  {
    configInit = _configInit;
    useBluetooth = _useBluetooth;
    scaleIndex = _scaleIndex;
    midiChannel = _midiChannel;
    masterVolume = _masterVolume;
    adjacentPinsFilter = _adjacentPinsFilter;
    dissonantNotesFilter = _dissonantNotesFilter;
    ccForModwheel = _ccForModwheel;
    memcpy((void *)broadcastAddressMidiHub.c_str(), _broadcastAddressMidiHub.c_str(), 6);
    
  }

  Serial.println(configInit);
  Serial.println(useBluetooth);
  Serial.println(scaleIndex);
  Serial.println(midiChannel);
  Serial.println(masterVolume);

  return true;
}

void BleOnConnected()
{
  Serial.println("Connected");

  bluetoothConnected = true;
}

void BleOnDisconnected()
{
  Serial.println("Disconnected");

  bluetoothConnected = false;
}

void setup() 
{
  Serial.begin(115200);

  //delay(5000);
  //Serial.println("Hello!");

#if 1
  if(!LittleFS.begin(true))
  {
      Serial.println("LittleFS Mount Failed");
      return;
  }
  else
  {
    //if(saveConfig()) 
    //{
    //  Serial.println("setup -> Config file saved");
    //}  

    readConfig(); 

    //Serial.println("configInit = " + String(configInit));
    //Serial.println("useBluetooth = " + String(useBluetooth));
  }
#endif

  // need to do this to force the scale to be loaded in case it isn't major scale...
  handleChangeRequest(176, 68, scaleIndex + 1);

  // It seems that for UART1 TX = 39 and RX = 38 and I wasn't able to change them
  // The pin definitions are in pins_arduino.h and of course I am using a modified
  // board definition for the adafruit_feather_esp32s2 so I say good enough!
  SERIALSLAVE.begin(2000000); 
  //SERIALSLAVE.begin(115200); 

  //SERIALEXTRA.begin(115200); // You have to remove the 0ohm resistors to the CP2102 to get the port to work.
  
  //Serial0.setDebugOutput(false); // Without this system error messages seem to get sent to this port!

#if 1 
  // Display initialization
  //Serial.println("Initializing display");
  Wire.setPins(35, 36); // SDA, SDL

  //delay(50); // wait for the OLED to power up

  // Show image buffer on the display hardware.
  // Since the buffer is intialized with an Adafruit splashscreen
  // internally, this will display the splashscreen.

  display.begin(0x3C, true); // Address 0x3D default
 //display.setContrast (0); // dim display

  display.setRotation(1);
 
  // Clear the buffer.
  display.clearDisplay();

  display.setTextSize(2);             
  display.setTextColor(SH110X_WHITE);       
  display.setCursor(8,15);             
  display.println(F(" EMMMA-K"));
  display.setCursor(11,37);             
  display.println(F(" v3.2.0"));

  display.display();
#endif

  //Serial.println("Initializing touchpad");
  touch_pad_init();

  //Serial.println("Initializing 6050 IMU");
  //init6050();
  //Serial.println("After initializing 6050 IMU");
  MPU6050Setup();

  // Initialize MIDI if enabled
#if USEMIDI
  // Initialize MIDI, and listen to all MIDI channels
  // This will also call usb_midi's begin()
  USBMIDI.begin(MIDI_CHANNEL_OMNI);

  // Attach the handleNoteOn function to the MIDI Library. It will
  // be called whenever the Bluefruit receives MIDI Note On messages.
  USBMIDI.setHandleNoteOn(handleNoteOn);

  // Do the same for MIDI Note Off messages.
  USBMIDI.setHandleNoteOff(handleNoteOff);

  // wait until device mounted
  Serial.println("Initializing MIDI");
  uint32_t elapsedMs = 0;
  while(!TinyUSBDevice.mounted()) 
  {
    delay(1);
    elapsedMs++; // the max should be around 200ms

    if(elapsedMs > 2100)
      break;
  }

  if(elapsedMs < 2000)  // comment off this line and the line below to send USB MIDI as well (for wirless latency testing)
    midiOn = true;

  Serial.print("Time to init USB MIDI: ");
  Serial.println(elapsedMs);

  delay(250); // need this or you get a squeal to start that doesn't go away!
#else // no midi
  midiOn = false;
#endif

  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);

  touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
  touch_pad_fsm_start();

  for(int i = 0; i < numPins; i++) 
  {
      touch_pad_config(pins[i]);
  }

  delay(2000);

  uint32_t touch_value;
  for (int i = 0; i < numPins; i++) 
  {
      //read benchmark value
      touch_pad_read_benchmark(pins[i], &touch_value);
      Serial.print(touch_value);
      Serial.print(" ");
      benchmark[i] = touch_value;

      if(i == 0 && touch_value > 50000) // is middle note pin touched during startup?
      {
        binding = true; // set flag to wait for hub to bind!
        Serial.println("Waiting for new hub to bind");
      }
  }
  Serial.println();

  //scaleToMidiValues(minorbluesscale, sizeof(minorbluesscale));
  //scaleToMidiValues(majorscale, sizeof(majorscale));
  //Serial.println("After scaleToMidiValues()");

  pixels.setBrightness(10);
  pixels.begin(); // INITIALIZE NeoPixel (REQUIRED)

  pixels.setPixelColor(0, 0x00FF00); // init to green

  pixels.show();   

  // The rest is for ESP-Now
  if(!midiOn)
  {
    if(useBluetooth)
    {
      BLEMIDI.setHandleConnected(BleOnConnected);
      BLEMIDI.setHandleDisconnected(BleOnDisconnected);
    
      MIDI.begin(MIDI_CHANNEL_OMNI);
    }
    else
    {
      WiFi.mode(WIFI_MODE_STA);
      //Serial.println(WiFi.macAddress());  // 84:F7:03:F8:3E:4E

      if (esp_now_init() != ESP_OK) 
      {
        //Serial.println("Error initializing ESP-NOW");

        pixels.setPixelColor(0, 0xFF0000); // set LED to red
        pixels.show();   

        return;
      }

      esp_now_register_send_cb(data_sent);
      esp_now_register_recv_cb(data_received);

      esp_now_peer_info_t peerInfo1 = {}; // must be initialized to 0
      memcpy(peerInfo1.peer_addr, (void *)broadcastAddressMidiHub.c_str(), 6);
      peerInfo1.channel = 0;  
      peerInfo1.encrypt = false;     

      if(esp_now_add_peer(&peerInfo1) != ESP_OK)
      {
        //Serial.println("Failed to add peer");

        pixels.setPixelColor(0, 0xFF0000); // set LED to red
        pixels.show(); 

        return;  
      }

      pixels.setPixelColor(0, 0x0000FF); // set LED to blue
      pixels.show(); 

      esp_now_peer_info_t peerInfo2 = {}; // must be initialized to 0
      memcpy(peerInfo2.peer_addr, broadcastAddressRgbMatrix, 6);
      peerInfo2.channel = 0;  
      peerInfo2.encrypt = false;     

      if(esp_now_add_peer(&peerInfo2) != ESP_OK)
      {
        //Serial.println("Failed to add peer");

        pixels.setPixelColor(0, 0xFF0000); // set LED to red
        pixels.show(); 

        return;  
      }

      pixels.setPixelColor(0, 0x0000FF); // set LED to blue
      pixels.show(); 

      //WiFi.setTxPower(WIFI_POWER_19_5dBm); // I'm not sure this will do anything...
    }
  }

  if(binding)
  {
    display.clearDisplay();
    display.setCursor(5,20);             
    display.println(F("Binding to hub..."));
    display.display();

    while(true)
      delay(100);  
  }

  display.clearDisplay();
  display.setTextSize(1);  
  //display.setCursor(10,25);             
  //display.println(F("Ready..."));
  //display.display();
  //Serial.println("displayScale()");
  mode = "Note";
  displayNotes(true);
  //Serial.println("after displayScale()");
}

// A note will be dissonant if there is a note on that is 1 or 2 semitones
// higher or lower. We calculate indices into the keyname array and then check
// if any notes satify this keeping in mind that it wraps around by 12.

bool dissonantNoteOn(uint8_t midiValueIndex)
{
  //Serial.print("Pin: ");
  //Serial.println(pin);

  uint8_t refIdx = (midiValues[midiValueIndex] % 60) % 12;
 
  bool result = false;

  for(int i = 0; i < totalNotePins; i++)
  {
    if(notePinsOn[i])
    {
      uint8_t idx = (midiValues[i] % 60) % 12;

      if(idx == 0)
      {
        result = refIdx == 1 || refIdx == 2 || refIdx == 11 || refIdx == 10;
      }
      else if(idx == 11)
      {
        result = refIdx == 0 || refIdx == 1 || refIdx == 10 || refIdx == 9;
      }
      else
      {
        result = refIdx == idx + 1  || refIdx == idx + 2|| refIdx == idx - 1 || refIdx == idx - 2;
      }
    }
  }

  if(enableDissonantNotes)
    return false;
  else
    return result;
}

// Check if there are any adjacent pins on (note that the end pins only have one adjacent pin)
// The master has 9 note pins which correspond to the even midiValues[]

bool adjacentPinOn(int pin)
{
  bool result = false;

  switch(pin)
  {
    case 0:
      result = notePinsOn[1] || notePinsOn[2]; // notePinsOn[1] is from the slave
      break;

    case 1:
      result = notePinsOn[0] || notePinsOn[4];
      break;

    case 2:
      result = notePinsOn[2] || notePinsOn[6];
      break;

    case 3:
      result = notePinsOn[4] || notePinsOn[8];
      break;

    case 4:
      result = notePinsOn[6] || notePinsOn[10];
      break;

    case 5:
      result = notePinsOn[8] || notePinsOn[12];
      break;

    case 6:
      result = notePinsOn[10] || notePinsOn[14];
      break;

    case 7:
      result = notePinsOn[12] || notePinsOn[16];
      break;

    case 8:
      result = notePinsOn[14];
      break;

    case 9:
      result = notePinsOn[0] || notePinsOn[3];
      break;

    case 10:
      result = notePinsOn[1] || notePinsOn[5];
      break;

    case 11:
      result = notePinsOn[3] || notePinsOn[7];
      break;

    case 12:
      result = notePinsOn[5] || notePinsOn[9];
      break;

    case 13:
      result = notePinsOn[7] || notePinsOn[11];
      break;

    case 14:
      result = notePinsOn[9] || notePinsOn[13];
      break;

    case 15:
      result = notePinsOn[11] || notePinsOn[15];
      break;

    case 16:
      result = notePinsOn[13];
      break;

    default:
      result = false;
      break;
  }

  if(enableAdjacentPins)
    return false;
  else
    return result;
}

void showNoteColour(uint8_t midiNote)
{
#if RGBLED
  uint8_t idx = midiNote % 12; // index to sequence is modulus 12

  uint32_t colourValue = scriabinColourSequence[idx];

  //Set the new color on the pixel.
  pixels.setPixelColor(0, colourValue); 

    // Send the updated pixel color to the hardware.
  pixels.show();   
#endif
}

void sendChordEspNow2(uint8_t *chordData)
{
  uint8_t mv;
  
  if(chordData[1])
    mv = masterVolume; // use masterVolume for noteon
  else
    mv = 0; // and zero for note off

  uint8_t msgNote[8] = {(uint8_t)(chordData[0]), (uint8_t)(chordData[1]), (uint8_t)mv, midiChannel, 
    (uint8_t)(chordData[2]), (uint8_t)(chordData[3]), (uint8_t)mv, 1}; 

  espNowMicrosAtSend = micros();
  esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  

  if(outcome)
  {
    pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
    pixels.show(); 
  }
}

void sendChordEspNow3(uint8_t *chordData)
{
  uint8_t mv;
  
  if(chordData[1])
    mv = masterVolume; // use masterVolume for noteon
  else
    mv = 0; // and zero for note off

  uint8_t msgNote[12] = {(uint8_t)(chordData[0]), (uint8_t)(chordData[1]), (uint8_t)mv, midiChannel, 
    (uint8_t)(chordData[2]), (uint8_t)(chordData[3]), (uint8_t)mv, midiChannel,
    (uint8_t)(chordData[4]), (uint8_t)(chordData[5]), (uint8_t)mv, midiChannel}; 

  espNowMicrosAtSend = micros();
  esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  

  if(outcome)
  {
    pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
    pixels.show(); 
  }
}

// Note that notes for chords are sent in reverse order incase the instrument can't handle chords (only the last note sent will play)
void sendChordOn(uint8_t idx, uint8_t ofs)
{
  if(scales[scaleIndex] == "Major" || scales[scaleIndex] == "Minor")
  {
    if(midiOn)
    {
      USBMIDI.sendNoteOn(midiValues[idx + 4] + ofs, masterVolume, midiChannel);
      USBMIDI.sendNoteOn(midiValues[idx + 2] + ofs, masterVolume, midiChannel);
      USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
    }
    else
    {
      uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 1, (uint8_t)(midiValues[idx + 2] + ofs), 1,
        (uint8_t)(midiValues[idx] + ofs), 1};

      sendChordEspNow3(chordData3);
    }
    
  }
  else if(scales[scaleIndex] == "Major Pentatonic")
  {
    switch(idx % 5)
    {
      case 0:
        // Scale root note (I)
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 3] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 2] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 1, (uint8_t)(midiValues[idx + 2] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 1:
      case 3:
      case 4:
        // Scale second, fourth and fifth note (II, IV, V), 
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 3] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 1] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 1, (uint8_t)(midiValues[idx + 1] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 2:
        // Scale third note (III) - no triad chord for this one so just play the diad
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 1] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData2[4] = {(uint8_t)(midiValues[idx + 1] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow2(chordData2);
        }
        break;
    }
  }
  else if(scales[scaleIndex] == "Minor Pentatonic")
  {
    switch(idx % 5)
    {
      case 0:
      case 2:
      case 4:
        // Scale root, third and fifth note (I, III, V)
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 3] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 1] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 1, (uint8_t)(midiValues[idx + 1] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 1:
        // Scale fourth note (IV) 
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 3] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 2] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 1, (uint8_t)(midiValues[idx + 2] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 3:
        // Scale third note (III) - no triad for this one so just play the diad
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 1] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData2[4] = {(uint8_t)(midiValues[idx + 1] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow2(chordData2);
        }
        break;
    }
  }
  else if(scales[scaleIndex] == "Minor Blues")
  {
    switch(idx % 6)
    {
      case 0:
        // Scale root(I)
        if(midiOn)
        {
        USBMIDI.sendNoteOn(midiValues[idx + 4] + ofs, masterVolume, midiChannel);
        USBMIDI.sendNoteOn(midiValues[idx + 1] + ofs, masterVolume, midiChannel);
        USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 1, (uint8_t)(midiValues[idx + 1] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 1:
        // Scale second note (II) 
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 4] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 3] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 1, (uint8_t)(midiValues[idx + 3] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 2:
        // Scale third note (III)
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 4] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 2] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 1, (uint8_t)(midiValues[idx + 2] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 3:
        // Scale third fourth note (IV)
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 3] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 2] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 1, (uint8_t)(midiValues[idx + 2] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
      case 4:
        // Scale fifth note (V) - no triad for this one so just play the diad
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 1] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData2[4] = {(uint8_t)(midiValues[idx + 1] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow2(chordData2);
        }
        break;
      case 5:
        // Scale sixth note (VI) 
        if(midiOn)
        {
          USBMIDI.sendNoteOn(midiValues[idx + 3] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx + 1] + ofs, masterVolume, midiChannel);
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 1, (uint8_t)(midiValues[idx + 1] + ofs), 1,
            (uint8_t)(midiValues[idx] + ofs), 1};

          sendChordEspNow3(chordData3);
        }
        break;
    }
  }
  else
  {
    // No chords supported, send root note only
    if(midiOn)
    {
      USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
    }
    else
    {
      uint8_t msgNote[] = {(uint8_t)(midiValues[idx] + ofs), midiChannel, (uint8_t)masterVolume, midiChannel}; 

      espNowMicrosAtSend = micros();
      esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  

      if(outcome)
      {
        //Serial.println("Error sending slave noteOn");
        pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
        pixels.show(); 
      }
    }
  }
}

void sendChordOff(uint8_t idx, uint8_t ofs)
{
  if(scales[scaleIndex] == "Major" || scales[scaleIndex] == "Minor")
  {
    if(midiOn)
    {
      USBMIDI.sendNoteOff(midiValues[idx + 4] + ofs, 0, 1);
      USBMIDI.sendNoteOff(midiValues[idx + 2] + ofs, 0, 1);
      USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
    }
    else
    {
      uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 0, (uint8_t)(midiValues[idx + 2] + ofs), 0,
        (uint8_t)(midiValues[idx] + ofs), 0};

      sendChordEspNow3(chordData3);
    }
  }
  else if(scales[scaleIndex] == "Major Pentatonic")
  {
    switch(idx % 5)
    {
      case 0:
        // Scale root note (I)
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 3] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 2] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 0, (uint8_t)(midiValues[idx + 2] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 1:
      case 3:
      case 4:
        // Scale second, fourth and fifth note (II, IV, V), 
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 3] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 1] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 0, (uint8_t)(midiValues[idx + 1] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 2:
        // Scale third note (III) - no triad for this one so just turn off the diad
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 1] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData2[4] = {(uint8_t)(midiValues[idx + 1] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow2(chordData2);
        }
        break;
    }
  }
  else if(scales[scaleIndex] == "Minor Pentatonic")
  {
    switch(idx % 5)
    {
      case 0:
      case 2:
      case 4:
        // Scale root, third and fifth note (I, III, V)
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 3] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 1] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
          }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 0, (uint8_t)(midiValues[idx + 1] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 1:
        // Scale fourth note (IV) 
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 3] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 2] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 0, (uint8_t)(midiValues[idx + 2] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 3:
        // Scale third note (III) - no triad for this one so just turnoff the diad
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 1] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData2[4] = {(uint8_t)(midiValues[idx + 1] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow2(chordData2);
        }
        break;
    }
  }
  else if(scales[scaleIndex] == "Minor Blues")
  {
    switch(idx % 6)
    {
      case 0:
        // Scale root(I)
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 4] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 1] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 0, (uint8_t)(midiValues[idx + 1] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 1:
        // Scale second note (II) 
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 4] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 3] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
          }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 0, (uint8_t)(midiValues[idx + 3] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 2:
        // Scale third note (III)
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 4] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 2] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 4] + ofs), 0, (uint8_t)(midiValues[idx + 2] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 3:
        // Scale fourth note (IV)
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 3] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 2] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 0, (uint8_t)(midiValues[idx + 2] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
      case 4:
        // Scale fifth note (V) - no triad for this one so just play the diad
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 1] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData2[4] = {(uint8_t)(midiValues[idx + 1] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow2(chordData2);
        }
        break;
      case 5:
        // Scale sixth note (VI) 
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx + 3] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx + 1] + ofs, 0, 1);
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t chordData3[6] = {(uint8_t)(midiValues[idx + 3] + ofs), 0, (uint8_t)(midiValues[idx + 1] + ofs), 0,
            (uint8_t)(midiValues[idx] + ofs), 0};

          sendChordEspNow3(chordData3);
        }
        break;
    }
  }
  else
  {
    // No chords supported, turn off root note only
    if(midiOn)
    {
      USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
    }
    else
    {
      uint8_t msgNote[] = {(uint8_t)(midiValues[idx] + ofs), 0, (uint8_t)0, midiChannel}; 

      espNowMicrosAtSend = micros();
      esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  

      if(outcome)
      {
        //Serial.println("Error sending slave noteOn");
        pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
        pixels.show(); 
      }
    }
  }
}

void processRemoteNotes(bool touched, int i)
{
  uint8_t idx = 1 + (i * 2); 
  uint8_t ofs = key + octave * 12;

  if(touched && !adjacentPinOn(i + 9) && !dissonantNoteOn(1 + (i * 2)))
  {
    if(dissonantNoteOn(1 + (i * 2)))
    {
      //Serial.println("Dissonant note is already on!");
    }

    if(!notePinsOn[1 + (i * 2)])
    {
      notePinsOn[1 + (i * 2)] = true;

      if(option4)
        notePlayedWhileOption4Touched = true;

      if(playChords)
      {
        sendChordOn(idx, ofs); // chords are handled seperately
      }
      else 
      {
        if(midiOn) // comment off this line and the 'else' below to send USB MIDI as well (for wirless latency testing)
        {
          USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
        }
        else
        {
          uint8_t msgNote[] = {(uint8_t)(midiValues[1 + (i * 2)] + key + octave * 12), 1, (uint8_t)masterVolume, midiChannel}; 
          //Serial.print("send remote note: ");
          //Serial.println(msgNote[0]);

          espNowMicrosAtSend = micros();
          esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  

          if(outcome)
          {
            //Serial.println("Error sending slave noteOn");
            pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
            pixels.show(); 
          }
        }
      }

      if(playChords && chordSupported())
        displayChords(false);
      else
        displayNotes(false);

      showNoteColour(midiValues[1 + (i * 2)]);
    }
  }
  else
  {
    if(notePinsOn[1 + (i * 2)])
    {
      notePinsOn[1 + (i * 2)] = false;

      if(playChords)
      {
        sendChordOff(idx, ofs); // chords are handled seperately
      }
      else 
      {
        if(midiOn)
        {
          USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
        }
        else
        {
          uint8_t msgNote[] = {(uint8_t)(midiValues[1 + (i * 2)] + key + octave * 12), 0, (uint8_t)0, midiChannel}; 

          espNowMicrosAtSend = micros();
          esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  

          if(outcome)
          {
            //Serial.println("Error sending slave noteOff");
            pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
            pixels.show(); 
          }
        }
      }

      if(playChords && chordSupported())
        displayChords(false);
      else
        displayNotes(false);        
    }
  }
}

bool allNotesOff()
{
  bool result = true;

  for(int i = 0; i < totalNotePins; i++)
  {
    if(notePinsOn[i])
      result = false;
  }

  return result;
}

void displayRefresh()
{
  if(mode == "Scale")
  {
    displayScale();
  }
  else if(mode == "Key")
  {
    displayKey();
  }
  else if(mode == "Octave")
  {
    displayOctave();
  }
}

bool toggleRelativeMajorMinor()
{
  bool result = false;

  if(!allNotesOff())
    return result;  // don't want to do this if any notes are on...

  if(scales[scaleIndex] == "Major" || scales[scaleIndex] == "Major Pentatonic")
  {
    result = true;

    scaleIndex++;

    key -= 3;

    if(key == -1)
    {
      key = 11;
      octave--;
    }
    else if(key == -2)
    {
      key = 10;
      octave--;
    }
    else if(key == -3)
    {
      key = 9;
      octave--;
    }

    if(scales[scaleIndex] == "Minor")
    {
      scaleToMidiValues(minorscale, sizeof(minorscale));
    }
    else
    {
      scaleToMidiValues(minorpentascale, sizeof(minorpentascale));
    }
  }
  else if(scales[scaleIndex] == "Minor" || scales[scaleIndex] == "Minor Pentatonic")
  {
    result = true;

    scaleIndex--;

    key += 3;

    if(key == 12)
    {
      key = 0;
      octave++;
    }
    else if(key == 13)
    {
      key = 1;
      octave++;
    }
    else if(key == 14)
    {
      key = 2;
      octave++;
    }

    if(scales[scaleIndex] == "Major")
      scaleToMidiValues(majorscale, sizeof(majorscale));
    else
      scaleToMidiValues(pentascale, sizeof(pentascale));
  }

  displayRefresh();

  return result;
}

void processPitchBend()
{
  static bool offsetCaptured = false;
  static double pitchOffset = 0.0;

  double pitch;

  pitch = ypr[2] * 180/M_PI; // This is actually pitch the way it is mounted

  pitch *= -1.0;

  if(option1 && !offsetCaptured)
  {
    offsetCaptured = true;
    pitchOffset = pitch;
  }
  else if(!option1)
  {
    offsetCaptured = false;
  }

  // calculate the desired 0 position 
  pitch -= pitchOffset; 

  double bendX = pitch / 30.0; // 30 deg for full pitch range   

  if(bendX > 1.0)
    bendX = 1.0;
  else if(bendX < -1.0)
    bendX = -1.0;

  const double MaxExpo = 5.0;  
  const double a1 = 30.0; // 30%           
  double a2 = a1/100.0 * MaxExpo; 
  double a3 = 1.0 / exp(a2);         
  double bendxWithExpo = bendX * exp(fabs(a2 * bendX)) * a3;     
  
  pitchBend(bendxWithExpo);  
}

void processModwheel()
{
  double roll = ypr[1] * 180/M_PI;  // ...and roll
  int iRoll = roll / 25.0 * 64; 
  if(iRoll < -64)
    iRoll = -64;
  else if (iRoll > 63)
    iRoll = 63;

  //uint8_t mod = map(iRoll, -64, 63, 1, 127);

  uint8_t mod;
  if(iRoll < 0)
    mod = -iRoll * 2;
  else
    mod =  iRoll * 2;

  if(mod > 127)
    mod = 127;
  
  modwheel(mod);
}

void loop() 
{
  uint32_t touch_value;

  if(SERIALSLAVE.availableForWrite())
    SERIALSLAVE.write(0xA5);

  //if(SERIALEXTRA.availableForWrite())
  //  SERIALEXTRA.write(0xA5);

    // read and process the right option pins

    touch_pad_read_raw_data(pins[9], &touch_value);   // right top (on PCB) option pin
    if(touch_value > benchmark[9] + (0.3 * benchmark[9]))
      option1 = true;
    else if(touch_value < benchmark[9] + (0.2 * benchmark[9]))
      option1 = false;

    touch_pad_read_raw_data(pins[10], &touch_value);   // right middle (on PCB) option pin
    if(touch_value > benchmark[10] + (0.3 * benchmark[10]))
      option2 = true;
    else if(touch_value < benchmark[10] + (0.2 * benchmark[10]))
      option2 = false;

    touch_pad_read_raw_data(pins[11], &touch_value);   // right bottom (on PCB) option pin
    if(touch_value > benchmark[11] + (0.3 * benchmark[11]))
      option3 = true;
    else if(touch_value < benchmark[11] + (0.2 * benchmark[11]))
      option3 = false;

  // The master has 9 note pins which correspond to the even midiValues[]

  for(int i = 0; i < notePins; i++)
  {
    uint8_t idx = (i * 2); 
    uint8_t ofs = key + octave * 12;

    touch_pad_read_raw_data(pins[i], &touch_value);

    if(touch_value > benchmark[i] + (0.3 * benchmark[i]) && !adjacentPinOn(i)  && !dissonantNoteOn(i * 2))
    {
      //Serial.print(touch_value);
      //Serial.print(" ");

      if(dissonantNoteOn(i * 2))
      {
        //Serial.println("Dissonant note is already on!");
      }

      if(!notePinsOn[i * 2])
      {
        notePinsOn[i * 2] = true;

        if(option4)
        {
          notePlayedWhileOption4Touched = true; 
        }

        if(playChords)
        {
          sendChordOn(idx, ofs);
        }
        else
        {
          if(midiOn)
          {
            USBMIDI.sendNoteOn(midiValues[idx] + ofs, masterVolume, midiChannel);
          }
          else
          {
            uint8_t msgNote[] = {(uint8_t)(midiValues[i * 2] + key + octave * 12), 1, (uint8_t)masterVolume, midiChannel}; 

            espNowMicrosAtSend = micros();
            esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  
            
            if(outcome)
            {
              //Serial.println("Error sending master noteOn");
              pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
              pixels.show(); 
            }
          }
        }

      if(playChords  && chordSupported())
        displayChords(false);
      else
        displayNotes(false);
        
        showNoteColour(midiValues[i * 2]);
      }
    }
    else if(touch_value < benchmark[i] + (0.2 * benchmark[i]))
    {
      if(notePinsOn[i * 2])
      {
        notePinsOn[i * 2] = false;

        if(playChords)
        {
          sendChordOff(idx, ofs); // chords are handled seperately
        }
        else 
        {
          if(midiOn)
          {
            USBMIDI.sendNoteOff(midiValues[idx] + ofs, 0, 1);
          }
          else
          {
            uint8_t msgNote[] = {(uint8_t)(midiValues[i * 2] + key + octave * 12), 0, (uint8_t)0, midiChannel}; 

            espNowMicrosAtSend = micros();
            esp_err_t outcome = wirelessSend((uint8_t *) &msgNote, sizeof(msgNote));  

            if(outcome)
            {
              //Serial.println("Error sending master noteOff");
              pixels.setPixelColor(0, 0x00FFFF); // set LED to yellow
              pixels.show(); 
            }
          }
        }
        
      if(playChords && chordSupported())
        displayChords(false);
      else
        displayNotes(false);        
      }
    }
  }
   
  float ax;
  float ay; 
  float az;

  static uint32_t t0 = millis();

  static uint32_t lastOption4millis = t0;
  static bool option4Touched = false;

  // This is the pitchbend and modwheel stuff. Only do every 25ms
  if(millis() - t0 > 25)
  {
    t0 = millis();

    // The MPU6050 processing. Pitch is used for pitch bend and roll for modwheel

    MPU6050Loop();

    messageUpdate(false); // for pop-up message timing

    processPitchBend();

    processModwheel();
  }

  // Read two bytes from the slave asynchronously. The first byte has the MSB set and
  // each byte has data for 7 pins. Only 8 pins are note pins.
  if(SERIALSLAVE.available())
  {
    uint8_t c;
    static uint8_t c1 = 0;
    static uint8_t lastc = 0;
      
    SERIALSLAVE.read(&c, 1);

    if(c & 0x80) // is it the first byte?
    {
      c1 = c; // just save it
      if(c != lastc)
      {
        lastc = c;
        //Serial.print(c, 16);
        //Serial.print(' ');
      }
    }
    else 
    {
      // second byte received
      // first process the first byte
      int i;

      for(i = 0; i < 7; i++)
      {
        bool touched = c1 & (0x40 >> i);

        processRemoteNotes(touched, i);
      }

      i = 7;
      bool touched = c & 0x01; // now look at the note bit in the second byte

      option4 = c & 0x10;

      if(option4)
      {
        if(!option4Touched)
        {
          // Here if touched but wasn't last time
          option4Touched = true;
          lastOption4millis = millis();
        }
      }
      else
      {
        // Here if untouched but it was last time
        if(option4Touched)
        {
          option4Touched = false;

          if(millis() - lastOption4millis < 500)
          {
            Serial.println("Toggle relative major/minor");
            bool success = toggleRelativeMajorMinor();
            String msg;
            if(success)
              msg = "To " + scales[scaleIndex];
            else
              msg = "Scale not supported  or note on";

            displayMessage(msg);
          }
          else
          {
            if(!notePlayedWhileOption4Touched)
            {
              Serial.println("Toggle chords on/off"); 
              if(playChords)
                displayMessage("  Chords OFF");
              else
                displayMessage("  Chords ON");
              playChords = !playChords;
            }
            else
              notePlayedWhileOption4Touched = false;
          }
        }
      }

      option5 = c & 0x80;
      option6 = c & 0x04;

      processRemoteNotes(touched, i);

      static bool lastOption2 = false;
      static bool lastOption3 = false;
      static bool lastOption6 = false;

      if(option2 && !lastOption2 && allNotesOff())
      {
        //Serial.println(optionsMode);
        if(optionsMode)
        {
          if(mode == "Scale")
          {
            changeScale(true);
            displayScale();
          }
          else if(mode == "Key")
          {
            changeKey(true);
            displayKey();
          }
          else if(mode == "Octave")
          {
            changeOctave(true);
            displayOctave();
          }
          //else if(mode == "Config")
          //{
          //  optionsMode = false;

          //  displayConfig();
          //}
        }
        else
        {
          // In config mode now
#if 0
          //Serial.println("config mode");
          if(config == "Adjacent Key Filt")
          {
            //Serial.println("change adjacent key filter");
            changeAdjacentPinFilt();

            //Serial.println("display adjacent key filter");
            displayAdjacentPinFilt();
          }
          else if(config == "Dissnt Notes Filt")
          {
            changeDissonantNotesFilt();

            displayDissonantNotesFilt();
          }
          else if(config == "MIDI Channel")
          {
            changeMidiChannel(true);

            displayMidiChannel();
          }
          else if(config == "Exit")
          {
            // save config here
            saveConfig();

            // exit
            optionsMode = true;

            displayMode();
          }
#endif
          (*configChangeFunctions[config])(true);

          if(!optionsMode)  // Note that exit will take us out of config mode so in that case don't displayConfig()
            displayConfig();
        }
      }
      
      lastOption2 = option2;

      if(option3 && !lastOption3 && allNotesOff())
      {
        if(optionsMode)
        {
          if(mode == "Scale")
          {
            changeScale(false);
            displayScale();
          }
          else if(mode == "Key")
          {
            changeKey(false);
            displayKey();
          }
          else if(mode == "Octave")
          {
            changeOctave(false);
            displayOctave();
          }
        }
        else
        {
          // In config mode now
          //Serial.println("config mode");
#if 0
          if(config == "Adjacent Key Filt")
          {
            //Serial.println("change adjacent key filter");
            changeAdjacentPinFilt();

            //Serial.println("display adjacent key filter");
            displayAdjacentPinFilt();
          }
          else if(config == "Dissnt Notes Filt")
          {
            changeDissonantNotesFilt();

            displayDissonantNotesFilt();
          }
          else if(config == "MIDI Channel")
          {
            changeMidiChannel(false);

            displayMidiChannel();
          }
#endif
          (*configChangeFunctions[config])(false);

          if(!optionsMode)  // Note that exit will take us out of config mode so in that case don't displayConfig()
            displayConfig();
        }
      }

      lastOption3 = option3;

      // Handle the adjacentPins and dissonantNotes filters
      if(playChords)
      {
        // Force the filters on if we are playing chords
        enableAdjacentPins = false;
        enableDissonantNotes = false;
      }
      else
      {
        // If option4 ignore both filters
        if(option4)
        {
          // Ignore the filters if option4 set
          enableAdjacentPins = true;
          enableDissonantNotes = true;
        }
        else
        {
          // set the enables according to the filters
          //Serial.printf("%d ", adjacentPinsFilter);
          if(adjacentPinsFilter)
            enableAdjacentPins = false;
          else
            enableAdjacentPins = true;

          if(dissonantNotesFilter)
            enableDissonantNotes = false;
          else
            enableDissonantNotes = true;
        }
      }

      static uint32_t t1 = millis();
      static uint32_t lastOption6millis = t1;
      static bool option6TimerStarted = false;
      static bool option6Timeout = true;

      if(option6 && !lastOption6) // Was option6 just pressed?
      {
        lastOption6millis = millis();

        option6TimerStarted = true;
      }

      if(option6 && option6TimerStarted)
      {
        if(millis() - lastOption6millis > 2000) // Has option6 been pressed for more that 2 seconds?
        {
          //Serial.println("Entering config mode");

          optionsMode = false;

          displayConfig();

          option6Timeout = true;

          option6TimerStarted = false;
        }
      }

      if(!option6 && lastOption6) // was option6 just released?
      {
        {
          if(optionsMode)
            changeMode();
          else
          {
            if(option6Timeout)
            {
              option6Timeout = false;
            }
            else
              changeConfig();
          }
        }
      }

      lastOption6 = option6;      
    }
  } 

  //if(SERIALEXTRA.available())
  //{
  //  char c;
      
  //  SERIALEXTRA.read(&c, 1);

  //  if(c != 0xA5)
  //  {
      //Serial.print(c);
      //Serial.print(" ");
  //  }
    //else
    //  Serial.print('.');
  //}

#if RGBLED
  if(allNotesOff())
  {
    pixels.setPixelColor(0, 0x000000); // set to black

    // Send the updated pixel color to the hardware.
    pixels.show();   
  }
#endif

  if(midiOn)
  {
    if(USBMIDI.read())
    {
      uint8_t type = USBMIDI.getType();
      uint8_t data1 = USBMIDI.getData1();
      uint8_t data2 = USBMIDI.getData2();

      handleChangeRequest(type, data1, data2);

      displayRefresh();
    }
  }
  else if(useBluetooth && bluetoothConnected)
  {
    MIDI.read();
  }
}

void handleNoteOn(byte channel, byte pitch, byte velocity)
{
  // Log when a note is pressed.
  //Serial.print("Note on: channel = ");
  //Serial.print(channel);

  //Serial.print(" pitch = ");
  //Serial.print(pitch);

  //Serial.print(" velocity = ");
  //Serial.println(velocity);
}

void handleNoteOff(byte channel, byte pitch, byte velocity)
{
  // Log when a note is released.
  //Serial.print("Note off: channel = ");
  //Serial.print(channel);

  //Serial.print(" pitch = ");
  //Serial.print(pitch);

  //Serial.print(" velocity = ");
  //Serial.println(velocity);
}

// MPU6050 Stuff
#include "I2Cdev.h"

#include "MPU6050_6Axis_MotionApps20.h"
//#include "MPU6050.h" // not necessary if using MotionApps include file

// Arduino Wire library is required if I2Cdev I2CDEV_ARDUINO_WIRE implementation
// is used in I2Cdev.h
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    #include "Wire.h"
#endif

// class default I2C address is 0x68
// specific I2C addresses may be passed as a parameter here
// AD0 low = 0x68 (default for SparkFun breakout and InvenSense evaluation board)
// AD0 high = 0x69
MPU6050 mpu(0x68, &Wire1);
//MPU6050 mpu(0x69); // <-- use for AD0 high

/* =========================================================================
   NOTE: In addition to connection 3.3v, GND, SDA, and SCL, this sketch
   depends on the MPU-6050's INT pin being connected to the Arduino's
   external interrupt #0 pin. On the Arduino Uno and Mega 2560, this is
   digital I/O pin 2.
 * ========================================================================= */

/* =========================================================================
   NOTE: Arduino v1.0.1 with the Leonardo board generates a compile error
   when using Serial.write(buf, len). The Teapot output uses this method.
   The solution requires a modification to the Arduino USBAPI.h file, which
   is fortunately simple, but annoying. This will be fixed in the next IDE
   release. For more info, see these links:

   http://arduino.cc/forum/index.php/topic,109987.0.html
   http://code.google.com/p/arduino/issues/detail?id=958
 * ========================================================================= */



// uncomment "OUTPUT_READABLE_QUATERNION" if you want to see the actual
// quaternion components in a [w, x, y, z] format (not best for parsing
// on a remote host such as Processing or something though)
//#define OUTPUT_READABLE_QUATERNION

// uncomment "OUTPUT_READABLE_EULER" if you want to see Euler angles
// (in degrees) calculated from the quaternions coming from the FIFO.
// Note that Euler angles suffer from gimbal lock (for more info, see
// http://en.wikipedia.org/wiki/Gimbal_lock)
//#define OUTPUT_READABLE_EULER

// uncomment "OUTPUT_READABLE_YAWPITCHROLL" if you want to see the yaw/
// pitch/roll angles (in degrees) calculated from the quaternions coming
// from the FIFO. Note this also requires gravity vector calculations.
// Also note that yaw/pitch/roll angles suffer from gimbal lock (for
// more info, see: http://en.wikipedia.org/wiki/Gimbal_lock)
#define OUTPUT_READABLE_YAWPITCHROLL

// uncomment "OUTPUT_READABLE_REALACCEL" if you want to see acceleration
// components with gravity removed. This acceleration reference frame is
// not compensated for orientation, so +X is always +X according to the
// sensor, just without the effects of gravity. If you want acceleration
// compensated for orientation, us OUTPUT_READABLE_WORLDACCEL instead.
//#define OUTPUT_READABLE_REALACCEL

// uncomment "OUTPUT_READABLE_WORLDACCEL" if you want to see acceleration
// components with gravity removed and adjusted for the world frame of
// reference (yaw is relative to initial orientation, since no magnetometer
// is present in this case). Could be quite handy in some cases.
//#define OUTPUT_READABLE_WORLDACCEL

// uncomment "OUTPUT_TEAPOT" if you want output that matches the
// format used for the InvenSense teapot demo
//#define OUTPUT_TEAPOT



//#define INTERRUPT_PIN 2  // use pin 2 on Arduino Uno & most boards
//#define LED_PIN 13 // (Arduino is 13, Teensy is 11, Teensy++ is 6)
bool blinkState = false;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
//float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

// packet structure for InvenSense teapot demo
uint8_t teapotPacket[14] = { '$', 0x02, 0,0, 0,0, 0,0, 0,0, 0x00, 0x00, '\r', '\n' };



// ================================================================
// ===               INTERRUPT DETECTION ROUTINE                ===
// ================================================================

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() 
{
    mpuInterrupt = true;
}



// ================================================================
// ===                      INITIAL SETUP                       ===
// ================================================================

void MPU6050Setup() 
{
    // join I2C bus (I2Cdev library doesn't do this automatically)
    #if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    
        Wire1.setPins(41, 42); // SDA, SCL // added to test on the EMMMA-K
        Wire1.begin();
        Wire1.setClock(400000); // 400kHz I2C clock. Comment this line if having compilation difficulties
    #elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
        Fastwire::setup(400, true);
    #endif

    // initialize serial communication
    // (115200 chosen because it is required for Teapot Demo output, but it's
    // really up to you depending on your project)
    //Serial.begin(115200);
    //delay(5000);
    //while (!Serial); // wait for Leonardo enumeration, others continue immediately

    // NOTE: 8MHz or slower host processors, like the Teensy @ 3.3V or Arduino
    // Pro Mini running at 3.3V, cannot handle this baud rate reliably due to
    // the baud timing being too misaligned with processor ticks. You must use
    // 38400 or slower in these cases, or use some kind of external separate
    // crystal solution for the UART timer.

    // initialize device
    Serial.println(F("Initializing I2C devices..."));
    mpu.initialize();
    //pinMode(INTERRUPT_PIN, INPUT);

    // verify connection
    Serial.println(F("Testing device connections..."));
    Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

    // wait for ready
    //Serial.println(F("\nSend any character to begin DMP programming and demo: "));
    //while (Serial.available() && Serial.read()); // empty buffer
    //while (!Serial.available());                 // wait for data
    //while (Serial.available() && Serial.read()); // empty buffer again

    // load and configure the DMP
    Serial.println(F("Initializing DMP..."));
    devStatus = mpu.dmpInitialize();

    // supply your own gyro offsets here, scaled for min sensitivity
    mpu.setXGyroOffset(220);
    mpu.setYGyroOffset(76);
    mpu.setZGyroOffset(-85);
    mpu.setZAccelOffset(1788); // 1688 factory default for my test chip

    // make sure it worked (returns 0 if so)
    if (devStatus == 0) {
        // Calibration Time: generate offsets and calibrate our MPU6050
        mpu.CalibrateAccel(6);
        mpu.CalibrateGyro(6);
        mpu.PrintActiveOffsets();
        // turn on the DMP, now that it's ready
        Serial.println(F("Enabling DMP..."));
        mpu.setDMPEnabled(true);

        // enable Arduino interrupt detection
        Serial.print(F("Enabling interrupt detection (Arduino external interrupt "));
        //Serial.print(digitalPinToInterrupt(INTERRUPT_PIN));
        Serial.println(F(")..."));
        //attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), dmpDataReady, RISING);
        mpuIntStatus = mpu.getIntStatus();

        // set our DMP Ready flag so the main loop() function knows it's okay to use it
        Serial.println(F("DMP ready! Waiting for first interrupt..."));
        dmpReady = true;

        // get expected DMP packet size for later comparison
        packetSize = mpu.dmpGetFIFOPacketSize();
    } 
    else 
    {
        // ERROR!
        // 1 = initial memory load failed
        // 2 = DMP configuration updates failed
        // (if it's going to break, usually the code will be 1)
        Serial.print(F("DMP Initialization failed (code "));
        Serial.print(devStatus);
        Serial.println(F(")"));
    }
}



// ================================================================
// ===                    MAIN PROGRAM LOOP                     ===
// ================================================================

void MPU6050Loop() 
{
    // if programming failed, don't try to do anything
    if (!dmpReady) return;
    // read a packet from FIFO
    if (mpu.dmpGetCurrentFIFOPacket(fifoBuffer)) { // Get the Latest packet 
        #ifdef OUTPUT_READABLE_QUATERNION
            // display quaternion values in easy matrix form: w x y z
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            Serial.print("quat\t");
            Serial.print(q.w);
            Serial.print("\t");
            Serial.print(q.x);
            Serial.print("\t");
            Serial.print(q.y);
            Serial.print("\t");
            Serial.println(q.z);
        #endif

        #ifdef OUTPUT_READABLE_EULER
            // display Euler angles in degrees
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetEuler(euler, &q);
            Serial.print("euler\t");
            Serial.print(euler[0] * 180/M_PI);
            Serial.print("\t");
            Serial.print(euler[1] * 180/M_PI);
            Serial.print("\t");
            Serial.println(euler[2] * 180/M_PI);
        #endif

        #ifdef OUTPUT_READABLE_YAWPITCHROLL
            // display Euler angles in degrees
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
            //Serial.print("ypr\t");
            //Serial.print(ypr[0] * 180/M_PI);
            //Serial.print("\t");
            //Serial.print(ypr[1] * 180/M_PI);
            //Serial.print("\t");
            //Serial.println(ypr[2] * 180/M_PI);
        #endif

        #ifdef OUTPUT_READABLE_REALACCEL
            // display real acceleration, adjusted to remove gravity
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetAccel(&aa, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
            Serial.print("areal\t");
            Serial.print(aaReal.x);
            Serial.print("\t");
            Serial.print(aaReal.y);
            Serial.print("\t");
            Serial.println(aaReal.z);
        #endif

        #ifdef OUTPUT_READABLE_WORLDACCEL
            // display initial world-frame acceleration, adjusted to remove gravity
            // and rotated based on known orientation from quaternion
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetAccel(&aa, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
            mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q);
            Serial.print("aworld\t");
            Serial.print(aaWorld.x);
            Serial.print("\t");
            Serial.print(aaWorld.y);
            Serial.print("\t");
            Serial.println(aaWorld.z);
        #endif
    
        #ifdef OUTPUT_TEAPOT
            // display quaternion values in InvenSense Teapot demo format:
            teapotPacket[2] = fifoBuffer[0];
            teapotPacket[3] = fifoBuffer[1];
            teapotPacket[4] = fifoBuffer[4];
            teapotPacket[5] = fifoBuffer[5];
            teapotPacket[6] = fifoBuffer[8];
            teapotPacket[7] = fifoBuffer[9];
            teapotPacket[8] = fifoBuffer[12];
            teapotPacket[9] = fifoBuffer[13];
            Serial.write(teapotPacket, 14);
            teapotPacket[11]++; // packetCount, loops at 0xFF on purpose
        #endif

        // blink LED to indicate activity
        //blinkState = !blinkState;
        //digitalWrite(LED_PIN, blinkState);
    }
}
