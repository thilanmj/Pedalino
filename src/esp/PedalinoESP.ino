//  __________           .___      .__  .__                   ___ ________________    ___
//  \______   \ ____   __| _/____  |  | |__| ____   ____     /  / \__    ___/     \   \  \   
//   |     ___// __ \ / __ |\__  \ |  | |  |/    \ /  _ \   /  /    |    | /  \ /  \   \  \  
//   |    |   \  ___// /_/ | / __ \|  |_|  |   |  (  <_> ) (  (     |    |/    Y    \   )  )
//   |____|    \___  >____ |(____  /____/__|___|  /\____/   \  \    |____|\____|__  /  /  /
//                 \/     \/     \/             \/           \__\                 \/  /__/
//                                                                       (c) 2018 alf45star
//
// ESP8266/ESP32 MIDI Gateway between Serial MIDI <-> WiFi AppleMIDI <-> Bluetooth LE MIDI <-> WiFi OSC

#include <Arduino.h>
#include <ArduinoJson.h>

#ifdef ARDUINO_ARCH_ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266LLMNR.h>
#include <ESP8266HTTPUpdateServer.h>
#endif

#ifdef ARDUINO_ARCH_ESP32
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_log.h>
#include <string>
#include <BlynkSimpleEsp32.h>

static const char LOG_TAG[] = "PedalinoESP";
#endif

#include <WiFiClient.h>
#include <WiFiUdp.h>

#include <MIDI.h>
#include <AppleMidi.h>
#include <OSCMessage.h>
#include <OSCBundle.h>
#include <OSCData.h>

//#define PEDALINO_SERIAL_DEBUG
//#define PEDALINO_TELNET_DEBUG

#ifdef PEDALINO_TELNET_DEBUG
#include "RemoteDebug.h"          // Remote debug over telnet - not recommended for production, only for development    
RemoteDebug Debug;
#endif

#define WIFI_CONNECT_TIMEOUT    10
#define SMART_CONFIG_TIMEOUT    30

#ifndef LED_BUILTIN
#define LED_BUILTIN    2
#endif

#define WIFI_LED       LED_BUILTIN  // onboard LED, used as status indicator

#if defined(ARDUINO_ARCH_ESP8266) && defined(PEDALINO_SERIAL_DEBUG)
#define SERIALDEBUG       Serial1
#define WIFI_LED       0  // ESP8266 only: onboard LED on GPIO2 is shared with Serial1 TX
#endif

#ifdef ARDUINO_ARCH_ESP32
#define SERIALDEBUG       Serial
#define DPRINT(...)       ESP_LOGI(LOG_TAG, __VA_ARGS__)
#define DPRINTLN(...)     ESP_LOGI(LOG_TAG, __VA_ARGS__)
#endif

#ifdef ARDUINO_ARCH_ESP8266
#define BLE_LED_OFF()
#define BLE_LED_ON()
#define WIFI_LED_OFF() digitalWrite(WIFI_LED, HIGH)
#define WIFI_LED_ON()  digitalWrite(WIFI_LED, LOW)
#endif

#ifdef ARDUINO_ARCH_ESP32
#define BLE_LED         21
#define WIFI_LED        19
#define BLE_LED_OFF()   digitalWrite(BLE_LED, LOW)
#define BLE_LED_ON()    digitalWrite(BLE_LED, HIGH)
#define WIFI_LED_OFF()  digitalWrite(WIFI_LED, LOW)
#define WIFI_LED_ON()   digitalWrite(WIFI_LED, HIGH)
#endif

const char host[]           = "pedalino";
const char blynkAuthToken[] = "63c670c13d334b059b9dbc9a0b690f4b";
WidgetLCD  blynkLCD(V0);

#ifdef ARDUINO_ARCH_ESP8266
ESP8266WebServer        httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
#endif

#ifdef ARDUINO_ARCH_ESP32
WebServer               httpServer(80);
HTTPUpload              httpUpdater;
#endif

// Bluetooth LE MIDI interface

#ifdef ARDUINO_ARCH_ESP32

#define MIDI_SERVICE_UUID        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define MIDI_CHARACTERISTIC_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"

BLEServer             *pServer;
BLEService            *pService;
BLEAdvertising        *pAdvertising;
BLECharacteristic     *pCharacteristic;
BLESecurity           *pSecurity;
#endif
bool                  bleMidiConnected = false;
unsigned long         bleLastOn        = 0;

// WiFi MIDI interface to comunicate with AppleMIDI/RTP-MDI devices

APPLEMIDI_CREATE_INSTANCE(WiFiUDP, AppleMIDI); // see definition in AppleMidi_Defs.h

bool          appleMidiConnected = false;
unsigned long wifiLastOn         = 0;

// Serial MIDI interface to comunicate with Arduino

#define SERIALMIDI_BAUD_RATE  115200

struct SerialMIDISettings : public midi::DefaultSettings
{
  static const long BaudRate = SERIALMIDI_BAUD_RATE;
};

#ifdef ARDUINO_ARCH_ESP8266
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, Serial, MIDI, SerialMIDISettings);
#endif

#ifdef ARDUINO_ARCH_ESP32
#define SERIALMIDI_RX         16
#define SERIALMIDI_TX         17
HardwareSerial                SerialMIDI(2);
MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, SerialMIDI, MIDI, SerialMIDISettings);
#endif


// WiFi OSC comunication

WiFiUDP                 oscUDP;                  // A UDP instance to let us send and receive packets over UDP
IPAddress               oscRemoteIp;             // remote IP of an external OSC device or broadcast address
const unsigned int      oscRemotePort = 9000;    // remote port of an external OSC device
const unsigned int      oscLocalPort = 8000;     // local port to listen for OSC packets (actually not used for sending)
OSCMessage              oscMsg;
OSCErrorCode            oscError;


#ifdef ARDUINO_ARCH_ESP32

void BLEMidiReceive(uint8_t *, uint8_t);

class MyBLEServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      bleMidiConnected = true;
      DPRINT("BLE client connected");
    };

    void onDisconnect(BLEServer* pServer) {
      bleMidiConnected = false;
      DPRINT("BLE client disconnected");
    }
};

class MyBLECharateristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();
      if (rxValue.length() > 0) {
        BLEMidiReceive((uint8_t *)(rxValue.c_str()), rxValue.length());
        DPRINT("Received %2d bytes: %2H %2H %2H", rxValue.length(), rxValue[2], rxValue[3], rxValue[4]);
      }
    }
};

