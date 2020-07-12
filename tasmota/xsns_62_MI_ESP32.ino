/*
  xsns_62_MI_ESP32.ino - MI-BLE-sensors via ESP32 support for Tasmota

  Copyright (C) 2020  Christian Baars and Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.


  --------------------------------------------------------------------------------------------
  Version yyyymmdd  Action    Description
  --------------------------------------------------------------------------------------------
  0.9.1.0 20200712  changed - add lights and yeerc, add pure passive mode with decryption,
                              lots of refactoring
  -------
  0.9.0.1 20200706  changed - adapt to new NimBLE-API, tweak scan process
  -------
  0.9.0.0 20200413  started - initial development by Christian Baars
                    forked  - from arendst/tasmota            - https://github.com/arendst/Tasmota

*/
#ifdef ESP32                       // ESP32 only. Use define USE_HM10 for ESP8266 support

#ifdef USE_MI_ESP32

#define XSNS_62                    62
#define USE_MI_DECRYPTION

#include <NimBLEDevice.h>
#include <vector>
#ifdef USE_MI_DECRYPTION
#include <mbedtls/ccm.h>
#endif //USE_MI_DECRYPTION

void MI32scanEndedCB(NimBLEScanResults results);
void MI32notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify);


struct {
  uint16_t perPage = 4;
  uint32_t period;             // set manually in addition to TELE-period, is set to TELE-period after start
  struct {
    uint32_t init:1;
    uint32_t connected:1;
    uint32_t autoScan:1;
    uint32_t canScan:1;
    uint32_t runningScan:1;
    uint32_t canConnect:1;
    uint32_t willConnect:1;
    uint32_t readingDone:1;
    uint32_t shallSetTime:1;
    uint32_t willSetTime:1;
    uint32_t shallReadBatt:1;
    uint32_t willReadBatt:1;
    uint32_t shallSetUnit:1;
    uint32_t willSetUnit:1;
    uint32_t triggeredTele:1;
    uint32_t shallClearResults:1;
    uint32_t directMQTT:1;    // TODO: direct bridging of every single sensor message

  } mode;
  struct {
    uint8_t sensor;           // points to to the number 0...255
  } state;
} MI32;

#pragma pack(1)  // byte-aligned structures to read the sensor data

  struct {
    uint16_t temp;
    uint8_t hum;
    uint16_t volt; // LYWSD03 only
  } LYWSD0x_HT;
  struct {
    uint8_t spare;
    uint16_t temp;
    uint16_t hum;
  } CGD1_HT;
  struct {
    uint16_t temp;
    uint8_t spare;
    uint32_t lux;
    uint8_t moist;
    uint16_t fert;
  } Flora_TLMF; // temperature, lux, moisture, fertility


struct mi_beacon_t{
  uint16_t frame;
  uint16_t productID;
  uint8_t counter;
  uint8_t MAC[6];
  uint8_t spare;
  uint8_t type;
  uint8_t ten;
  uint8_t size;
  union {
    struct{ //0d
      uint16_t temp;
      uint16_t hum;
    }HT;
    uint8_t bat; //0a
    uint16_t temp; //04
    uint16_t hum; //06
    uint32_t lux; //07
    uint8_t moist; //08
    uint16_t fert; //09
    uint32_t NMT; //17
    struct{ //01
      uint16_t num;
      uint8_t longPress; 
    }Btn; 
  };
  uint8_t padding[12];
};

struct cg_packet_t {
  uint16_t frameID;
  uint8_t MAC[6];
  uint16_t mode;
  union {
    struct {
    int16_t temp;  // -9 - 59 °C
    uint16_t hum;
    };
    uint8_t bat;
  };
};

struct encPacket_t{
  // the packet is longer, but this part is enough to decrypt
  uint16_t PID;
  uint8_t frameCnt;
  uint8_t MAC[6];
  uint8_t payload[16]; // only a pointer to the address, size is variable
};

union mi_bindKey_t{
  struct{
    uint8_t key[16];
    uint8_t MAC[6];
    };
  uint8_t buf[22];
};

#pragma pack(0)

struct mi_sensor_t{
  uint8_t type; //Flora = 1; MI-HT_V1=2; LYWSD02=3; LYWSD03=4; CGG1=5; CGD1=6
  uint8_t lastCnt; //device generated counter of the packet
  uint8_t shallSendMQTT;
  uint8_t MAC[6];
  // uint8_t showedUp;
  uint32_t lastTime;
  uint32_t lux;
  float temp; //Flora, MJ_HT_V1, LYWSD0x, CGx
  union {
    struct {
      float moisture;
      float fertility;
      char firmware[6]; // actually only for FLORA but hopefully we can add for more devices
    }; // Flora
    struct {
      float hum;
    }; // MJ_HT_V1, LYWSD0x
    struct {
      uint16_t events; //"alarms" since boot
      uint32_t NMT;    // no motion time in seconds for the MJYD2S
      uint8_t eventType; //internal type of actual event for the MJYD2S -> 1: PIR, 2: No PIR, 3: NMT
    };
    uint16_t Btn;
  };
  union {
      uint8_t bat; // many values seem to be hard-coded garbage (LYWSD0x, GCD1)
  };
};

std::vector<mi_sensor_t> MIBLEsensors;
std::vector<mi_bindKey_t> MIBLEbindKeys;

static BLEScan* MI32Scan;

/*********************************************************************************************\
 * constants
\*********************************************************************************************/

#define D_CMND_MI32 "MI32"

const char S_JSON_MI32_COMMAND_NVALUE[] PROGMEM = "{\"" D_CMND_MI32 "%s\":%d}";
const char S_JSON_MI32_COMMAND[] PROGMEM        = "{\"" D_CMND_MI32 "%s%s\"}";
const char kMI32_Commands[] PROGMEM             = "Period|Time|Page|Battery|Unit|Key";

#define FLORA       1
#define MJ_HT_V1    2
#define LYWSD02     3
#define LYWSD03MMC  4
#define CGG1        5
#define CGD1        6
#define NLIGHT      7
#define MJYD2S      8
#define YEERC       9

const uint16_t kMI32DeviceID[9]={ 0x0098, // Flora
                                  0x01aa, // MJ_HT_V1
                                  0x045b, // LYWSD02
                                  0x055b, // LYWSD03
                                  0x0347, // CGG1
                                  0x0576, // CGD1
                                  0x03dd, // NLIGHT
                                  0x07f6, // MJYD2S
                                  0x0153  // yee-rc
                                  };

const char kMI32DeviceType1[] PROGMEM = "Flora";
const char kMI32DeviceType2[] PROGMEM = "MJ_HT_V1";
const char kMI32DeviceType3[] PROGMEM = "LYWSD02";
const char kMI32DeviceType4[] PROGMEM = "LYWSD03";
const char kMI32DeviceType5[] PROGMEM = "CGG1";
const char kMI32DeviceType6[] PROGMEM = "CGD1";
const char kMI32DeviceType7[] PROGMEM = "NLIGHT";
const char kMI32DeviceType8[] PROGMEM = "MJYD2S";
const char kMI32DeviceType9[] PROGMEM = "YEERC";
const char * kMI32DeviceType[] PROGMEM = {kMI32DeviceType1,kMI32DeviceType2,kMI32DeviceType3,kMI32DeviceType4,kMI32DeviceType5,kMI32DeviceType6,kMI32DeviceType7,kMI32DeviceType8,kMI32DeviceType9};

/*********************************************************************************************\
 * enumerations
\*********************************************************************************************/

enum MI32_Commands {          // commands useable in console or rules
  CMND_MI32_PERIOD,           // set period like TELE-period in seconds between read-cycles
  CMND_MI32_TIME,             // set LYWSD02-Time from ESP8266-time
  CMND_MI32_PAGE,             // sensor entries per web page, which will be shown alternated
  CMND_MI32_BATTERY,          // read all battery levels
  CMND_MI32_UNIT,             // toggles the displayed unit between C/F (LYWSD02)
  CMND_MI32_KEY               // add bind key to a mac for packet decryption
  };

enum MI32_TASK {
       MI32_TASK_SCAN = 0,
       MI32_TASK_CONN = 1,
       MI32_TASK_TIME = 2,
       MI32_TASK_BATT = 3,
       MI32_TASK_UNIT = 4,
};

/*********************************************************************************************\
 * Classes
\*********************************************************************************************/