void BLEMidiStart ()
{
  BLEDevice::init("Pedal");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyBLEServerCallbacks());

  pService = pServer->createService(MIDI_SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(MIDI_CHARACTERISTIC_UUID,
                    BLECharacteristic::PROPERTY_READ   |
                    BLECharacteristic::PROPERTY_NOTIFY |
                    BLECharacteristic::PROPERTY_WRITE_NR);

  pCharacteristic->setCallbacks(new MyBLECharateristicCallbacks());

  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  DPRINT("BLE Service started");

  pAdvertising = pServer->getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->start();

  pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);

  DPRINT("BLE Advertising started");
}

void BLEMidiTimestamp (uint8_t *header, uint8_t *timestamp)
{
  /*
    The first byte of all BLE packets must be a header byte. This is followed by timestamp bytes and MIDI messages.

    Header Byte
      bit 7     Set to 1.
      bit 6     Set to 0. (Reserved for future use)
      bits 5-0  timestampHigh:Most significant 6 bits of timestamp information.
    The header byte contains the topmost 6 bits of timing information for MIDI events in the BLE
    packet. The remaining 7 bits of timing information for individual MIDI messages encoded in a
    packet is expressed by timestamp bytes.
    Timestamp Byte
    bit 7       Set to 1.
    bits 6-0    timestampLow: Least Significant 7 bits of timestamp information.
    The 13-bit timestamp for the first MIDI message in a packet is calculated using 6 bits from the
    header byte and 7 bits from the timestamp byte.
    Timestamps are 13-bit values in milliseconds, and therefore the maximum value is 8,191 ms.
    Timestamps must be issued by the sender in a monotonically increasing fashion.
    timestampHigh is initially set using the lower 6 bits from the header byte while the timestampLow is
    formed of the lower 7 bits from the timestamp byte. Should the timestamp value of a subsequent
    MIDI message in the same packet overflow/wrap (i.e., the timestampLow is smaller than a
    preceding timestampLow), the receiver is responsible for tracking this by incrementing the
    timestampHigh by one (the incremented value is not transmitted, only understood as a result of the
    overflow condition).
    In practice, the time difference between MIDI messages in the same BLE packet should not span
    more than twice the connection interval. As a result, a maximum of one overflow/wrap may occur
    per BLE packet.
    Timestamps are in the sender’s clock domain and are not allowed to be scheduled in the future.
    Correlation between the receiver’s clock and the received timestamps must be performed to
    ensure accurate rendering of MIDI messages, and is not addressed in this document.
  */
  /*
    Calculating a Timestamp
    To calculate the timestamp, the built-in millis() is used.
    The BLE standard only specifies 13 bits worth of millisecond data though,
    so it’s bitwise anded with 0x1FFF for an ever repeating cycle of 13 bits.
    This is done right after a MIDI message is detected. It’s split into a 6 upper bits, 7 lower bits,
    and the MSB of both bytes are set to indicate that this is a header byte.
    Both bytes are placed into the first two position of an array in preparation for a MIDI message.
  */
  unsigned long currentTimeStamp = millis() & 0x01FFF;

  *header = ((currentTimeStamp >> 7) & 0x3F) | 0x80;        // 6 bits plus MSB
  *timestamp = (currentTimeStamp & 0x7F) | 0x80;            // 7 bits plus MSB
}


// Decodes the BLE characteristics and calls MIDI.send if the packet contains sendable MIDI data
// https://learn.sparkfun.com/tutorials/midi-ble-tutorial

void BLEMidiReceive(uint8_t *buffer, uint8_t bufferSize)
{
  /*
    The general form of a MIDI message follows:

    n-byte MIDI Message
      Byte 0            MIDI message Status byte, Bit 7 is Set to 1.
      Bytes 1 to n-1    MIDI message Data bytes, if n > 1. Bit 7 is Set to 0
    There are two types of MIDI messages that can appear in a single packet: full MIDI messages and
    Running Status MIDI messages. Each is encoded differently.
    A full MIDI message is simply the MIDI message with the Status byte included.
    A Running Status MIDI message is a MIDI message with the Status byte omitted. Running Status
    MIDI messages may only be placed in the data stream if the following criteria are met:
    1.  The original MIDI message is 2 bytes or greater and is not a System Common or System
        Real-Time message.
    2.  The omitted Status byte matches the most recently preceding full MIDI message’s Status
        byte within the same BLE packet.
    In addition, the following rules apply with respect to Running Status:
    1.  A Running Status MIDI message is allowed within the packet after at least one full MIDI
        message.
    2.  Every MIDI Status byte must be preceded by a timestamp byte. Running Status MIDI
        messages may be preceded by a timestamp byte. If a Running Status MIDI message is not
        preceded by a timestamp byte, the timestamp byte of the most recently preceding message
        in the same packet is used.
    3.  System Common and System Real-Time messages do not cancel Running Status if
        interspersed between Running Status MIDI messages. However, a timestamp byte must
        precede the Running Status MIDI message that follows.
    4.  The end of a BLE packet does cancel Running Status.
    In the MIDI 1.0 protocol, System Real-Time messages can be sent at any time and may be
    inserted anywhere in a MIDI data stream, including between Status and Data bytes of any other
    MIDI messages. In the MIDI BLE protocol, the System Real-Time messages must be deinterleaved
    from other messages – except for System Exclusive messages.
  */
  midi::Channel   channel;
  midi::MidiType  command;

  //Pointers used to search through payload.
  uint8_t lPtr = 0;
  uint8_t rPtr = 0;
  //lastStatus used to capture runningStatus
  uint8_t lastStatus;
  //Decode first packet -- SHALL be "Full MIDI message"
  lPtr = 2; //Start at first MIDI status -- SHALL be "MIDI status"
  //While statement contains incrementing pointers and breaks when buffer size exceeded.
  while (1) {
    lastStatus = buffer[lPtr];
    if ( (buffer[lPtr] < 0x80) ) {
      //Status message not present, bail
      return;
    }
    command = MIDI.getTypeFromStatusByte(lastStatus);
    channel = MIDI.getChannelFromStatusByte(lastStatus);
    //Point to next non-data byte
    rPtr = lPtr;
    while ( (buffer[rPtr + 1] < 0x80) && (rPtr < (bufferSize - 1)) ) {
      rPtr++;
    }
    //look at l and r pointers and decode by size.
    if ( rPtr - lPtr < 1 ) {
      //Time code or system
      MIDI.send(command, 0, 0, channel);
    } else if ( rPtr - lPtr < 2 ) {
      MIDI.send(command, buffer[lPtr + 1], 0, channel);
    } else if ( rPtr - lPtr < 3 ) {
      MIDI.send(command, buffer[lPtr + 1], buffer[lPtr + 2], channel);
    } else {
      //Too much data
      //If not System Common or System Real-Time, send it as running status
      switch ( buffer[lPtr] & 0xF0 )
      {
        case 0x80:
        case 0x90:
        case 0xA0:
        case 0xB0:
        case 0xE0:
          for (int i = lPtr; i < rPtr; i = i + 2) {
            MIDI.send(command, buffer[i + 1], buffer[i + 2], channel);
          }
          break;
        case 0xC0:
        case 0xD0:
          for (int i = lPtr; i < rPtr; i = i + 1) {
            MIDI.send(command, buffer[i + 1], 0, channel);
          }
          break;
        default:
          break;
      }
    }
    //Point to next status
    lPtr = rPtr + 2;
    if (lPtr >= bufferSize) {
      //end of packet
      return;
    }
  }
}

void BLESendChannelMessage1(byte type, byte channel, byte data1)
{
  uint8_t midiPacket[4];

  BLEMidiTimestamp(&midiPacket[0], &midiPacket[1]);
  midiPacket[2] = (type & 0xf0) | (channel & 0x0f);
  midiPacket[3] = data1;
  pCharacteristic->setValue(midiPacket, 4);
  pCharacteristic->notify();
}

void BLESendChannelMessage2(byte type, byte channel, byte data1, byte data2)
{
  uint8_t midiPacket[5];

  BLEMidiTimestamp(&midiPacket[0], &midiPacket[1]);
  midiPacket[2] = (type & 0xf0) | (channel & 0x0f);
  midiPacket[3] = data1;
  midiPacket[4] = data2;
  pCharacteristic->setValue(midiPacket, 5);
  pCharacteristic->notify();
}

void BLESendSystemCommonMessage1(byte type, byte data1)
{
  uint8_t midiPacket[4];

  BLEMidiTimestamp(&midiPacket[0], &midiPacket[1]);
  midiPacket[2] = type;
  midiPacket[3] = data1;
  pCharacteristic->setValue(midiPacket, 4);
  pCharacteristic->notify();
}

void BLESendSystemCommonMessage2(byte type, byte data1, byte data2)
{
  uint8_t midiPacket[5];

  BLEMidiTimestamp(&midiPacket[0], &midiPacket[1]);
  midiPacket[2] = type;
  midiPacket[3] = data1;
  midiPacket[4] = data2;
  pCharacteristic->setValue(midiPacket, 5);
  pCharacteristic->notify();
}

void BLESendRealTimeMessage(byte type)
{
  uint8_t midiPacket[3];

  BLEMidiTimestamp(&midiPacket[0], &midiPacket[1]);
  midiPacket[2] = type;
  pCharacteristic->setValue(midiPacket, 3);
  pCharacteristic->notify();
}

void BLESendNoteOn(byte note, byte velocity, byte channel)
{
  BLESendChannelMessage2(midi::NoteOn, channel, note, velocity);
}

void BLESendNoteOff(byte note, byte velocity, byte channel)
{
  BLESendChannelMessage2(midi::NoteOff, channel, note, velocity);
}

void BLESendAfterTouchPoly(byte note, byte pressure, byte channel)
{
  BLESendChannelMessage2(midi::AfterTouchPoly, channel, note, pressure);
}

void BLESendControlChange(byte number, byte value, byte channel)
{
  BLESendChannelMessage2(midi::ControlChange, channel, number, value);
}

void BLESendProgramChange(byte number, byte channel)
{
  BLESendChannelMessage1(midi::ProgramChange, channel, number);
}

void BLESendAfterTouch(byte pressure, byte channel)
{
  BLESendChannelMessage1(midi::AfterTouchChannel, channel, pressure);
}

void BLESendPitchBend(int bend, byte channel)
{
  BLESendChannelMessage1(midi::PitchBend, channel, bend);
}

void BLESendSystemExclusive(const byte* array, unsigned size)
{
  /*
    Multiple Packet Encoding (SysEx Only)
    Only a SysEx (System Exclusive) message may span multiple BLE packets and is encoded as
    follows:
    1.  The SysEx start byte, which is a MIDI Status byte, is preceded by a timestamp byte.
    2.  Following the SysEx start byte, any number of Data bytes (up to the number of the
        remaining bytes in the packet) may be written.
    3.  Any remaining data may be sent in one or more SysEx continuation packets. A SysEx
        continuation packet begins with a header byte but does not contain a timestamp byte. It
        then contains one or more bytes of the SysEx data, up to the maximum packet length. This
        lack of a timestamp byte serves as a signal to the decoder of a SysEx continuation.
    4.  System Real-Time messages may appear at any point inside a SysEx message and must
        be preceded by a timestamp byte.
    5.  SysEx continuations for unterminated SysEx messages must follow either the packet’s
        header byte or a real-time byte.
    6.  Continue sending SysEx continuation packets until the entire message is transmitted.
    7.  In the last packet containing SysEx data, precede the EOX message (SysEx end byte),
        which is a MIDI Status byte, with a timestamp byte.
    Once a SysEx transfer has begun, only System Real-Time messages are allowed to precede its
    completion as follows:
    1.  A System Real-Time message interrupting a yet unterminated SysEx message must be
        preceded by its own timestamp byte.
    2.  SysEx continuations for unterminated SysEx messages must follow either the packet’s
        header byte or a real-time byte.
  */

  //
  //  to be implemented
  //
}

void BLESendTimeCodeQuarterFrame(byte data)
{
  BLESendSystemCommonMessage1(midi::TimeCodeQuarterFrame, data);
}

void BLESendSongPosition(unsigned int beats)
{
  BLESendSystemCommonMessage2(midi::SongPosition, beats >> 4, beats & 0x0f);
}

void BLESendSongSelect(byte songnumber)
{
  BLESendSystemCommonMessage1(midi::SongSelect, songnumber);
}

void BLESendTuneRequest(void)
{
  BLESendRealTimeMessage(midi::TuneRequest);
}