class MI32SensorCallback : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) {
    AddLog_P2(LOG_LEVEL_DEBUG,PSTR("connected %s"), kMI32DeviceType[(MIBLEsensors[MI32.state.sensor].type)-1]);
    MI32.mode.willConnect = 0;
    MI32.mode.connected = 1;
  }
  void onDisconnect(NimBLEClient* pclient) {
    MI32.mode.connected = 0;
    AddLog_P2(LOG_LEVEL_DEBUG,PSTR("disconnected %s"), kMI32DeviceType[(MIBLEsensors[MI32.state.sensor].type)-1]);
  }
  bool onConnParamsUpdateRequest(NimBLEClient* MI32Client, const ble_gap_upd_params* params) {
    if(params->itvl_min < 24) { /** 1.25ms units */
      return false;
    } else if(params->itvl_max > 40) { /** 1.25ms units */
      return false;
    } else if(params->latency > 2) { /** Number of intervals allowed to skip */
      return false;
    } else if(params->supervision_timeout > 100) { /** 10ms units */
      return false;
    }
    return true;
  }
};

class MI32AdvCallbacks: public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
    // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Advertised Device: %s Buffer: %u"),advertisedDevice->getAddress().toString().c_str(),advertisedDevice->getServiceData().length());
    if (advertisedDevice->getServiceData().length() == 0) {
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("No Xiaomi Device: %s Buffer: %u"),advertisedDevice->getAddress().toString().c_str(),advertisedDevice->getServiceData().length());
      MI32Scan->erase(advertisedDevice->getAddress());
      return;
    }
    uint16_t uuid = advertisedDevice->getServiceDataUUID().getNative()->u16.value;
    // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("UUID: %x"),uuid);
    uint8_t addr[6];
    memcpy(addr,advertisedDevice->getAddress().getNative(),6);
    MI32_ReverseMAC(addr);
    if(uuid==0xfe95) {
      MI32ParseResponse((char*)advertisedDevice->getServiceData().data(),advertisedDevice->getServiceData().length(), addr);
    }
    else if(uuid==0xfdcd) {
      MI32parseCGD1Packet((char*)advertisedDevice->getServiceData().data(),advertisedDevice->getServiceData().length(), addr);
    }
    else {
      MI32Scan->erase(advertisedDevice->getAddress());
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("No Xiaomi Device: %s Buffer: %u"),advertisedDevice->getAddress().toString().c_str(),advertisedDevice->getServiceData().length());
    }
  };
};


static MI32AdvCallbacks MI32ScanCallbacks;
static MI32SensorCallback MI32SensorCB;
static NimBLEClient* MI32Client;

/*********************************************************************************************\
 * BLE callback functions
\*********************************************************************************************/

void MI32scanEndedCB(NimBLEScanResults results){
  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Scan ended"));
  MI32.mode.runningScan = 0;
}

void MI32notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify){
    AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Notified length: %u"),length);
    switch(MIBLEsensors[MI32.state.sensor].type){
      case LYWSD03MMC: case LYWSD02:
        MI32readHT_LY((char*)pData);
        MI32.mode.readingDone = 1;
        break;
      default:
        MI32.mode.readingDone = 1;
        break;
    }
}
/*********************************************************************************************\
 * Helper functions
\*********************************************************************************************/

void MI32_ReverseMAC(uint8_t _mac[]){
  uint8_t _reversedMAC[6];
  for (uint8_t i=0; i<6; i++){
    _reversedMAC[5-i] = _mac[i];
  }
  memcpy(_mac,_reversedMAC, sizeof(_reversedMAC));
}

#ifdef USE_MI_DECRYPTION
void MI32AddKey(char* payload){
  mi_bindKey_t keyMAC;
  memset(keyMAC.buf,0,sizeof(keyMAC));
  MI32KeyMACStringToBytes(payload,keyMAC.buf);
  bool unknownKey = true;
  for(uint32_t i=0; i<MIBLEbindKeys.size(); i++){
    if(memcmp(keyMAC.MAC,MIBLEbindKeys[i].MAC,sizeof(keyMAC.MAC))==0){
      AddLog_P2(LOG_LEVEL_DEBUG,PSTR("known key"));
      unknownKey=false;
    }
  }
  if(unknownKey){
    AddLog_P2(LOG_LEVEL_DEBUG,PSTR("New key"));
    MIBLEbindKeys.push_back(keyMAC);
  }
}

/**
 * @brief Convert combined key-MAC-string to 
 *
 * @param _string input string in format: AABBCCDDEEFF... (upper case!), must be 44 chars!!
 * @param _mac  target byte array with fixed size of 16 + 6 
 */
void MI32KeyMACStringToBytes(char* _string,uint8_t _keyMAC[]) { //uppercase
    uint32_t index = 0;
    while (index < 44) {
        char c = _string[index];
        uint8_t value = 0;
        if(c >= '0' && c <= '9')
          value = (c - '0');
        else if (c >= 'A' && c <= 'F')
          value = (10 + (c - 'A'));
        _keyMAC[(index/2)] += value << (((index + 1) % 2) * 4);
        index++;
    }
    // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("MI32:  %s to:"),_string);
    // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("MI32:  key-array: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X"),_keyMAC[0],_keyMAC[1],_keyMAC[2],_keyMAC[3],_keyMAC[4],_keyMAC[5],_keyMAC[6],_keyMAC[7],_keyMAC[8],_keyMAC[9],_keyMAC[10],_keyMAC[11],_string,_keyMAC[12],_keyMAC[13],_keyMAC[14],_keyMAC[15]);
    // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("MI32: MAC-array: %02X%02X%02X%02X%02X%02X"),_keyMAC[16],_keyMAC[17],_keyMAC[18],_keyMAC[19],_keyMAC[20],_keyMAC[21]);
}

/**
 * @brief Decrypts payload in place
 * 
 * @param _buf - pointer to the buffer at position of PID
 * @param _bufSize - buffersize (last position is last byte of TAG)
 * @param _type - sensor type
 * @return int - error code, 0 for success
 */
int MI32_decryptPacket(char *_buf, uint16_t _bufSize, uint32_t _type){
  encPacket_t *packet = (encPacket_t*)_buf;
  // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("to decrypt: %02x %02x %02x %02x %02x %02x %02x %02x"),(uint8_t)_buf[0],(uint8_t)_buf[1],(uint8_t)_buf[2],(uint8_t)_buf[3],(uint8_t)_buf[4],(uint8_t)_buf[5],(uint8_t)_buf[6],(uint8_t)_buf[7]);
  // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("       : %02x %02x %02x %02x %02x %02x %02x %02x"),(uint8_t)_buf[8],(uint8_t)_buf[9],(uint8_t)_buf[10],(uint8_t)_buf[11],(uint8_t)_buf[12],(uint8_t)_buf[13],(uint8_t)_buf[14],(uint8_t)_buf[15]);
  // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("       : %02x %02x %02x %02x %02x %02x %02x %02x"),(uint8_t)_buf[16],(uint8_t)_buf[17],(uint8_t)_buf[18],(uint8_t)_buf[19],(uint8_t)_buf[20],(uint8_t)_buf[21],(uint8_t)_buf[22],(uint8_t)_buf[23]);
  // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("as packet:  MAC: %02x  %02x  %02x  %02x  %02x  %02x"), packet->MAC[0], packet->MAC[1], packet->MAC[2], packet->MAC[3], packet->MAC[4], packet->MAC[5]);

  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Decrypt Size of Buffer: %u"), _bufSize);
  int ret = 0;
  unsigned char output[10] = {0};
  uint8_t nonce[12];
  uint8_t tag[4 ];
  const unsigned char authData[1] = {0x11};

  // nonce: device MAC, device type, frame cnt, ext. cnt
  for (uint32_t i = 0; i<6; i++){
    nonce[i] = packet->MAC[i];
  }
  memcpy((uint8_t*)&nonce+6,(uint8_t*)&packet->PID,2);
  nonce[8] = packet->frameCnt;
  memcpy((uint8_t*)&nonce+9,(char*)&_buf[_bufSize-9],3);
  // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("nonceCnt1 and 2: %02x %02x %02x"),nonce[9],nonce[10],nonce[11]);
  memcpy((uint8_t*)&tag,(char*)&_buf[_bufSize-6],4);
  // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("tag: %02x %02x %02x %02x"),tag[0],tag[1],tag[2],tag[3]);

  MI32_ReverseMAC(packet->MAC);
  uint8_t _bindkey[16] = {0x0};
  for(uint32_t i=0; i<MIBLEbindKeys.size(); i++){
    if(memcmp(packet->MAC,MIBLEbindKeys[i].MAC,sizeof(packet->MAC))==0){
      AddLog_P2(LOG_LEVEL_DEBUG,PSTR("have key"));
      memcpy(_bindkey,MIBLEbindKeys[i].key,sizeof(_bindkey));
    }
    else{
    // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mac in packet: %02x  %02x  %02x  %02x  %02x  %02x"), packet->MAC[0], packet->MAC[1], packet->MAC[2], packet->MAC[3], packet->MAC[4], packet->MAC[5]);
    // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mac in vector: %02x  %02x  %02x  %02x  %02x  %02x"), MIBLEbindKeys[i].MAC[0], MIBLEbindKeys[i].MAC[1], MIBLEbindKeys[i].MAC[2], MIBLEbindKeys[i].MAC[3], MIBLEbindKeys[i].MAC[4], MIBLEbindKeys[i].MAC[5]);
    }
  }

  // init
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  // set bind key
  ret = mbedtls_ccm_setkey(&ctx,
    MBEDTLS_CIPHER_ID_AES,
    _bindkey,
    16 * 8 //bits
  );
  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("set key: %i, MAC: %02x  %02x  %02x  %02x  %02x  %02x"),ret, packet->MAC[0], packet->MAC[1], packet->MAC[2], packet->MAC[3], packet->MAC[4], packet->MAC[5]);

/*int mbedtls_ccm_auth_decrypt( mbedtls_ccm_context *ctx, size_t length,
                      const unsigned char *iv, size_t iv_len,
                      const unsigned char *add, size_t add_len,
                      const unsigned char *input, unsigned char *output,
                      const unsigned char *tag, size_t tag_len )
*/
  ret = mbedtls_ccm_auth_decrypt(&ctx,_bufSize-18,
    (const unsigned char*)&nonce, sizeof(nonce),
    authData, sizeof(authData),
    (const unsigned char*)packet->payload, output,
    tag,sizeof(tag));

  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Decrypted %i: %02x  %02x  %02x  %02x  %02x %02x  %02x"), ret, output[0],output[1],output[2],output[3],output[4],output[5],output[6]);
  // put decrypted data in place
  memcpy((uint8_t*)(packet->payload)+1,output,_bufSize-18);
  // clean up
  mbedtls_ccm_free(&ctx);
  return ret;
}
#endif // USE_MI_DECRYPTION

/*********************************************************************************************\
 * common functions
\*********************************************************************************************/


/**
 * @brief Return the slot number of a known sensor or return create new sensor slot
 *
 * @param _MAC     BLE address of the sensor
 * @param _type       Type number of the sensor
 * @return uint32_t   Known or new slot in the sensors-vector
 */
uint32_t MIBLEgetSensorSlot(uint8_t (&_MAC)[6], uint16_t _type, uint8_t counter){

  DEBUG_SENSOR_LOG(PSTR("%s: will test ID-type: %x"),D_CMND_MI32, _type);
  bool _success = false;
  for (uint32_t i=0;i<9;i++){ // i < sizeof(kMI32DeviceID) gives compiler warning
    if(_type == kMI32DeviceID[i]){
      DEBUG_SENSOR_LOG(PSTR("MI32: ID is type %u"), i);
      _type = i+1;
      _success = true;
    }
    else {
      DEBUG_SENSOR_LOG(PSTR("%s: ID-type is not: %x"),D_CMND_MI32,kMI32DeviceID[i]);
    }
  }
  if(!_success) return 0xff;

  DEBUG_SENSOR_LOG(PSTR("%s: vector size %u"),D_CMND_MI32, MIBLEsensors.size());
  for(uint32_t i=0; i<MIBLEsensors.size(); i++){
    if(memcmp(_MAC,MIBLEsensors[i].MAC,sizeof(_MAC))==0){
      DEBUG_SENSOR_LOG(PSTR("%s: known sensor at slot: %u"),D_CMND_MI32, i);
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Counters: %x %x"),MIBLEsensors[i].lastCnt, counter);
      if(MIBLEsensors[i].lastCnt==counter) {
        // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Old packet"));
        return 0xff; // packet received before, stop here
      }
      return i;
    }
    DEBUG_SENSOR_LOG(PSTR("%s: i: %x %x %x %x %x %x"),D_CMND_MI32, MIBLEsensors[i].MAC[5], MIBLEsensors[i].MAC[4],MIBLEsensors[i].MAC[3],MIBLEsensors[i].MAC[2],MIBLEsensors[i].MAC[1],MIBLEsensors[i].MAC[0]);
    DEBUG_SENSOR_LOG(PSTR("%s: n: %x %x %x %x %x %x"),D_CMND_MI32, _MAC[5], _MAC[4], _MAC[3],_MAC[2],_MAC[1],_MAC[0]);
  }
  DEBUG_SENSOR_LOG(PSTR("%s: found new sensor"),D_CMND_MI32);
  mi_sensor_t _newSensor;
  memcpy(_newSensor.MAC,_MAC, sizeof(_MAC));
  _newSensor.type = _type;

  _newSensor.temp =NAN;
  _newSensor.bat=0x00;
  _newSensor.lux = 0x00ffffff;
  switch (_type)
    {
    case FLORA:
      _newSensor.moisture =NAN;
      _newSensor.fertility =NAN;
      break;
    case 2: case 3: case 4: case 5: case 6:
      _newSensor.hum=NAN;
      break;
    default:
      _newSensor.NMT=0;
      _newSensor.events=0x00;
      _newSensor.eventType=0x00;
      break;
    }
  MIBLEsensors.push_back(_newSensor);
  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: new %s at slot: %u"),D_CMND_MI32, kMI32DeviceType[_type-1],MIBLEsensors.size()-1);
  return MIBLEsensors.size()-1;
};

/**
 * @brief trigger real-time message for PIR or RC
 * 
 */
void MI32triggerTele(void){
    MI32.mode.triggeredTele = true;
    mqtt_data[0] = '\0';
    if (MqttShowSensor()) {
      MqttPublishPrefixTopic_P(TELE, PSTR(D_RSLT_SENSOR), Settings.flag.mqtt_sensor_retain);
  #ifdef USE_RULES
      RulesTeleperiod();  // Allow rule based HA messages
  #endif  // USE_RULES
    }
}
/*********************************************************************************************\
 * init NimBLE
\*********************************************************************************************/

void MI32Init(void) {
  MI32.mode.init = false;
  if (!MI32.mode.init) {
    NimBLEDevice::init("");

    MI32.mode.canScan = 1;
    MI32.mode.init = 1;
    MI32.period = Settings.tele_period;

    MI32StartScanTask(); // Let's get started !!
  }
  return;
}

/*********************************************************************************************\
 * Task section
\*********************************************************************************************/

void MI32StartTask(uint32_t task){
  switch(task){
    case MI32_TASK_SCAN:
      if (MI32.mode.canScan == 0 || MI32.mode.willConnect == 1) return;
      if (MI32.mode.runningScan == 1 || MI32.mode.connected == 1) return;
      MI32StartScanTask();
      break;
    case MI32_TASK_CONN:
      if (MI32.mode.canConnect == 0 || MI32.mode.willConnect == 1 ) return;
      if (MI32.mode.connected == 1) return;
      MI32StartSensorTask();
      break;
    case MI32_TASK_TIME:
      if (MI32.mode.shallSetTime == 0) return;
      MI32StartTimeTask();
      break;
    case MI32_TASK_BATT:
      if (MI32.mode.willReadBatt == 1) return;
      MI32StartBatteryTask();
      break;
    case MI32_TASK_UNIT:
      if (MI32.mode.shallSetUnit == 0) return;
      MI32StartUnitTask();
      break;
    default:
      break;
  }
}

bool MI32ConnectActiveSensor(){ // only use inside a task !!
    MI32.mode.connected = 0;

    NimBLEAddress _address = NimBLEAddress(MIBLEsensors[MI32.state.sensor].MAC);
    if(NimBLEDevice::getClientListSize()) {
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: found any clients in the list"),D_CMND_MI32);
      MI32Client = NimBLEDevice::getClientByPeerAddress(_address);
      if(MI32Client){
        // Should be impossible
        // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: got connected client"),D_CMND_MI32);
      }
      else {
        // Should be the norm after the first iteration
        MI32Client = NimBLEDevice::getDisconnectedClient();
        // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: got disconnected client"),D_CMND_MI32);
      }
    }

    if(NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS) {
        MI32.mode.willConnect = 0;
        DEBUG_SENSOR_LOG(PSTR("%s: max connection already reached"),D_CMND_MI32);
        return false;
    }
    if(!MI32Client) {
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: will create client"),D_CMND_MI32);
      MI32Client = NimBLEDevice::createClient();
      MI32Client->setClientCallbacks(&MI32SensorCB , false);
      MI32Client->setConnectionParams(12,12,0,48);
      MI32Client->setConnectTimeout(30);
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: did create new client"),D_CMND_MI32);
    }
    vTaskDelay(300/ portTICK_PERIOD_MS);
    if (!MI32Client->connect(_address,false)) {
        MI32.mode.willConnect = 0;
        // NimBLEDevice::deleteClient(MI32Client);
        // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: did not connect client"),D_CMND_MI32);
        return false;
    }
    return true;
  // }
}