void BLESendClock(void)
{
  BLESendRealTimeMessage(midi::Clock);
}

void BLESendStart(void)
{
  BLESendRealTimeMessage(midi::Start);
}

void BLESendContinue(void)
{
  BLESendRealTimeMessage(midi::Continue);
}

void BLESendStop(void)
{
  BLESendRealTimeMessage(midi::Stop);
}

void BLESendActiveSensing(void)
{
  BLESendRealTimeMessage(midi::ActiveSensing);
}

void BLESendSystemReset(void)
{
  BLESendRealTimeMessage(midi::SystemReset);
}
#else
#define BLEMidiStart(...)
#define BLEMidiReceive(...)
#define BLESendNoteOn(...)
#define BLESendNoteOff(...)
#define BLESendAfterTouchPoly(...)
#define BLESendControlChange(...)
#define BLESendProgramChange(...)
#define BLESendAfterTouch(...)
#define BLESendPitchBend(...)
#define BLESendSystemExclusive(...)
#define BLESendTimeCodeQuarterFrame(...)
#define BLESendSongPosition(...)
#define BLESendSongSelect(...)
#define BLESendTuneRequest(...)
#define BLESendClock(...)
#define BLESendStart(...)
#define BLESendContinue(...)
#define BLESendStop(...) {}
#define BLESendActiveSensing(...)
#define BLESendSystemReset(...)
#endif


// Send messages to WiFI OSC interface