void MI32StartScanTask(){
    if (MI32.mode.connected) return;
    MI32.mode.runningScan = 1;
    xTaskCreatePinnedToCore(
    MI32ScanTask,    /* Function to implement the task */
    "MI32ScanTask",  /* Name of the task */
    4096,             /* Stack size in words */
    NULL,             /* Task input parameter */
    0,                /* Priority of the task */
    NULL,             /* Task handle. */
    0);               /* Core where the task should run */
    AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: Start scanning"),D_CMND_MI32);
}

void MI32ScanTask(void *pvParameters){
  if (MI32Scan == nullptr) MI32Scan = NimBLEDevice::getScan();
  DEBUG_SENSOR_LOG(PSTR("%s: Scan Cache Length: %u"),D_CMND_MI32, MI32Scan->getResults().getCount());
  MI32Scan->setInterval(70);
  MI32Scan->setWindow(50);
  MI32Scan->setAdvertisedDeviceCallbacks(&MI32ScanCallbacks,true);
  MI32Scan->setActiveScan(false);
  MI32Scan->start(0, MI32scanEndedCB, true); // never stop scanning, will pause automaically while connecting

  uint32_t timer = 0;
  for(;;){
    if(MI32.mode.shallClearResults){
      MI32Scan->clearResults();
      MI32.mode.shallClearResults=0;
    }
    vTaskDelay(10000/ portTICK_PERIOD_MS);
  }
  vTaskDelete( NULL );
}

void MI32StartSensorTask(){
    MI32.mode.willConnect = 1;
    if (MIBLEsensors[MI32.state.sensor].type != LYWSD03MMC) {
      MI32.mode.willConnect = 0;
      return;
    }
    xTaskCreatePinnedToCore(
      MI32SensorTask,    /* Function to implement the task */
      "MI32SensorTask",  /* Name of the task */
      4096,             /* Stack size in words */
      NULL,             /* Task input parameter */
      15,                /* Priority of the task */
      NULL,             /* Task handle. */
      0);               /* Core where the task should run */
      AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: Start sensor connections"),D_CMND_MI32);
      AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: with sensor: %u"),D_CMND_MI32, MI32.state.sensor);
}

void MI32SensorTask(void *pvParameters){
    if (MI32ConnectActiveSensor()){
      uint32_t timer = 0;
      while (MI32.mode.connected == 0){
        if (timer>1000){
          MI32Client->disconnect();
          // NimBLEDevice::deleteClient(MI32Client);
          MI32.mode.willConnect = 0;
          vTaskDelay(100/ portTICK_PERIOD_MS);
          vTaskDelete( NULL );
        }
        timer++;
        vTaskDelay(10/ portTICK_PERIOD_MS);
      }

      timer = 150;
      switch(MIBLEsensors[MI32.state.sensor].type){
        case LYWSD03MMC:
          MI32.mode.readingDone = 0;
          if(MI32connectLYWSD03forNotification()) timer=0;
          break;
        default:
        break;
      }
      
      while (!MI32.mode.readingDone){
        if (timer>150){
          break;
        }
        timer++;
        vTaskDelay(100/ portTICK_PERIOD_MS);
      }
      MI32Client->disconnect();
      DEBUG_SENSOR_LOG(PSTR("%s: requested disconnect"),D_CMND_MI32);
    }

    MI32.mode.connected = 0;
    vTaskDelete( NULL );
}

bool MI32connectLYWSD03forNotification(){
  NimBLERemoteService* pSvc = nullptr;
  NimBLERemoteCharacteristic* pChr = nullptr;
  static BLEUUID serviceUUID(0xebe0ccb0,0x7a0a,0x4b0c,0x8a1a6ff2997da3a6);
  static BLEUUID charUUID(0xebe0ccc1,0x7a0a,0x4b0c,0x8a1a6ff2997da3a6);
  pSvc = MI32Client->getService(serviceUUID);
  if(pSvc) {
      pChr = pSvc->getCharacteristic(charUUID);
  }
  if (pChr){
    if(pChr->canNotify()) {
      if(pChr->subscribe(true,false,MI32notifyCB)) {
        return true;
      }
    }
  }
  return false;
}

void MI32StartTimeTask(){
    MI32.mode.willConnect = 1;
    xTaskCreatePinnedToCore(
      MI32TimeTask,    /* Function to implement the task */
      "MI32TimeTask",  /* Name of the task */
      4096,             /* Stack size in words */
      NULL,             /* Task input parameter */
      15,                /* Priority of the task */
      NULL,             /* Task handle. */
      0);               /* Core where the task should run */
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: Start time set"),D_CMND_MI32);
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: with sensor: %u"),D_CMND_MI32, MI32.state.sensor);
}

void MI32TimeTask(void *pvParameters){
  if (MIBLEsensors[MI32.state.sensor].type != LYWSD02) {
      MI32.mode.shallSetTime = 0;
      vTaskDelete( NULL );
    }

  if(MI32ConnectActiveSensor()){  
    uint32_t timer = 0;
    while (MI32.mode.connected == 0){
        if (timer>1000){
          break;
        }
        timer++;
        vTaskDelay(10/ portTICK_PERIOD_MS);
      }

    NimBLERemoteService* pSvc = nullptr;
    NimBLERemoteCharacteristic* pChr = nullptr;
    static BLEUUID serviceUUID(0xEBE0CCB0,0x7A0A,0x4B0C,0x8A1A6FF2997DA3A6);
    static BLEUUID charUUID(0xEBE0CCB7,0x7A0A,0x4B0C,0x8A1A6FF2997DA3A6);
    pSvc = MI32Client->getService(serviceUUID);
    if(pSvc) {
        pChr = pSvc->getCharacteristic(charUUID);

    }
    if (pChr){    
      if(pChr->canWrite()) {
        union {
          uint8_t buf[5];
          uint32_t time;
        } _utc;
        _utc.time = Rtc.utc_time;
        _utc.buf[4] = Rtc.time_timezone / 60;

        if(!pChr->writeValue(_utc.buf,sizeof(_utc.buf),true)) { // true is important !
          MI32.mode.willConnect = 0;
          MI32Client->disconnect();
        }
        else {
          MI32.mode.shallSetTime = 0;
          MI32.mode.willSetTime = 0;
        }
      }
    }
    MI32Client->disconnect();
  }

  MI32.mode.connected = 0;
  MI32.mode.canScan = 1;
  vTaskDelete( NULL );
}

void MI32StartUnitTask(){
    MI32.mode.willConnect = 1;
    xTaskCreatePinnedToCore(
      MI32UnitTask,    /* Function to implement the task */
      "MI32UnitTask",  /* Name of the task */
      4096,             /* Stack size in words */
      NULL,             /* Task input parameter */
      15,                /* Priority of the task */
      NULL,             /* Task handle. */
      0);               /* Core where the task should run */
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: Start unit set"),D_CMND_MI32);
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: with sensor: %u"),D_CMND_MI32, MI32.state.sensor);
}

void MI32UnitTask(void *pvParameters){
  if (MIBLEsensors[MI32.state.sensor].type != LYWSD02) {
      MI32.mode.shallSetUnit = 0;
      vTaskDelete( NULL );
    }

  if(MI32ConnectActiveSensor()){  
    uint32_t timer = 0;
    while (MI32.mode.connected == 0){
        if (timer>1000){
          break;
        }
        timer++;
        vTaskDelay(10/ portTICK_PERIOD_MS);
      }

    NimBLERemoteService* pSvc = nullptr;
    NimBLERemoteCharacteristic* pChr = nullptr;
    static BLEUUID serviceUUID(0xEBE0CCB0,0x7A0A,0x4B0C,0x8A1A6FF2997DA3A6);
    static BLEUUID charUUID(0xEBE0CCBE,0x7A0A,0x4B0C,0x8A1A6FF2997DA3A6);
    pSvc = MI32Client->getService(serviceUUID);
    if(pSvc) {
        pChr = pSvc->getCharacteristic(charUUID);
    }

    if(pChr->canRead()){
      uint8_t curUnit;
      const char *buf = pChr->readValue().c_str();
      if( buf[0] != 0 && buf[0]<101 ){
          curUnit = buf[0];
      }

      if(pChr->canWrite()) {
        curUnit = curUnit == 0x01?0xFF:0x01;  // C/F

        if(!pChr->writeValue(&curUnit,sizeof(curUnit),true)) { // true is important !
          MI32.mode.willConnect = 0;
          MI32Client->disconnect();
        }
        else {
          MI32.mode.shallSetUnit = 0;
          MI32.mode.willSetUnit = 0;
        }
      }
    }
    MI32Client->disconnect();
  }

  MI32.mode.connected = 0;
  MI32.mode.canScan = 1;
  vTaskDelete( NULL );
}

void MI32StartBatteryTask(){
    if (MI32.mode.connected) return;
    MI32.mode.willReadBatt = 1;
    MI32.mode.willConnect = 1;
    MI32.mode.canScan = 0;

    switch (MIBLEsensors[MI32.state.sensor].type){
      case LYWSD03MMC: case MJ_HT_V1: case CGG1: case NLIGHT: case MJYD2S: case YEERC:
        MI32.mode.willConnect = 0;
        MI32.mode.willReadBatt = 0;
        return;
      }

    xTaskCreatePinnedToCore(
    MI32BatteryTask, /* Function to implement the task */
    "MI32BatteryTask",  /* Name of the task */
    4096,             /* Stack size in words */
    NULL,             /* Task input parameter */
    15,                /* Priority of the task */
    NULL,             /* Task handle. */
    0);               /* Core where the task should run */
}

void MI32BatteryTask(void *pvParameters){
    // all reported battery values are probably crap, but we allow the reading on demand

    MI32.mode.connected = 0;
    if(MI32ConnectActiveSensor()){
        uint32_t timer = 0;
        while (MI32.mode.connected == 0){
        if (timer>1000){
          break;
        }
        timer++;
        vTaskDelay(30/ portTICK_PERIOD_MS);
      }

      switch(MIBLEsensors[MI32.state.sensor].type){
        case FLORA: case LYWSD02: case CGD1:
          MI32batteryRead(MIBLEsensors[MI32.state.sensor].type);
        break;
      }
      MI32Client->disconnect();
      }
    MI32.mode.willReadBatt = 0;
    MI32.mode.connected = 0;
    vTaskDelete( NULL );
}

void MI32batteryRead(uint32_t _type){
  uint32_t timer = 0;
  while (!MI32.mode.connected){
      if (timer>1000){
        break;
      }
      timer++;
      vTaskDelay(10/ portTICK_PERIOD_MS);
    }
  DEBUG_SENSOR_LOG(PSTR("%s connected for battery"),kMI32DeviceType[MIBLEsensors[MI32.state.sensor].type-1] );
  NimBLERemoteService* pSvc = nullptr;
  NimBLERemoteCharacteristic* pChr = nullptr;

  switch(_type){
    case FLORA:
    {
      static BLEUUID _serviceUUID(0x00001204,0x0000,0x1000,0x800000805f9b34fb);
      static BLEUUID _charUUID(0x00001a02,0x0000,0x1000,0x800000805f9b34fb);
      pSvc = MI32Client->getService(_serviceUUID);
      if(pSvc) {
          pChr = pSvc->getCharacteristic(_charUUID);
      }
    }
    break;
    case LYWSD02:
    {
      static BLEUUID _serviceUUID(0xEBE0CCB0,0x7A0A,0x4B0C,0x8A1A6FF2997DA3A6);
      static BLEUUID _charUUID(0xEBE0CCC4,0x7A0A,0x4B0C,0x8A1A6FF2997DA3A6);
      pSvc = MI32Client->getService(_serviceUUID);
      if(pSvc) {
          pChr = pSvc->getCharacteristic(_charUUID);
      }
    }
    break;
    case CGD1:
    {
      static BLEUUID _serviceUUID((uint16_t)0x180F);
      static BLEUUID _charUUID((uint16_t)0x2A19);
      pSvc = MI32Client->getService(_serviceUUID);
      if(pSvc) {
          pChr = pSvc->getCharacteristic(_charUUID);
      }
    }
    break;
  }

  if (pChr){
    DEBUG_SENSOR_LOG(PSTR("%s: got %s char %s"),D_CMND_MI32, kMI32DeviceType[MIBLEsensors[MI32.state.sensor].type-1], pChr->getUUID().toString().c_str());
    if(pChr->canRead()) {
      const char *buf = pChr->readValue().c_str();
      MI32readBat((char*)buf);
    }
  }
  MI32.mode.readingDone = 1;
}

/*********************************************************************************************\
 * parse the response from advertisements
\*********************************************************************************************/

void MI32parseMiBeacon(char * _buf, uint32_t _slot, uint16_t _bufSize){
  float _tempFloat;
  mi_beacon_t _beacon;

  if (MIBLEsensors[_slot].type==MJ_HT_V1 || MIBLEsensors[_slot].type==CGG1 || MIBLEsensors[_slot].type==YEERC){
    memcpy((uint8_t*)&_beacon+1,(uint8_t*)_buf, sizeof(_beacon)-1); // shift by one byte for the MJ_HT_V1 DANGER!!!
    memcpy((uint8_t*)&_beacon.MAC,(uint8_t*)&_beacon.MAC+1,6);      // but shift back the MAC
    _beacon.counter = _buf[4];                                      // restore the counter
  }
  else{
    memcpy((char *)&_beacon, _buf, _bufSize);
  }

  MIBLEsensors[_slot].lastCnt = _beacon.counter;
#ifdef USE_MI_DECRYPTION
  switch(MIBLEsensors[_slot].type){
    case LYWSD03MMC:
      if (_beacon.frame == 0x5858){
        int decryptRet = MI32_decryptPacket((char*)&_beacon.productID,_bufSize, LYWSD03MMC); //start with PID
      }
      break;
    case MJYD2S:
      AddLog_P2(LOG_LEVEL_DEBUG,PSTR("MJYD2S: %x"),_beacon.frame);
      if (_beacon.frame == 0x5948){                                               // Now let's build/recreate a special MiBeacon
        memmove((uint8_t*)&_beacon.MAC+6,(uint8_t*)&_beacon.MAC, _bufSize);       // shift payload by the size of the MAC = 6 bytes
        memcpy((uint8_t*)&_beacon.MAC,MIBLEsensors[_slot].MAC,6);                 // now insert the real MAC from our internal vector
        _bufSize+=6;                                                              // the packet has grown
        MI32_ReverseMAC(_beacon.MAC);                                             // payload MAC is always reversed
      }
      if (_beacon.frame != 0x5910){
        int decryptRet = MI32_decryptPacket((char*)&_beacon.productID,_bufSize,MJYD2S); //start with PID
      }
      else{
        if(millis()-MIBLEsensors[_slot].lastTime>120000){
          MIBLEsensors[_slot].eventType = 1;
          MIBLEsensors[_slot].events++;
          MIBLEsensors[_slot].shallSendMQTT = 1;
          MIBLEsensors[_slot].lastTime = millis();
          AddLog_P2(LOG_LEVEL_DEBUG,PSTR("MI32: MJYD2S secondary PIR"));
          MIBLEsensors[_slot].NMT = 0; 
          MI32triggerTele();
        }
      }
      break;
  }
#endif //USE_MI_DECRYPTION

if (MIBLEsensors[_slot].type==NLIGHT){
  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("MiBeacon type:%02x: %02x %02x %02x %02x %02x %02x %02x %02x"),_beacon.type, (uint8_t)_buf[0],(uint8_t)_buf[1],(uint8_t)_buf[2],(uint8_t)_buf[3],(uint8_t)_buf[4],(uint8_t)_buf[5],(uint8_t)_buf[6],(uint8_t)_buf[7]);
  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("         type:%02x: %02x %02x %02x %02x %02x %02x %02x %02x"),_beacon.type, (uint8_t)_buf[8],(uint8_t)_buf[9],(uint8_t)_buf[10],(uint8_t)_buf[11],(uint8_t)_buf[12],(uint8_t)_buf[13],(uint8_t)_buf[14],(uint8_t)_buf[15]);
}

  if(MIBLEsensors[_slot].type==6){
    DEBUG_SENSOR_LOG(PSTR("LYWSD03 and CGD1 no support for MiBeacon, type %u"),MIBLEsensors[_slot].type);
    return;
  }
  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s at slot %u"), kMI32DeviceType[MIBLEsensors[_slot].type-1],_slot);
  switch(_beacon.type){
    case 0x01:
      MIBLEsensors[_slot].Btn=_beacon.Btn.num + (_beacon.Btn.longPress/2)*6;
      MIBLEsensors[_slot].shallSendMQTT = 1;
      MI32triggerTele();
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode 1: U16:  %u Button"), MIBLEsensors[_slot].Btn );
    break;
    case 0x04:
      _tempFloat=(float)(_beacon.temp)/10.0f;
      if(_tempFloat<60){
          MIBLEsensors[_slot].temp=_tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode 4: temp updated"));
      }
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode 4: U16:  %u Temp"), _beacon.temp );
    break;
    case 0x06:
      _tempFloat=(float)(_beacon.hum)/10.0f;
      if(_tempFloat<101){
          MIBLEsensors[_slot].hum=_tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode 6: hum updated"));
      }
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode 6: U16:  %u Hum"), _beacon.hum);
    break;
    case 0x07:
      MIBLEsensors[_slot].lux=_beacon.lux & 0x00ffffff;
      if(MIBLEsensors[_slot].type==MJYD2S){
        MIBLEsensors[_slot].eventType = 2; //No PIR
        MIBLEsensors[_slot].shallSendMQTT = 1;      
        MIBLEsensors[_slot].lastTime = millis();
        MI32triggerTele();
      }
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode 7: U24: %u Lux"), _beacon.lux & 0x00ffffff);
    break;
    case 0x08:
      _tempFloat =(float)_beacon.moist;
      if(_tempFloat<100){
          MIBLEsensors[_slot].moisture=_tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode 8: moisture updated"));
      }
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode 8: U8: %u Moisture"), _beacon.moist);
    break;
    case 0x09:
      _tempFloat=(float)(_beacon.fert);
      if(_tempFloat<65535){ // ???
          MIBLEsensors[_slot].fertility=_tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode 9: fertility updated"));
      }
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode 9: U16: %u Fertility"), _beacon.fert);
    break;
    case 0x0a:
      if(_beacon.bat<101){
        MIBLEsensors[_slot].bat = _beacon.bat;
        DEBUG_SENSOR_LOG(PSTR("Mode a: bat updated"));
        }
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode a: U8: %u %%"), _beacon.bat);
    break;
    case 0x0d:
      _tempFloat=(float)(_beacon.HT.temp)/10.0f;
      if(_tempFloat<60){
          MIBLEsensors[_slot].temp = _tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode d: temp updated"));
      }
      _tempFloat=(float)(_beacon.HT.hum)/10.0f;
      if(_tempFloat<100){
          MIBLEsensors[_slot].hum = _tempFloat;
          DEBUG_SENSOR_LOG(PSTR("Mode d: hum updated"));
      }
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode d: U16:  %x Temp U16: %x Hum"), _beacon.HT.temp,  _beacon.HT.hum);
    break;
#ifdef USE_MI_DECRYPTION
    case 0x0f:
    if (_beacon.ten!=0) break;
      MIBLEsensors[_slot].eventType = 1; //PIR
      MIBLEsensors[_slot].shallSendMQTT = 1;
      MIBLEsensors[_slot].lastTime = millis();
      MIBLEsensors[_slot].events++;
      MIBLEsensors[_slot].lux = _beacon.lux;
      MIBLEsensors[_slot].NMT = 0; 
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("PIR: primary"),MIBLEsensors[_slot].lux );
      MI32triggerTele();
    break;
    case 0x17:
      MIBLEsensors[_slot].NMT = _beacon.NMT;
      MIBLEsensors[_slot].eventType = 3; // NMT
      MIBLEsensors[_slot].shallSendMQTT = 1;      
      // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("Mode 17: NMT: %u seconds"), _beacon.NMT);
      MI32triggerTele();
    break;