void OSCSendNoteOn(byte note, byte velocity, byte channel)
{
  String msg = "/pedalino/midi/note/";
  msg += note;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((float)(velocity / 127.0)).add((int32_t)channel).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendNoteOff(byte note, byte velocity, byte channel)
{
  String msg = "/pedalino/midi/note/";
  msg += note;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((float)0).add((int32_t)channel).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendAfterTouchPoly(byte note, byte pressure, byte channel)
{
  String msg = "/pedalino/midi/aftertouchpoly/";
  msg += note;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((float)(pressure / 127.0)).add((int32_t)channel).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendControlChange(byte number, byte value, byte channel)
{
  String msg = "/pedalino/midi/cc/";
  msg += number;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((float)(value / 127.0)).add((int32_t)channel).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendProgramChange(byte number, byte channel)
{
  String msg = "/pedalino/midi/pc/";
  msg += number;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((int32_t)channel).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendAfterTouch(byte pressure, byte channel)
{
  String msg = "/pedalino/midi/aftertouchchannel/";
  msg += channel;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((float)(pressure / 127.0)).add((int32_t)channel).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendPitchBend(int bend, byte channel)
{
  String msg = "/pedalino/midi/pitchbend/";
  msg += channel;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((float)((bend + 8192) / 16383.0)).add((int32_t)channel).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendSystemExclusive(const byte* array, unsigned size)
{
}

void OSCSendTimeCodeQuarterFrame(byte data)
{
}

void OSCSendSongPosition(unsigned int beats)
{
  String msg = "/pedalino/midi/songpostion/";
  msg += beats;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((int32_t)beats).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendSongSelect(byte songnumber)
{
  String msg = "/pedalino/midi/songselect/";
  msg += songnumber;
  OSCMessage oscMsg(msg.c_str());
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.add((int32_t)songnumber).send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendTuneRequest(void)
{
  OSCMessage oscMsg("/pedalino/midi/tunerequest/");
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendClock(void)
{
}

void OSCSendStart(void)
{
  OSCMessage oscMsg("/pedalino/midi/start/");
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendContinue(void)
{
  OSCMessage oscMsg("/pedalino/midi/continue/");
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendStop(void)
{
  OSCMessage oscMsg("/pedalino/midi/stop/");
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendActiveSensing(void)
{
  OSCMessage oscMsg("/pedalino/midi/activesensing/");
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.send(oscUDP).empty();
  oscUDP.endPacket();
}

void OSCSendSystemReset(void)
{
  OSCMessage oscMsg("/pedalino/midi/reset/");
  oscUDP.beginPacket(oscRemoteIp, oscRemotePort);
  oscMsg.send(oscUDP).empty();
  oscUDP.endPacket();
}


// Forward messages received from serial MIDI interface to WiFI interface

void OnSerialMidiNoteOn(byte channel, byte note, byte velocity)
{
  BLESendNoteOn(note, velocity, channel);
  AppleMIDI.noteOn(note, velocity, channel);
  OSCSendNoteOn(note, velocity, channel);
}

void OnSerialMidiNoteOff(byte channel, byte note, byte velocity)
{
  BLESendNoteOff(note, velocity, channel);
  AppleMIDI.noteOff(note, velocity, channel);
  OSCSendNoteOff(note, velocity, channel);
}

void OnSerialMidiAfterTouchPoly(byte channel, byte note, byte pressure)
{
  BLESendAfterTouchPoly(note, pressure, channel);
  AppleMIDI.polyPressure(note, pressure, channel);
  OSCSendAfterTouchPoly(note, pressure, channel);
}

void OnSerialMidiControlChange(byte channel, byte number, byte value)
{
  BLESendControlChange(number, value, channel);
  AppleMIDI.controlChange(number, value, channel);
  OSCSendControlChange(number, value, channel);
}

void OnSerialMidiProgramChange(byte channel, byte number)
{
  BLESendProgramChange(number, channel);
  AppleMIDI.programChange(number, channel);
  OSCSendProgramChange(number, channel);
}

void OnSerialMidiAfterTouchChannel(byte channel, byte pressure)
{
  BLESendAfterTouch(pressure, channel);
  AppleMIDI.afterTouch(pressure, channel);
  OSCSendAfterTouch(pressure, channel);
}

void OnSerialMidiPitchBend(byte channel, int bend)
{
  BLESendPitchBend(bend, channel);
  AppleMIDI.pitchBend(bend, channel);
  OSCSendPitchBend(bend, channel);
}

void OnSerialMidiSystemExclusive(byte* array, unsigned size)
{
  char json[size - 1];

  // Extract JSON string
  //
  memset(json, 0, size - 1);
  memcpy(json, &array[1], size - 2);
  DPRINTLN("JSON: %s", json);

  // Memory pool for JSON object tree.
  //
  StaticJsonBuffer<200> jsonBuffer;

  // Root of the object tree.
  //
  JsonObject& root = jsonBuffer.parseObject(json);

  // Test if parsing succeeds.
  if (root.success()) {
    // Fetch values.
    //
    const char *lcd1 = root["lcd1"];
    const char *lcd2 = root["lcd2"];
    if (lcd1) blynkLCD.print(0, 0, lcd1);
    if (lcd2) blynkLCD.print(0, 1, lcd2);
    else {
      BLESendSystemExclusive(array, size);
      AppleMIDI.sysEx(array, size);
      OSCSendSystemExclusive(array, size);
    }
  }
}

void OnSerialMidiTimeCodeQuarterFrame(byte data)
{
  BLESendTimeCodeQuarterFrame(data);
  AppleMIDI.timeCodeQuarterFrame(data);
  OSCSendTimeCodeQuarterFrame(data);
}

void OnSerialMidiSongPosition(unsigned int beats)
{
  BLESendSongPosition(beats);
  AppleMIDI.songPosition(beats);
  OSCSendSongPosition(beats);
}

void OnSerialMidiSongSelect(byte songnumber)
{
  BLESendSongSelect(songnumber);
  AppleMIDI.songSelect(songnumber);
  OSCSendSongSelect(songnumber);
}

void OnSerialMidiTuneRequest(void)
{
  BLESendTuneRequest();
  AppleMIDI.tuneRequest();
  OSCSendTuneRequest();
}

void OnSerialMidiClock(void)
{
  BLESendClock();
  AppleMIDI.clock();
  OSCSendClock();
}

void OnSerialMidiStart(void)
{
  BLESendStart();
  AppleMIDI.start();
  OSCSendStart();
}

void OnSerialMidiContinue(void)
{
  BLESendContinue();
  AppleMIDI._continue();
  OSCSendContinue();
}

void OnSerialMidiStop(void)
{
  BLESendStop();
  AppleMIDI.stop();
  OSCSendStop();
}

void OnSerialMidiActiveSensing(void)
{
  BLESendActiveSensing();
  AppleMIDI.activeSensing();
  OSCSendActiveSensing();
}

void OnSerialMidiSystemReset(void)
{
  BLESendSystemReset();
  AppleMIDI.reset();
  OSCSendSystemReset();
}


// Forward messages received from WiFI MIDI interface to serial MIDI interface

void OnAppleMidiConnected(uint32_t ssrc, char* name)
{
  appleMidiConnected  = true;
#ifdef PEDALINO_TELNET_DEBUG
  DEBUG("AppleMIDI Connected Session %d %s\n", ssrc, name);
#endif
}

void OnAppleMidiDisconnected(uint32_t ssrc)
{
  appleMidiConnected  = false;
#ifdef PEDALINO_TELNET_DEBUG
  DEBUG("AppleMIDI Disonnected Session ID %d\n", ssrc);
#endif
}

void OnAppleMidiNoteOn(byte channel, byte note, byte velocity)
{
  MIDI.sendNoteOn(note, velocity, channel);
  BLESendNoteOn(note, velocity, channel);
  OSCSendNoteOn(note, velocity, channel);
}

void OnAppleMidiNoteOff(byte channel, byte note, byte velocity)
{
  MIDI.sendNoteOff(note, velocity, channel);
  BLESendNoteOff(note, velocity, channel);
  OSCSendNoteOff(note, velocity, channel);
}

void OnAppleMidiReceiveAfterTouchPoly(byte channel, byte note, byte pressure)
{
  MIDI.sendAfterTouch(note, pressure, channel);
  BLESendAfterTouchPoly(note, pressure, channel);
  OSCSendAfterTouchPoly(note, pressure, channel);
}

void OnAppleMidiReceiveControlChange(byte channel, byte number, byte value)
{
  MIDI.sendControlChange(number, value, channel);
  BLESendControlChange(number, value, channel);
  OSCSendControlChange(number, value, channel);
}

void OnAppleMidiReceiveProgramChange(byte channel, byte number)
{
  MIDI.sendProgramChange(number, channel);
  BLESendProgramChange(number, channel);
  OSCSendProgramChange(number, channel);
}

void OnAppleMidiReceiveAfterTouchChannel(byte channel, byte pressure)
{
  MIDI.sendAfterTouch(pressure, channel);
  BLESendAfterTouch(pressure, channel);
  OSCSendAfterTouch(pressure, channel);
}

void OnAppleMidiReceivePitchBend(byte channel, int bend)
{
  MIDI.sendPitchBend(bend, channel);
  BLESendPitchBend(bend, channel);
  OSCSendPitchBend(bend, channel);
}

void OnAppleMidiReceiveSysEx(const byte * data, uint16_t size)
{
  MIDI.sendSysEx(size, data);
  BLESendSystemExclusive(data, size);
  OSCSendSystemExclusive(data, size);
}

void OnAppleMidiReceiveTimeCodeQuarterFrame(byte data)
{
  MIDI.sendTimeCodeQuarterFrame(data);
  BLESendTimeCodeQuarterFrame(data);
  OSCSendTimeCodeQuarterFrame(data);
}

void OnAppleMidiReceiveSongPosition(unsigned short beats)
{
  MIDI.sendSongPosition(beats);
  BLESendSongPosition(beats);
  OSCSendSongPosition(beats);
}

void OnAppleMidiReceiveSongSelect(byte songnumber)
{
  MIDI.sendSongSelect(songnumber);
  BLESendSongSelect(songnumber);
  OSCSendSongSelect(songnumber);
}

void OnAppleMidiReceiveTuneRequest(void)
{
  MIDI.sendTuneRequest();
  BLESendTuneRequest();
  OSCSendTuneRequest();
}

void OnAppleMidiReceiveClock(void)
{
  MIDI.sendRealTime(midi::Clock);
  BLESendClock();
  OSCSendClock();
}

void OnAppleMidiReceiveStart(void)
{
  MIDI.sendRealTime(midi::Start);
  BLESendStart();
  OSCSendStart();
}

void OnAppleMidiReceiveContinue(void)
{
  MIDI.sendRealTime(midi::Continue);
  BLESendContinue();
  OSCSendContinue();
}

void OnAppleMidiReceiveStop(void)
{
  MIDI.sendRealTime(midi::Stop);
  BLESendStop();
  OSCSendStop();
}

void OnAppleMidiReceiveActiveSensing(void)
{
  MIDI.sendRealTime(midi::ActiveSensing);
  BLESendActiveSensing();
  OSCSendActiveSensing();
}

void OnAppleMidiReceiveReset(void)
{
  MIDI.sendRealTime(midi::SystemReset);
  BLESendSystemReset();
  OSCSendSystemReset();
}


// Forward messages received from WiFI OSC interface to serial MIDI interface

void OnOscNoteOn(OSCMessage &msg)
{
  MIDI.sendNoteOn(msg.getInt(1), msg.getInt(2), msg.getInt(0));
}

void OnOscNoteOff(OSCMessage &msg)
{
  MIDI.sendNoteOff(msg.getInt(1), msg.getInt(2), msg.getInt(0));
}

void OnOscControlChange(OSCMessage &msg)
{
  MIDI.sendControlChange(msg.getInt(1), msg.getInt(2), msg.getInt(0));
}

void status_blink()
{
  WIFI_LED_ON();
  delay(50);
  WIFI_LED_OFF();
}

void ap_mode_start()
{
  WIFI_LED_OFF();

  WiFi.mode(WIFI_AP);
  boolean result = WiFi.softAP("Pedalino");
  DPRINTLN("AP mode started");
  DPRINTLN("Connect to 'Pedalino' wireless with no password");
}

void ap_mode_stop()
{
  if (WiFi.getMode() == WIFI_AP) {
    WiFi.softAPdisconnect();
    WIFI_LED_OFF();
  }
}

bool smart_config()
{
  // Return 'true' if SSID and password received within SMART_CONFIG_TIMEOUT seconds

  // Re-establish lost connection to the AP
  WiFi.setAutoReconnect(true);

  // Automatically connect on power on to the last used access point
  WiFi.setAutoConnect(true);

  // Waiting for SSID and password from from mobile app
  // SmartConfig works only in STA mode
  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();

  DPRINT("SmartConfig started");

  for (int i = 0; i < SMART_CONFIG_TIMEOUT && !WiFi.smartConfigDone(); i++) {
    status_blink();
    delay(950);
    DPRINT(".");
  }

  if (WiFi.smartConfigDone())
  {
    DPRINT("[SUCCESS]");
    DPRINTLN("SSID        : %s", WiFi.SSID().c_str());
    DPRINTLN("Password    : %s", WiFi.psk().c_str());

#ifdef ARDUINO_ARCH_ESP32
    int address = 0;
    EEPROM.writeString(address, WiFi.SSID());
    address += WiFi.SSID().length() + 1;
    EEPROM.writeString(address, WiFi.psk());
    EEPROM.commit();
    DPRINTLN("AP saved into EEPROM");
#endif
  }
  else
    DPRINT("[TIMEOUT]");

  if (WiFi.smartConfigDone())
  {
    WiFi.stopSmartConfig();
    return true;
  }
  else
  {
    WiFi.stopSmartConfig();
    return false;
  }
}

bool auto_reconnect()
{
  // Return 'true' if connected to the (last used) access point within WIFI_CONNECT_TIMEOUT seconds

  String ssid;
  String password;

#ifdef ARDUINO_ARCH_ESP8266
  ssid = WiFi.SSID();
  password = WiFi.psk();
#endif

#ifdef ARDUINO_ARCH_ESP32
  int address = 0;
  ssid = EEPROM.readString(address);
  address += ssid.length() + 1;
  password = EEPROM.readString(address);
#endif

  if (ssid.length() == 0) return false;

  DPRINTLN("Connecting to last used AP");
  DPRINTLN("SSID        : %s", ssid.c_str());
  DPRINTLN("Password    : %s", password.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  for (byte i = 0; i < WIFI_CONNECT_TIMEOUT * 2 && WiFi.status() != WL_CONNECTED; i++) {
    status_blink();
    delay(100);
    status_blink();
    delay(300);
    DPRINT(".");
  }

  WiFi.status() == WL_CONNECTED ? WIFI_LED_ON() : WIFI_LED_OFF();

  if (WiFi.status() == WL_CONNECTED)
    DPRINTLN("[SUCCESS]");
  else
    DPRINTLN("[TIMEOUT]");

  return WiFi.status() == WL_CONNECTED;
}

void wifi_connect()
{
  if (!auto_reconnect())       // WIFI_CONNECT_TIMEOUT seconds to reconnect to last used access point
    if (smart_config())        // SMART_CONFIG_TIMEOUT seconds to receive SmartConfig parameters
      auto_reconnect();        // WIFI_CONNECT_TIMEOUT seconds to connect to SmartConfig access point
  if (WiFi.status() != WL_CONNECTED) {
    ap_mode_start();          // switch to AP mode until next reboot
  }
  else
  {
    // connected to an AP

#ifdef ARDUINO_ARCH_ESP8266
    WiFi.hostname(host);
#endif
#ifdef ARDUINO_ARCH_ESP32
    WiFi.setHostname(host);
#endif

#if PEDALINO_DEBUG_SERIAL
    DPRINTLN("");
    WiFi.printDiag(SERIALDEBUG);
    DPRINTLN("");
#endif

    uint8_t macAddr[6];
    WiFi.macAddress(macAddr);
    DPRINTLN("BSSID       : %s", WiFi.BSSIDstr().c_str());
    DPRINTLN("RSSI        : %d dBm", WiFi.RSSI());
#ifdef ARDUINO_ARCH_ESP8266
    DPRINTLN("Hostname    : %s", WiFi.hostname().c_str());
#endif
#ifdef ARDUINO_ARCH_ESP32
    DPRINTLN("Hostname    : %s", WiFi.getHostname());
#endif
    DPRINTLN("STA         : %02X:%02X:%02X:%02X:%02X:%02X", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
    DPRINTLN("IP address  : %s", WiFi.localIP().toString().c_str());
    DPRINTLN("Subnet mask : %s", WiFi.subnetMask().toString().c_str());
    DPRINTLN("Gataway IP  : %s", WiFi.gatewayIP().toString().c_str());
    DPRINTLN("DNS 1       : %s", WiFi.dnsIP(0).toString().c_str());
    DPRINTLN("DNS 2       : %s", WiFi.dnsIP(1).toString().c_str());
  }

#ifdef ARDUINO_ARCH_ESP8266
  // Start LLMNR (Link-Local Multicast Name Resolution) responder
  LLMNR.begin(host);
  DPRINT("LLMNR responder started\n");
#endif

  // Start mDNS (Multicast DNS) responder (ping pedalino.local)
  if (MDNS.begin(host)) {
    DPRINTLN("mDNS responder started");
    MDNS.addService("apple-midi", "udp", 5004);
    MDNS.addService("osc",        "udp", oscLocalPort);
  }

  // Start firmawre update via HTTP (connect to http://pedalino.local/update)
#ifdef ARDUINO_ARCH_ESP8266
  httpUpdater.setup(&httpServer);
#endif
  httpServer.begin();
  MDNS.addService("http", "tcp", 80);
#ifdef PEDALINO_TELNET_DEBUG
  MDNS.addService("telnet", "tcp", 23);
#endif
  DPRINTLN("HTTP server started");
  DPRINTLN("Connect to http://pedalino.local/update for firmware update");

  // Calculate the broadcast address of local WiFi to broadcast OSC messages
  oscRemoteIp = WiFi.localIP();
  IPAddress localMask = WiFi.subnetMask();
  for (int i = 0; i < 4; i++)
    oscRemoteIp[i] |= (localMask[i] ^ B11111111);

  // Set incoming OSC messages port
  oscUDP.begin(oscLocalPort);
  DPRINTLN("OSC server started");
#ifdef ARDUINO_ARCH_ESP8266
  DPRINT("Local port: ");
  DPRINTLN(oscUDP.localPort());
#endif
}


void midi_connect()
{
  // Connect the handle function called upon reception of a MIDI message from serial MIDI interface
  MIDI.setHandleNoteOn(OnSerialMidiNoteOn);
  MIDI.setHandleNoteOff(OnSerialMidiNoteOff);
  MIDI.setHandleAfterTouchPoly(OnSerialMidiAfterTouchPoly);
  MIDI.setHandleControlChange(OnSerialMidiControlChange);
  MIDI.setHandleProgramChange(OnSerialMidiProgramChange);
  MIDI.setHandleAfterTouchChannel(OnSerialMidiAfterTouchChannel);
  MIDI.setHandlePitchBend(OnSerialMidiPitchBend);
  MIDI.setHandleSystemExclusive(OnSerialMidiSystemExclusive);
  MIDI.setHandleTimeCodeQuarterFrame(OnSerialMidiTimeCodeQuarterFrame);
  MIDI.setHandleSongPosition(OnSerialMidiSongPosition);
  MIDI.setHandleSongSelect(OnSerialMidiSongSelect);
  MIDI.setHandleTuneRequest(OnSerialMidiTuneRequest);
  MIDI.setHandleClock(OnSerialMidiClock);
  MIDI.setHandleStart(OnSerialMidiStart);
  MIDI.setHandleContinue(OnSerialMidiContinue);
  MIDI.setHandleStop(OnSerialMidiStop);
  MIDI.setHandleActiveSensing(OnSerialMidiActiveSensing);
  MIDI.setHandleSystemReset(OnSerialMidiSystemReset);

  // Initiate serial MIDI communications, listen to all channels
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  // Create a session and wait for a remote host to connect to us
  AppleMIDI.begin("Pedalino(TM)");

  AppleMIDI.OnConnected(OnAppleMidiConnected);
  AppleMIDI.OnDisconnected(OnAppleMidiDisconnected);

  // Connect the handle function called upon reception of a MIDI message from WiFi MIDI interface
  AppleMIDI.OnReceiveNoteOn(OnAppleMidiNoteOn);
  AppleMIDI.OnReceiveNoteOff(OnAppleMidiNoteOff);
  AppleMIDI.OnReceiveAfterTouchPoly(OnAppleMidiReceiveAfterTouchPoly);
  AppleMIDI.OnReceiveControlChange(OnAppleMidiReceiveControlChange);
  AppleMIDI.OnReceiveProgramChange(OnAppleMidiReceiveProgramChange);
  AppleMIDI.OnReceiveAfterTouchChannel(OnAppleMidiReceiveAfterTouchChannel);
  AppleMIDI.OnReceivePitchBend(OnAppleMidiReceivePitchBend);
  AppleMIDI.OnReceiveSysEx(OnAppleMidiReceiveSysEx);
  AppleMIDI.OnReceiveTimeCodeQuarterFrame(OnAppleMidiReceiveTimeCodeQuarterFrame);
  AppleMIDI.OnReceiveSongPosition(OnAppleMidiReceiveSongPosition);
  AppleMIDI.OnReceiveSongSelect(OnAppleMidiReceiveSongSelect);
  AppleMIDI.OnReceiveTuneRequest(OnAppleMidiReceiveTuneRequest);
  AppleMIDI.OnReceiveClock(OnAppleMidiReceiveClock);
  AppleMIDI.OnReceiveStart(OnAppleMidiReceiveStart);
  AppleMIDI.OnReceiveContinue(OnAppleMidiReceiveContinue);
  AppleMIDI.OnReceiveStop(OnAppleMidiReceiveStop);
  AppleMIDI.OnReceiveActiveSensing(OnAppleMidiReceiveActiveSensing);
  AppleMIDI.OnReceiveReset(OnAppleMidiReceiveReset);
}

BLYNK_CONNECTED() {
  // This function is called when hardware connects to Blynk Cloud or private server.
  DPRINTLN("Connected to Blynk");
  blynkLCD.clear();
  Blynk.virtualWrite(V90, (appleMidiConnected) ? 1 : 0);
  Blynk.virtualWrite(V91, 0);
  Blynk.virtualWrite(V95, 0);
}

BLYNK_APP_CONNECTED() {
  //  This function is called every time Blynk app client connects to Blynk server.
  DPRINTLN("Blink App connected");
}

BLYNK_APP_DISCONNECTED() {
  // This function is called every time the Blynk app disconnects from Blynk Cloud or private server.
  DPRINTLN("Blink App disconnected");
}

BLYNK_READ(V40) {
  Blynk.virtualWrite(41, 4);
}

BLYNK_WRITE(V40) {
  int interface = param.asInt();
  switch (interface) {
    case 1: // USB
    case 2: // DIN
    case 3: // RTP
    case 4: // BLE
      //currentInterface = interface - 1;
      DPRINTLN("%d", interface);
      break;
  }
}

BLYNK_READ(V90) {
  Blynk.virtualWrite(V90, (appleMidiConnected) ? 0 : 1);
}

BLYNK_WRITE(V90) {
  int wifiOnOff = param.asInt();
  switch (wifiOnOff) {
    case 0: // OFF
      WiFi.mode(WIFI_OFF);
      DPRINTLN("WiFi Off Mode");
      break;

    case 1: // ON
      wifi_connect();
      Blynk.connect();
      break;
  }
}

BLYNK_READ(V91) {
  Blynk.virtualWrite(90, 0);
}

BLYNK_WRITE(V91) {
  int scan = param.asInt();
  if (scan) {
    DPRINTLN("WiFi Scan started");
    // WiFi.scanNetworks will return the number of networks found
    int n = WiFi.scanNetworks();
    DPRINTLN("WiFi Scan done");
    if (n == 0) {
      DPRINTLN("No networks found");
    } else {
      DPRINTLN("%d networks found", n);
      BlynkParamAllocated items(128); // list length, in bytes
      for (int i = 0; i < n; i++) {
        DPRINTLN("%2d. %s %s %d dBm", i + 1, WiFi.BSSIDstr().c_str(), WiFi.SSID(i).c_str(), WiFi.RSSI(i));
        items.add(WiFi.SSID(i).c_str());
      }
      Blynk.setProperty(V92, "labels", items);
      Blynk.virtualWrite(V91, 0);
      DPRINTLN("Blink updated");
    }
  }
}

BLYNK_WRITE(V95) {
  int smartconfig = param.asInt();
  if (smartconfig) {
    smart_config();
    if (auto_reconnect()) {
      Blynk.connect();
    }
  }
}

void setup()
{
  SERIALDEBUG.begin(115200);
  SERIALDEBUG.setDebugOutput(true);

  DPRINTLN("  __________           .___      .__  .__                   ___ ________________    ___");
  DPRINTLN("  \\______   \\ ____   __| _/____  |  | |__| ____   ____     /  / \\__    ___/     \\   \\  \\");
  DPRINTLN("   |     ___// __ \\ / __ |\\__  \\ |  | |  |/    \\ /  _ \\   /  /    |    | /  \\ /  \\   \\  \\");
  DPRINTLN("   |    |   \\  ___// /_/ | / __ \\|  |_|  |   |  (  <_> ) (  (     |    |/    Y    \\   )  )");
  DPRINTLN("   |____|    \\___  >____ |(____  /____/__|___|  /\\____/   \\  \\    |____|\\____|__  /  /  /");
  DPRINTLN("                 \\/     \\/     \\/             \\/           \\__\\                 \\/  /__/");
  DPRINTLN("                                                                       (c) 2018 alf45star");

#ifdef ARDUINO_ARCH_ESP32
  esp_log_level_set("*",            ESP_LOG_ERROR);
  esp_log_level_set("wifi",         ESP_LOG_WARN);
  esp_log_level_set("ble",          ESP_LOG_DEBUG);
  esp_log_level_set("PedalinoESP",  ESP_LOG_INFO);

  DPRINTLN("Testing EEPROM Library");
  if (!EEPROM.begin(128)) {
    DPRINTLN("Failed to initialise EEPROM");
    DPRINTLN("Restarting...");
    delay(1000);
    ESP.restart();
  }
#endif

  pinMode(WIFI_LED, OUTPUT);

#ifdef ARDUINO_ARCH_ESP32
  pinMode(BLE_LED, OUTPUT);
  SerialMIDI.begin(SERIALMIDI_BAUD_RATE, SERIAL_8N1, SERIALMIDI_RX, SERIALMIDI_TX);
#endif

  // BLE MIDI server advertising
  BLEMidiStart();

  // Write SSID/password to flash only if currently used values do not match what is already stored in flash
  WiFi.persistent(false);
  wifi_connect();

  // Connect to Blynk
  Blynk.config(blynkAuthToken);
  Blynk.connect();

  SERIALDEBUG.flush();

#ifdef PEDALINO_TELNET_DEBUG
  // Initialize the telnet server of RemoteDebug
  Debug.begin(host);              // Initiaze the telnet server
  Debug.setResetCmdEnabled(true); // Enable the reset command
#endif

  // On receiving MIDI data callbacks setup
  midi_connect();
}

void loop()
{
  if (!appleMidiConnected) WIFI_LED_OFF();
  if (!bleMidiConnected)  BLE_LED_OFF();
  if (appleMidiConnected ||  bleMidiConnected) {
    // led fast blinking (5 times per second)
    if (millis() - wifiLastOn > 200) {
      if (bleMidiConnected) BLE_LED_ON();
      if (appleMidiConnected) WIFI_LED_ON();
      wifiLastOn = millis();
    }
    else if (millis() - wifiLastOn > 100) {
      BLE_LED_OFF();
      WIFI_LED_OFF();
    }
  }
  else
    // led always on if connected to an AP or one or more client connected the the internal AP
    switch (WiFi.getMode()) {
      case WIFI_STA:
        WiFi.isConnected() ? WIFI_LED_ON() : WIFI_LED_OFF();
        break;
      case WIFI_AP:
        WiFi.softAPgetStationNum() > 0 ? WIFI_LED_ON() : WIFI_LED_OFF();
        break;
      default:
        WIFI_LED_OFF();
        break;
    }

  // Listen to incoming messages from Arduino

#ifdef PEDALINO_TELNET_DEBUG
  if (MIDI.read()) {
    DEBUG("Received MIDI message:   Type 0x % 02x   Data1 0x % 02x   Data2 0x % 02x   Channel 0x % 02x\n", MIDI.getType(), MIDI.getData1(), MIDI.getData2(), MIDI.getChannel());
  }
#else
  MIDI.read();
#endif

  // Listen to incoming AppleMIDI messages from WiFi
  AppleMIDI.run();

  // Listen to incoming OSC messages from WiFi
  int size = oscUDP.parsePacket();

  if (size > 0) {
    while (size--) oscMsg.fill(oscUDP.read());
    if (!oscMsg.hasError()) {
      oscMsg.dispatch(" / pedalino / midi / noteOn",        OnOscNoteOn);
      oscMsg.dispatch(" / pedalino / midi / noteOff",       OnOscNoteOff);
      oscMsg.dispatch(" / pedalino / midi / controlChange", OnOscControlChange);
    } else {
      oscError = oscMsg.getError();
#ifdef PEDALINO_TELNET_DEBUG
      DEBUG("OSC error: ");
      //DEBUG(oscError);
      DEBUG("\n");
#endif
    }
  }

  // Run HTTP Updater
  httpServer.handleClient();

  Blynk.run();

#ifdef PEDALINO_TELNET_DEBUG
  // Remote debug over telnet
  Debug.handle();
#endif
}