#endif //USE_MI_DECRYPTION
    default:
      if (MIBLEsensors[_slot].type==NLIGHT){
        MIBLEsensors[_slot].eventType = 1; //PIR
        MIBLEsensors[_slot].shallSendMQTT = 1;
        MIBLEsensors[_slot].events++;
        MIBLEsensors[_slot].NMT = 0; 
        MIBLEsensors[_slot].lastTime = millis();
        // AddLog_P2(LOG_LEVEL_DEBUG,PSTR("PIR: primary"),MIBLEsensors[_slot].lux );
        MI32triggerTele();
      }
    break;
  }
}

void MI32parseCGD1Packet(char * _buf, uint32_t length, uint8_t addr[6]){ // no MiBeacon
  uint8_t _addr[6];
  memcpy(_addr,addr,6);
  uint32_t _slot = MIBLEgetSensorSlot(_addr, 0x0576, 0); // This must be hard-coded, no object-id in Cleargrass-packet, we have no packet counter too
  AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s at slot %u"), kMI32DeviceType[MIBLEsensors[_slot].type-1],_slot);
  if(_slot==0xff) return;
  cg_packet_t _packet;
  memcpy((char*)&_packet,_buf,sizeof(_packet));
  switch (_packet.mode){
    case 0x0401:
      float _tempFloat;
      _tempFloat=(float)(_packet.temp)/10.0f;
      if(_tempFloat<60){
          MIBLEsensors.at(_slot).temp = _tempFloat;
          DEBUG_SENSOR_LOG(PSTR("CGD1: temp updated"));
      }
      _tempFloat=(float)(_packet.hum)/10.0f;
      if(_tempFloat<100){
          MIBLEsensors.at(_slot).hum = _tempFloat;
          DEBUG_SENSOR_LOG(PSTR("CGD1: hum updated"));
      }
      DEBUG_SENSOR_LOG(PSTR("CGD1: U16:  %x Temp U16: %x Hum"), _packet.temp,  _packet.hum);
      break;
    case 0x0102:
      if(_packet.bat<101){
      MIBLEsensors.at(_slot).bat = _packet.bat;
      DEBUG_SENSOR_LOG(PSTR("Mode a: bat updated"));
      }
      break;
    default:
      DEBUG_SENSOR_LOG(PSTR("MI32: unexpected CGD1-packet"));
  }
}

void MI32ParseResponse(char *buf, uint16_t bufsize, uint8_t addr[6]) {
    if(bufsize<9) {  //9 is from the NLIGHT
      return;
    }
    uint16_t _type= buf[3]*256 + buf[2];
    // AddLog_P2(LOG_LEVEL_INFO, PSTR("%02x %02x %02x %02x"),(uint8_t)buf[0], (uint8_t)buf[1],(uint8_t)buf[2],(uint8_t)buf[3]);
    uint8_t _addr[6];
    memcpy(_addr,addr,6);
    uint16_t _slot = MIBLEgetSensorSlot(_addr, _type, buf[4]);
    if(_slot!=0xff) MI32parseMiBeacon(buf,_slot,bufsize);
}

/***********************************************************************\
 * Read data from connections
\***********************************************************************/

void MI32readHT_LY(char *_buf){
  DEBUG_SENSOR_LOG(PSTR("%s: raw data: %x%x%x%x%x%x%x"),D_CMND_MI32,_buf[0],_buf[1],_buf[2],_buf[3],_buf[4],_buf[5],_buf[6]);
  if(_buf[0] != 0 && _buf[1] != 0){
    memcpy(&LYWSD0x_HT,(void *)_buf,sizeof(LYWSD0x_HT));
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("%s: T * 100: %u, H: %u, V: %u"),D_CMND_MI32,LYWSD0x_HT.temp,LYWSD0x_HT.hum, LYWSD0x_HT.volt);
    uint32_t _slot = MI32.state.sensor;

    DEBUG_SENSOR_LOG(PSTR("MIBLE: Sensor slot: %u"), _slot);
    static float _tempFloat;
    _tempFloat=(float)(LYWSD0x_HT.temp)/100.0f;
    if(_tempFloat<60){
        MIBLEsensors[_slot].temp=_tempFloat;
        // MIBLEsensors[_slot].showedUp=255; // this sensor is real
    }
    _tempFloat=(float)LYWSD0x_HT.hum;
    if(_tempFloat<100){
      MIBLEsensors[_slot].hum = _tempFloat;
      DEBUG_SENSOR_LOG(PSTR("LYWSD0x: hum updated"));
    }
    if (MIBLEsensors[_slot].type == LYWSD03MMC){
      MIBLEsensors[_slot].bat = ((float)LYWSD0x_HT.volt-2100.0f)/12.0f;
    }
  }
}

bool MI32readBat(char *_buf){
  DEBUG_SENSOR_LOG(PSTR("%s: raw data: %x%x%x%x%x%x%x"),D_CMND_MI32,_buf[0],_buf[1],_buf[2],_buf[3],_buf[4],_buf[5],_buf[6]);
  if(_buf[0] != 0){
    AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: Battery: %u"),D_CMND_MI32,_buf[0]);
    uint32_t _slot = MI32.state.sensor;
    DEBUG_SENSOR_LOG(PSTR("MIBLE: Sensor slot: %u"), _slot);
    if(_buf[0]<101){
        MIBLEsensors[_slot].bat=_buf[0];
        if(MIBLEsensors[_slot].type==FLORA){
          memcpy(MIBLEsensors[_slot].firmware, _buf+2, 5);
          MIBLEsensors[_slot].firmware[5] = '\0';
          AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: Firmware: %s"),D_CMND_MI32,MIBLEsensors[_slot].firmware);
         }
        return true;
    }
  }
  return false;
}

/**
 * @brief Main loop of the driver, "high level"-loop
 *
 */

void MI32EverySecond(bool restart){
  static uint32_t _counter = MI32.period - 15;
  static uint32_t _nextSensorSlot = 0;

  for (uint32_t i = 0; i < MIBLEsensors.size(); i++) {
    if(MIBLEsensors[i].type==NLIGHT || MIBLEsensors[i].type==MJYD2S){
      MIBLEsensors[i].NMT++;
    }
  }

  if(restart){
    _counter = 0;
    MI32.mode.canScan = 0;
    MI32.mode.canConnect = 1;
    MI32.mode.willReadBatt = 0;
    MI32.mode.willConnect = 0;
    return;
  }

  if (MI32.mode.shallSetTime) {
    MI32.mode.canScan = 0;
    MI32.mode.canConnect = 0;
    if (MI32.mode.willSetTime == 0){
      MI32.mode.willSetTime = 1;
      MI32StartTask(MI32_TASK_TIME);
    }
  }

  if (MI32.mode.shallSetUnit) {
    MI32.mode.canScan = 0;
    MI32.mode.canConnect = 0;
    if (MI32.mode.willSetUnit == 0){
      MI32.mode.willSetUnit = 1;
      MI32StartTask(MI32_TASK_UNIT);
    }
  }

  if (MI32.mode.willReadBatt) return;

  if (_counter>MI32.period) {
    _counter = 0;
    MI32.mode.canScan = 0;
    MI32.mode.canConnect = 1;
  }

  if(MI32.mode.connected == 1 || MI32.mode.willConnect == 1) return;

  if(MIBLEsensors.size()==0) {
    if (MI32.mode.runningScan == 0 && MI32.mode.canScan == 1) MI32StartTask(MI32_TASK_SCAN);
    return;
  }

  if(_counter==0) {
  
    MI32.state.sensor = _nextSensorSlot;
    AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: active sensor now: %u of %u"),D_CMND_MI32, MI32.state.sensor, MIBLEsensors.size()-1);
    MI32.mode.canScan = 0;
    // if (MI32.mode.runningScan|| MI32.mode.connected || MI32.mode.willConnect) return;
    if (MI32.mode.connected || MI32.mode.willConnect) return;
    _nextSensorSlot++;
    MI32.mode.canConnect = 1;
    if(MI32.mode.connected == 0) {
    AddLog_P2(LOG_LEVEL_DEBUG, PSTR("will connect to %s"),kMI32DeviceType[MIBLEsensors[MI32.state.sensor].type-1] );

    if (MI32.mode.shallReadBatt) {
      MI32StartTask(MI32_TASK_BATT);
      }
#ifndef USE_MI_DECRYPTION // turn off connections, because we only listen to advertisements
    else{
      MI32StartTask(MI32_TASK_CONN);
      }
#endif //USE_MI_DECRYPTION
    }
    if (_nextSensorSlot>(MIBLEsensors.size()-1)) {
      _nextSensorSlot= 0;
      _counter++;
      if (MI32.mode.shallReadBatt){
        MI32.mode.shallReadBatt = 0;
      }
      MI32.mode.canConnect = 0;
      MI32.mode.canScan = 1;
    }
  }
  else _counter++;
  if (MI32.state.sensor>MIBLEsensors.size()-1) {
    _nextSensorSlot = 0;
    MI32.mode.canScan = 1;
  }
  MI32StartTask(MI32_TASK_SCAN);
}

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

bool MI32Cmd(void) {
  char command[CMDSZ];
  bool serviced = true;
  uint8_t disp_len = strlen(D_CMND_MI32);

  if (!strncasecmp_P(XdrvMailbox.topic, PSTR(D_CMND_MI32), disp_len)) {  // prefix
    uint32_t command_code = GetCommandCode(command, sizeof(command), XdrvMailbox.topic + disp_len, kMI32_Commands);
    switch (command_code) {
      case CMND_MI32_PERIOD:
        if (XdrvMailbox.data_len > 0) {
          if (XdrvMailbox.payload==1) {
            MI32EverySecond(true);
            XdrvMailbox.payload = MI32.period;
            }
          else {
            MI32.period = XdrvMailbox.payload;
          }
        }
        else {
          XdrvMailbox.payload = MI32.period;
        }
        Response_P(S_JSON_MI32_COMMAND_NVALUE, command, XdrvMailbox.payload);
        break;
      case CMND_MI32_TIME:
        if (XdrvMailbox.data_len > 0) {
          if(MIBLEsensors.size()>XdrvMailbox.payload){
            if(MIBLEsensors[XdrvMailbox.payload].type == LYWSD02){
              AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: will set Time"),D_CMND_MI32);
              MI32.state.sensor = XdrvMailbox.payload;
              MI32.mode.canScan = 0;
              MI32.mode.canConnect = 0;
              MI32.mode.shallSetTime = 1;
              MI32.mode.willSetTime = 0;
              }
            }
          }
        Response_P(S_JSON_MI32_COMMAND_NVALUE, command, XdrvMailbox.payload);
        break;
      case CMND_MI32_UNIT:
        if (XdrvMailbox.data_len > 0) {
          if(MIBLEsensors.size()>XdrvMailbox.payload){
            if(MIBLEsensors[XdrvMailbox.payload].type == LYWSD02){
              AddLog_P2(LOG_LEVEL_DEBUG,PSTR("%s: will set Unit"),D_CMND_MI32);
              MI32.state.sensor = XdrvMailbox.payload;
              MI32.mode.canScan = 0;
              MI32.mode.canConnect = 0;
              MI32.mode.shallSetUnit = 1;
              MI32.mode.willSetUnit = 0;
              }
            }
          }
        Response_P(S_JSON_MI32_COMMAND_NVALUE, command, XdrvMailbox.payload);
        break;
      case CMND_MI32_PAGE:
        if (XdrvMailbox.data_len > 0) {
            if (XdrvMailbox.payload == 0) XdrvMailbox.payload = MI32.perPage; // ignore 0
            MI32.perPage = XdrvMailbox.payload;
          }
        else XdrvMailbox.payload = MI32.perPage;
        Response_P(S_JSON_MI32_COMMAND_NVALUE, command, XdrvMailbox.payload);
        break;
      case CMND_MI32_BATTERY:
        MI32EverySecond(true);
        MI32.mode.shallReadBatt = 1;
        MI32.mode.canConnect = 1;
        XdrvMailbox.payload = MI32.period;
        Response_P(S_JSON_MI32_COMMAND, command, "");
        break;
#ifdef USE_MI_DECRYPTION        
      case CMND_MI32_KEY:
        if (XdrvMailbox.data_len==44){  // a KEY-MAC-string
          MI32AddKey(XdrvMailbox.data);
          Response_P(S_JSON_MI32_COMMAND, command, XdrvMailbox.data);
        }
        break;
#endif //USE_MI_DECRYPTION
      default:
        // else for Unknown command
        serviced = false;
      break;
    }
  } else {
    return false;
  }
  return serviced;
}


/*********************************************************************************************\
 * Presentation
\*********************************************************************************************/

const char HTTP_MI32[] PROGMEM = "{s}MI ESP32 {m}%u%s / %u{e}";
const char HTTP_MI32_SERIAL[] PROGMEM = "{s}%s %s{m}%02x:%02x:%02x:%02x:%02x:%02x%{e}";
const char HTTP_BATTERY[] PROGMEM = "{s}%s" " Battery" "{m}%u %%{e}";
const char HTTP_VOLTAGE[] PROGMEM = "{s}%s " D_VOLTAGE "{m}%s V{e}";
const char HTTP_LASTBUTTON[] PROGMEM = "{s}%s Last Button{m}%u {e}";
const char HTTP_EVENTS[] PROGMEM = "{s}%s Events{m}%u {e}";
const char HTTP_NMT[] PROGMEM = "{s}%s No motion{m}> %u seconds{e}";
const char HTTP_MI32_FLORA_DATA[] PROGMEM = "{s}%s" " Fertility" "{m}%u us/cm{e}";
const char HTTP_MI32_HL[] PROGMEM = "{s}<hr>{m}<hr>{e}";

void MI32Show(bool json)
{
  if (json) {
    if(!MI32.mode.triggeredTele){
      MI32.mode.shallClearResults=1;
      }
    for (uint32_t i = 0; i < MIBLEsensors.size(); i++) {
      switch(MIBLEsensors[i].type){
        case YEERC:
          if(MIBLEsensors[i].shallSendMQTT==0) continue;
          break;
        default:
          if(MI32.mode.triggeredTele) continue;
          break;
        }
/*
      char slave[33];
      snprintf_P(slave, sizeof(slave), PSTR("%s-%02x%02x%02x"),
        kMI32DeviceType[MIBLEsensors[i].type-1],MIBLEsensors[i].serial[3],MIBLEsensors[i].serial[4],MIBLEsensors[i].serial[5]);
      ResponseAppend_P(PSTR(",\"%s\":{"), slave);
*/
      ResponseAppend_P(PSTR(",\"%s-%02x%02x%02x\":{"),
        kMI32DeviceType[MIBLEsensors[i].type-1],
        MIBLEsensors[i].MAC[3], MIBLEsensors[i].MAC[4], MIBLEsensors[i].MAC[5]);

      if (MIBLEsensors[i].type == FLORA) {
        if (!isnan(MIBLEsensors[i].temp)) {
          char temperature[FLOATSZ]; // all sensors have temperature
          dtostrfd(MIBLEsensors[i].temp, Settings.flag2.temperature_resolution, temperature);
          ResponseAppend_P(PSTR("\"" D_JSON_TEMPERATURE "\":%s"), temperature);
        } else {
          ResponseAppend_P(PSTR("}"));
          continue;
        }
        if (MIBLEsensors[i].lux!=0x0ffffff) { // this is the error code -> no lux
          ResponseAppend_P(PSTR(",\"" D_JSON_ILLUMINANCE "\":%u"), MIBLEsensors[i].lux);
        }
        if (!isnan(MIBLEsensors[i].moisture)) {
          ResponseAppend_P(PSTR(",\"" D_JSON_MOISTURE "\":%f"), MIBLEsensors[i].moisture);
        }
        if (!isnan(MIBLEsensors[i].fertility)) {
          ResponseAppend_P(PSTR(",\"Fertility\":%f"), MIBLEsensors[i].fertility);
        }
        ResponseAppend_P(PSTR(",\"Firmware\":\"%s\""), MIBLEsensors[i].firmware);
      }
      if (MIBLEsensors[i].type > FLORA){
        if (!isnan(MIBLEsensors[i].hum) && !isnan(MIBLEsensors[i].temp)) {
          ResponseAppendTHD(MIBLEsensors[i].temp, MIBLEsensors[i].hum);
        }
      }
#ifdef USE_MI_DECRYPTION
      if (MIBLEsensors[i].type == MJYD2S){
        ResponseAppend_P(PSTR("\"Events\":%u"),MIBLEsensors[i].events);
        if(MIBLEsensors[i].shallSendMQTT && MIBLEsensors[i].eventType<3) ResponseAppend_P(PSTR(",\"PIR\":%u"), 2 - MIBLEsensors[i].eventType);
        if(MIBLEsensors[i].eventType==3) ResponseAppend_P(PSTR(",\"NMT\":%u"), MIBLEsensors[i].NMT);
        MIBLEsensors[i].eventType=0;
        if(MIBLEsensors[i].lux!=0x0ffffff) ResponseAppend_P(PSTR(",\"" D_JSON_ILLUMINANCE "\":%u"), MIBLEsensors[i].lux);
      }
#endif //USE_MI_DECRYPTION
      if (MIBLEsensors[i].type == NLIGHT){
        ResponseAppend_P(PSTR("\"Events\":%u"),MIBLEsensors[i].events);
        if(MIBLEsensors[i].shallSendMQTT) ResponseAppend_P(PSTR(",\"PIR\":1"));
      }
      if (MIBLEsensors[i].type == YEERC){
        if(MIBLEsensors[i].shallSendMQTT) ResponseAppend_P(PSTR("\"Btn\":%u"),MIBLEsensors[i].Btn);
      }
      if (MIBLEsensors[i].bat != 0x00) { // this is the error code -> no battery
        ResponseAppend_P(PSTR(",\"Battery\":%u"), MIBLEsensors[i].bat);
      }
      ResponseAppend_P(PSTR("}"));
    MIBLEsensors[i].shallSendMQTT = 0;
    MI32.mode.triggeredTele = 0;  
    }
#ifdef USE_WEBSERVER
    } else {
      static  uint16_t _page = 0;
      static  uint16_t _counter = 0;
      int32_t i = _page * MI32.perPage;
      uint32_t j = i + MI32.perPage;
      if (j+1>MIBLEsensors.size()){
        j = MIBLEsensors.size();
      }
      char stemp[5] ={0};
      if (MIBLEsensors.size()-(_page*MI32.perPage)>1 && MI32.perPage!=1) {
        sprintf_P(stemp,"-%u",j);
      }
      if (MIBLEsensors.size()==0) i=-1; // only for the GUI

      WSContentSend_PD(HTTP_MI32, i+1,stemp,MIBLEsensors.size());
      for (i; i<j; i++) {
        WSContentSend_PD(HTTP_MI32_HL);
        WSContentSend_PD(HTTP_MI32_SERIAL, kMI32DeviceType[MIBLEsensors[i].type-1], D_MAC_ADDRESS, MIBLEsensors[i].MAC[0], MIBLEsensors[i].MAC[1],MIBLEsensors[i].MAC[2],MIBLEsensors[i].MAC[3],MIBLEsensors[i].MAC[4],MIBLEsensors[i].MAC[5]);
        if (MIBLEsensors[i].type==FLORA) {
          if (!isnan(MIBLEsensors[i].temp)) {
            char temperature[FLOATSZ];
            dtostrfd(MIBLEsensors[i].temp, Settings.flag2.temperature_resolution, temperature);
            WSContentSend_PD(HTTP_SNS_TEMP, kMI32DeviceType[MIBLEsensors[i].type-1], temperature, TempUnit());
          }
          if (!isnan(MIBLEsensors[i].moisture)) {
            WSContentSend_PD(HTTP_SNS_MOISTURE, kMI32DeviceType[MIBLEsensors[i].type-1], int(MIBLEsensors[i].moisture));
          }
          if (!isnan(MIBLEsensors[i].fertility)) {
            WSContentSend_PD(HTTP_MI32_FLORA_DATA, kMI32DeviceType[MIBLEsensors[i].type-1], int(MIBLEsensors[i].fertility));
          }
        }
        if (MIBLEsensors[i].type>FLORA) { // everything "above" Flora
          if (!isnan(MIBLEsensors[i].hum) && !isnan(MIBLEsensors[i].temp)) {
            WSContentSend_THD(kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].temp, MIBLEsensors[i].hum);
          }
        }
#ifdef USE_MI_DECRYPTION
        if (MIBLEsensors[i].type==NLIGHT || MIBLEsensors[i].type==MJYD2S) {
#else
        if (MIBLEsensors[i].type==NLIGHT) {
#endif //USE_MI_DECRYPTION
          WSContentSend_PD(HTTP_EVENTS, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].events);
          if(MIBLEsensors[i].NMT>0) WSContentSend_PD(HTTP_NMT, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].NMT);
        }
        if (MIBLEsensors[i].lux!=0x00ffffff) { // this is the error code -> no valid value
          WSContentSend_PD(HTTP_SNS_ILLUMINANCE, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].lux);
        }
        if(MIBLEsensors[i].bat!=0x00){
            WSContentSend_PD(HTTP_BATTERY, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].bat);
        }
        if (MIBLEsensors[i].type==YEERC){
          WSContentSend_PD(HTTP_LASTBUTTON, kMI32DeviceType[MIBLEsensors[i].type-1], MIBLEsensors[i].Btn);
        }
      }
      _counter++;
      if(_counter>3) {
        _page++;
        _counter=0;
      }
      if (MIBLEsensors.size()%MI32.perPage==0 && _page==MIBLEsensors.size()/MI32.perPage) { _page = 0; }
      if (_page>MIBLEsensors.size()/MI32.perPage) { _page = 0; }
#endif  // USE_WEBSERVER
    }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xsns62(uint8_t function)
{
  bool result = false;
  if (FUNC_INIT == function){
    MI32Init();
  }

  if (MI32.mode.init) {
    switch (function) {
      case FUNC_EVERY_SECOND:
        MI32EverySecond(false);
        break;
      case FUNC_COMMAND:
        result = MI32Cmd();
        break;
      case FUNC_JSON_APPEND:
        MI32Show(1);
        break;
#ifdef USE_WEBSERVER
      case FUNC_WEB_SENSOR:
        MI32Show(0);
        break;
#endif  // USE_WEBSERVER
      }
  }
  return result;
}
#endif  // USE_MI_ESP32
#endif  // ESP32
