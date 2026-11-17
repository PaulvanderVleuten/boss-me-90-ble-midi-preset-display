#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include "index.h"
#include <SPI.h>
#include "FS.h"
#include "LittleFS.h"
#include <TFT_eSPI.h>
#include <Control_Surface.h>
#include <BLEMidi.h> // Maxime André's ESP32-BLE-MIDI header
#include <NimBLEDevice.h>

///////////////FUNCTION PROTOTYPES////////////////////
void drawMainUI();
void handleMainScreenTouch();
void drawBluetoothIcon(int x, int y, int w, int h, uint16_t color, uint16_t bgColor);
void drawWifiIcon(int x, int y, uint16_t fgColor, uint16_t bgColor);
void updateWiFiIcon();
void loadPatchesFromFlash();
void renderNewPatch(byte program, bool isPreselected);
void drawWordWrappedString(String text, int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t padX, int16_t padY);
void drawUI();
void handleTouch();
void updateWiFiStatusLine();
void redrawKeyFacesOnly();
void loadCredentials();
void saveCredentials();
void touch_calibrate();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
void handleRoot();
void savePatchesToFlash();

/////////////////GPIO CONFIGURATION//////////////////////////
#define BUTTON_PREV_PIN 1  // patch preselect Previous button
#define BUTTON_NEXT_PIN   2  // patch preselect Next button
#define BUTTON_SEND_PIN 3  // Send button -> send preselect patch to ME-90

////////VARIABLE DECLARATIONS///////////////////////////////
TFT_eSPI tft = TFT_eSPI();
bool showWifiSetupUI = false; 

#define TOTAL_PATCHES 72
String patchNames[TOTAL_PATCHES]; 
uint16_t patchColors[TOTAL_PATCHES]; 

String ssid = ""; 
String password = "";
bool editingPassword = false;
bool isShiftActive = false;
const int KEY_ROWS = 4;
const int KEY_COLS = 10;
const char keys[KEY_ROWS][KEY_COLS] = {
  {'1','2','3','4','5','6','7','8','9','0'},
  {'q','w','e','r','t','y','u','i','o','p'},
  {'a','s','d','f','g','h','j','k','l','_'},
  {'z','x','c','v','b','n','m','<',' ','^'} 
};

int keyW = 46;       
int keyH = 42;       
int keyStartX = 10;  
int keyStartY = 140; 

Preferences preferences;

// Bootup SysEx Buffer (Catch fragmented BLE SysEx packets)
#define SYSEX_BUFFER_SIZE 512
uint8_t* sysExBootBuffer = nullptr;
size_t sysExBootIndex = 0;

// NimBLE Remote Characteristic pointer for direct SysEx I/O
NimBLERemoteCharacteristic* pMidiRemoteChar = nullptr;

// MIDI Staging Pipeline Variables
volatile bool newMidiMessageAvailable = false;
volatile byte latestMidiProgram = 0;

byte preselectedMidiProgram = 0;   // Staged index showing on screen before sending

bool postSetupResetRequired = false;
bool bleDisconnectTriggered = false;

WebServer server(80);                        
WebSocketsServer webSocket = WebSocketsServer(81); 

//!!!!!!!!!!!!!!!!!!TOUCH CALIBRATION !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
#define CALIBRATION_FILE "/TouchCalData"
#define REPEAT_CAL false        // At first upload make it "true" to calibrate, when done turn it back to "false" and upload again
//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

/////////////// SysEx //////////////////////////////////
///////////////Checksum calculation ////////////////////
uint8_t calculateRolandChecksum(const uint8_t* address, const uint8_t* size) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += address[i];
    for (int i = 0; i < 4; i++) sum += size[i];
    uint8_t accumulated = sum & 0x7F;
    return (128 - accumulated) & 0x7F;
}
///////////////Incoming SysEx Patch ////////////////////
void handleIncomingSysExPatch(uint8_t activePatchIndex) {
    if (!showWifiSetupUI) {
        latestMidiProgram = activePatchIndex;
        newMidiMessageAvailable = true; 
    }
}
// Wraps a raw SysEx array in BLE-MIDI timestamps and transmits over NimBLE
void sendRawBleSysEx(const uint8_t* sysexData, size_t len) {
    if (!pMidiRemoteChar || len < 2) return;

    uint8_t blePacket[64];
    if (len + 3 > sizeof(blePacket)) return;

    size_t idx = 0;
    blePacket[idx++] = 0x80; // BLE Header byte
    blePacket[idx++] = 0x80; // Start Timestamp byte

    for (size_t i = 0; i < len - 1; i++) {
        blePacket[idx++] = sysexData[i];
    }

    blePacket[idx++] = 0x80;               // End Timestamp byte
    blePacket[idx++] = sysexData[len - 1]; // 0xF7

    if (pMidiRemoteChar->canWriteNoResponse()) {
        pMidiRemoteChar->writeValue(blePacket, idx, false);
    } else if (pMidiRemoteChar->canWrite()) {
        pMidiRemoteChar->writeValue(blePacket, idx, true);
    }

  }

//CUSTOM CONTROL SURFACE INTERFACE FOR BLE MIDI CLIENT (v0.4.0 API)//////////////////////////////////////////
class BLEMIDI_ClientInterface : public MIDI_Interface {
  private:
    volatile bool subscribePending = false; 
  public:
    BLEMIDI_ClientInterface() : MIDI_Interface() {}
    
    void begin() {
        MIDI_Interface::begin();
        BLEMidiClient.begin("ESP32-S3 Display Hub");

        // Midi Program Change listener
        BLEMidiClient.setProgramChangeCallback([](uint8_t channel, uint8_t program, uint16_t timestamp) {
            ChannelMessage msg{
                MIDIMessageType::ProgramChange,
                Channel(channel),  
                program,           
                0, 
                Cable(0)
            };
            if (instance && instance->callbacks) {
                instance->callbacks->onChannelMessage(*instance, msg);
            }
        });

        //Flag asynchronously when the connection handshake executes
        BLEMidiClient.setOnConnectCallback([]() {
            if (instance) {
                instance->subscribePending = true; 
            }
        });
        BLEMidiClient.setOnDisconnectCallback([]() {
            bleDisconnectTriggered = true; 
        });

        instance = this;
    }

    void update() override {
        if (showWifiSetupUI) return; 
        
       //DIRECT NATIVE NimBLE SUBSCRIPTION 
        if (subscribePending) {
            subscribePending = false; 
            delay(500); // settle time for service discovery
            
            // Retrieve connected peer lists directly from NimBLE core
            std::vector<NimBLEClient*> clients = NimBLEDevice::getConnectedClients();
            if (!clients.empty() && clients[0] != nullptr) {
                
                // Establish connection hook straight to standard global BLE-MIDI Service UUID
                NimBLERemoteService* pService = clients[0]->getService("03b80e5a-ede8-4b33-a751-6ce34ec4c700"); //BLE-MIDI Service UUID
                if (pService) {
                    
                    // Grab the raw BLE-MIDI characteristic descriptor notification node
                    NimBLERemoteCharacteristic* pChar = pService->getCharacteristic("7772e5db-3868-4112-a1a9-f2669d106bf3"); //MIDI Data Characteristic UUID
                    if (pChar && pChar->canNotify()) {
                        pMidiRemoteChar = pChar;
                        
                        // Subscribe directly to the raw wireless byte chunks 
                        pChar->subscribe(true, [](NimBLERemoteCharacteristic* /*pRemoteChar*/, uint8_t* pData, size_t length, bool /*isNotify*/) {
                            if (length < 2) return;
                          
                            if (!sysExBootBuffer) return;
                            static bool expectProgramChangeValue = false;

                            // Parse BLE-MIDI packet (skip byte 0 which is always the BLE Header byte)
                            for (size_t i = 1; i < length; i++) {
                                uint8_t b = pData[i];

                                //Catch normal footswitch Program Change (0xC0 on Channel 1)
                                if (b == 0xC0) {
                                    expectProgramChangeValue = true;
                                    continue;
                                }
                                if (expectProgramChangeValue && b < 0x80) {
                                    expectProgramChangeValue = false;
                                    if (b < TOTAL_PATCHES) {
                                        handleIncomingSysExPatch(b);
                                    }
                                    continue;
                                }

                                //Catch SysEx Start (0xF0)
                                if (b == 0xF0) {
                                    sysExBootIndex = 0;
                                    sysExBootBuffer[sysExBootIndex++] = b;
                                }
                                //Catch SysEx End (0xF7)
                                else if (b == 0xF7) {
                                    if (sysExBootIndex > 0 && sysExBootIndex < SYSEX_BUFFER_SIZE) {
                                        sysExBootBuffer[sysExBootIndex++] = b;
                                        // Verify exact ME-90 15-byte Active Patch DT1 packet:
                                        // F0 41 10 [01 05 03] [12] [00 00 00 00] [MSB] [LSB] [Checksum] F7
                                        if (sysExBootIndex == 15 &&
                                            sysExBootBuffer[1] == 0x41 &&
                                            sysExBootBuffer[3] == 0x01 &&
                                            sysExBootBuffer[4] == 0x05 &&
                                            sysExBootBuffer[5] == 0x03 &&
                                            sysExBootBuffer[6] == 0x12 &&
                                            sysExBootBuffer[7] == 0x00 &&
                                            sysExBootBuffer[8] == 0x00 &&
                                            sysExBootBuffer[9] == 0x00 &&
                                            sysExBootBuffer[10] == 0x00) {
                                        
                                        // Reconstruct 0-71 patch index from the two 4-bit nibbles (MSB << 4 | LSB)
                                            uint8_t activePatchIndex = (sysExBootBuffer[11] << 4) | (sysExBootBuffer[12] & 0x0F);

                                            if (activePatchIndex < TOTAL_PATCHES) {
                                                handleIncomingSysExPatch(activePatchIndex);
                                                }
                                      
                                    }
                                }
                                sysExBootIndex = 0;
                              }
                                    //Collect 7-bit SysEx payload data (ignores BLE timestamp bytes >= 0x80)
                                    else if (b < 0x80 && sysExBootIndex > 0 && sysExBootIndex < SYSEX_BUFFER_SIZE) {
                                    sysExBootBuffer[sysExBootIndex++] = b;
                                    }
                            }
                        }, true); // True overrides the internal library listener

                  
                  // 16-byte RQ1 Query for ME-90 Active Patch Address (00 00 00 00, Size 00 00 00 02)
                  uint8_t me90Address[4] = { 0x00, 0x00, 0x00, 0x00 };
                  uint8_t targetSize[4]  = { 0x00, 0x00, 0x00, 0x02 };
                  uint8_t checkSum = calculateRolandChecksum(me90Address, targetSize);
                  uint8_t sysexRequest[] = {
                  0xF0, 0x41, 0x10, 0x01, 0x05, 0x03, // ME-90 3-Byte Model ID (01 05 03)
                  0x11,                               // RQ1 Data Request Command
                  me90Address[0], me90Address[1], me90Address[2], me90Address[3],
                  targetSize[0],  targetSize[1],  targetSize[2],  targetSize[3],
                  checkSum, 0xF7
                  };

                  SysExMessage sysexMsg{sysexRequest, sizeof(sysexRequest), Cable(0)};
                  instance->send(sysexMsg);
                }
              }
            }
            return;
          }

        if (bleDisconnectTriggered) {
            bleDisconnectTriggered = false; 
            delay(1500); 
            return; 
        }

        if (!BLEMidiClient.isConnected()) {     // SEARCHING
            drawBluetoothIcon(440, 14, 14, 28, TFT_RED, TFT_BLACK);

            static unsigned long lastScanTime = 0;
            static unsigned long postSetupPauseTimer = 0; 

           if (postSetupResetRequired) {
                postSetupResetRequired = false;  
                postSetupPauseTimer = millis(); 
                lastScanTime = millis();        
                return;              
            }

            if (millis() - postSetupPauseTimer < 3000) return; 

            if (millis() - lastScanTime > 6000 ) { 
                lastScanTime = millis();
                int deviceCount = BLEMidiClient.scan(); 
                int targetDeviceIndex = -1;
                
                for (int i = 0; i < deviceCount; i++) {  
                  String blename = BLEMidiClient.deviceName(i);
                  if (blename == "" || blename.length() == 0) {
                     blename = BLEMidiClient.deviceMacAddress(i); 
                  }
                  if (blename.indexOf("ME-90") >= 0 || blename.indexOf("BT-DUAL") >= 0) {
                      targetDeviceIndex = i; 
                  }
                }

                if (deviceCount > 0 && targetDeviceIndex != -1) {  //CONNECTED
                    if (BLEMidiClient.connect(targetDeviceIndex)) {                         
                        tft.fillRect(435, 5, 120, 60, TFT_BLACK); 
                        drawBluetoothIcon(440, 14, 14, 28, TFT_GREEN, TFT_BLACK);    
                    } else {                         
                        NimBLEDevice::deleteAllBonds(); 
                    }               
                } 
            }
        }
    }

    void handleStall() override {}
    void setCallbacks(MIDI_Callbacks *cb) { this->callbacks = cb; }
    void setCallbacks(MIDI_Callbacks &cb) { this->callbacks = &cb; }

    // Direct interface send mechanism accessible to main loop touch thread
    void triggerHardwareChange(uint8_t program) {
        if (BLEMidiClient.isConnected()) {
            BLEMidiClient.programChange(0, program);
        }
    }

 protected:
    void sendChannelMessageImpl(ChannelMessage msg) override {
        if (BLEMidiClient.isConnected() && msg.getMessageType() == MIDIMessageType::ProgramChange) {
            BLEMidiClient.programChange(msg.getChannel().getRaw(), msg.getData1());
        }
    }
    void sendSysCommonImpl(SysCommonMessage msg) override {}

    // Transmits SysEx messages via direct NimBLE 
    void sendSysExImpl(SysExMessage msg) override {
      if (msg.data && msg.length > 0) {
      sendRawBleSysEx(msg.data, msg.length);
    }
  }
    
    void sendRealTimeImpl(RealTimeMessage msg) override {}
    void sendNowImpl() override {}

  private:
    MIDI_Callbacks *callbacks = nullptr;
    static BLEMIDI_ClientInterface* instance;
};

BLEMIDI_ClientInterface* BLEMIDI_ClientInterface::instance = nullptr;
BLEMIDI_ClientInterface midi;

//MIDI CALLBACK COMPONENT
struct MyMIDICallbacks : public MIDI_Callbacks {
    void onChannelMessage(MIDI_Interface & /*interface*/, ChannelMessage message) override {
        if (showWifiSetupUI) return;
        if (message.getMessageType() == MIDIMessageType::ProgramChange && message.getChannel() == Channel(0)) {
            latestMidiProgram = message.getData1();
            newMidiMessageAvailable = true;
        }
    } 
void onSysExMessage(MIDI_Interface & /*interface*/, SysExMessage message) override {
// This is Handled directly inside the NimBLE characteristic notification callback
}   
};
MyMIDICallbacks myMIDIhandler; // Global instance
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// SETUP 
void setup() {
    
    Serial.begin(115200);
    
    // ==========================================
    // TEMPORARY: Hardcode your credentials for testing
    // ==========================================
    //ssid = "YOUR WiFi SSID ";          
    //password = "YOUR WiFi PASSWORD";  
    //saveCredentials();                // save them to Flash memory (NVS)
    // ==========================================

    // Allocate bootup  sysex buffer in RAM
    sysExBootBuffer = (uint8_t*)malloc(SYSEX_BUFFER_SIZE);
    if (sysExBootBuffer != nullptr) {
    memset(sysExBootBuffer, 0, SYSEX_BUFFER_SIZE);
    }
    
    if (!LittleFS.begin(true)) { // check if LittleFS is loaded
        return;
    }

    // INITIALIZE GPIO PINs 
    pinMode(BUTTON_PREV_PIN, INPUT_PULLUP);
    pinMode(BUTTON_NEXT_PIN,   INPUT_PULLUP);
    pinMode(BUTTON_SEND_PIN, INPUT_PULLUP);
   
    loadPatchesFromFlash(); 

    tft.init();
    tft.setRotation(1); 
    tft.fillScreen(TFT_BLACK);

    // Touch calibration procedure
    touch_calibrate();

    // Font Extraction Fallback
    if (LittleFS.exists("/NotoSansJP-Bold36.vlw")) { 
        tft.loadFont("NotoSansJP-Bold36", LittleFS); 
    } else {
        tft.setTextFont(4); 
    }

    loadCredentials();
     
    // Wi-Fi Connect Sequence
    if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), password.c_str());
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 10) { 
            delay(500);
            attempts++;
        }
    }

  // Initialize mDNS responder for "patch.local"
  if (!MDNS.begin("patch")) {
    return;
  }
  
    // Init MainUI
    showWifiSetupUI = false;
    drawMainUI();

    tft.unloadFont(); // clear setup fonts from RAM

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    server.on("/", HTTP_GET, handleRoot);
    server.begin();
    
    //Init BLE MIDI ---
    midi.begin(); 
    midi.setCallbacks(myMIDIhandler); 
    Control_Surface.begin();  
    
    
   
    
}

//  LOOP 
void loop() {
    server.handleClient();
    webSocket.loop();
    Control_Surface.loop();

    
    // Background Checking BLE MIDI for available midi
    if (newMidiMessageAvailable) {
        newMidiMessageAvailable = false; // Reset the flag immediately
        preselectedMidiProgram = latestMidiProgram; // Sync staged patch with ME-90 active patch!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
        renderNewPatch(latestMidiProgram, false); // Draw inside the main thread stack ( MIDI is Fast)
    }

    //Realtime WiFi Status WATCHDOG
    static wl_status_t lastStatus = WL_IDLE_STATUS;
    wl_status_t currentStatus = WiFi.status();
    
    //Only re-render Status Line if in setup and NOT waiting on a post-setup screen redraw
      if (showWifiSetupUI && !postSetupResetRequired && currentStatus != lastStatus) {
        lastStatus = currentStatus;
        updateWiFiStatusLine();
    } 
    //Only re-render Wifi Icon if in mainUI and NOT waiting on a post-setup screen redraw
      if (!showWifiSetupUI && !postSetupResetRequired && currentStatus != lastStatus) {
        lastStatus = currentStatus;
        updateWiFiIcon();
    } 
    
        //HARDWARE GPIO PROCESSING 
    if (!showWifiSetupUI) {
        
        // 1. Physical DOWN button click handler
        if (digitalRead(BUTTON_PREV_PIN) == LOW) {
            if (preselectedMidiProgram > 0) {
                preselectedMidiProgram--;
            } else {
                preselectedMidiProgram = TOTAL_PATCHES - 1;
            }
            renderNewPatch(preselectedMidiProgram, true); 
            delay(250); // Debounce
        }

        // 2. Physical UP button click handler
        if (digitalRead(BUTTON_NEXT_PIN) == LOW) {
            if (preselectedMidiProgram < TOTAL_PATCHES - 1) {
                preselectedMidiProgram++;
            } else {
                preselectedMidiProgram = 0;
            }
            renderNewPatch(preselectedMidiProgram, true); 
            delay(250); // Debounce
        }

        // 3. Physical SEND button click handler
        if (digitalRead(BUTTON_SEND_PIN) == LOW) {
            midi.triggerHardwareChange(preselectedMidiProgram);
            latestMidiProgram = preselectedMidiProgram;
            renderNewPatch(latestMidiProgram, false);
            delay(300); // Debounce
        }
      }
  // Contextual UI Processing
    if (showWifiSetupUI) {
        handleTouch();
    } else {
        handleMainScreenTouch();
    }
    

}

/////////////////////////////////////////////////////// FUNCTIONS///////////////////////////////////////////////////

////////////////////////////////////MainUI/////////////////////////////////////
void drawMainUI() {
           
  tft.fillScreen(TFT_BLACK);

  tft.fillRect(0, 70, 480, 180, TFT_WHITE);  // White retangle behind patchnames

  tft.loadFont("NotoSansJP-Bold36", LittleFS);
  
  tft.fillRect(0, 260, 150, 60, TFT_ORANGE); tft.drawRect(0, 260, 150, 60, TFT_WHITE); 
  tft.setTextColor(TFT_WHITE, TFT_ORANGE); tft.drawCentreString("PREV", 0 + (150/2), 274, 2); 
  tft.fillRect(160, 260, 155, 60, TFT_DARKGREEN);  tft.drawRect(160, 260, 155, 60, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN); tft.drawCentreString("SEND", 165 + (155/2), 274, 2);
  tft.fillRect(325, 260, 155, 60, TFT_ORANGE); tft.drawRect(325, 260, 155, 60, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_ORANGE); tft.drawCentreString("NEXT", 325 + (155/2), 274, 2);

  
  tft.setTextColor(TFT_RED, TFT_BLACK); 
  tft.drawString("PATCH: ", 80, 20);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("---", 250, 20); // Display --- until ME-90 responds on boot
 
  tft.unloadFont();

updateWiFiIcon();
}

  
// Load Patches from flash
void loadPatchesFromFlash() { 
  File file = LittleFS.open("/patches.txt", FILE_READ); 
  if (!file) { 
    for(int i=0; i<TOTAL_PATCHES; i++) {
        patchNames[i] = "Preset " + String(i+1);
        patchColors[i] = TFT_WHITE;
    }
    return; 
  }
  int index = 0;
  while (file.available() && index < TOTAL_PATCHES) {
    String line = file.readStringUntil('\n');
    line.trim(); 
    if (line.length() > 0) {
      int pipeIndex = line.indexOf('|');
      if(pipeIndex != -1) {
         patchNames[index] = line.substring(0, pipeIndex);
         String colorHex = line.substring(pipeIndex + 1);
         patchColors[index] = (uint16_t)strtol(colorHex.c_str(), NULL, 16);
      } else {
         patchNames[index] = line;
         patchColors[index] = TFT_WHITE; // Default fallback if no pipe char exists yet
      }
      index++;
    }
  }
  file.close(); 
}

// New Patchname rendering
void renderNewPatch(byte program, bool isPreselected) {  
    char prefix = 'U';
    int bankNum = 0;
    int patchNum = 0;
    const char* currentName = "Empty Patch";
    uint16_t currentBgColor = TFT_WHITE; 

    if (program >= 0 && program <= 35) {
        prefix = 'U';
        bankNum = (program / 4) + 1;
        patchNum = (program % 4) + 1;
    } 
    else if (program >= 36 && program <= 71) {
        prefix = 'P';
        int adjustedPC = program - 36;
        bankNum = (adjustedPC / 4) + 1;
        patchNum = (adjustedPC % 4) + 1;
    } else {
        return; 
    }

    if (program < TOTAL_PATCHES) {
        if (patchNames[program].length() > 0) {
            currentName = patchNames[program].c_str();
        }
        currentBgColor = patchColors[program]; 
    }
    
    char displayBuffer[16];
    snprintf(displayBuffer, sizeof(displayBuffer), "%c%d-%d", prefix, bankNum, patchNum);
    
    tft.loadFont("NotoSansJP-Bold36", LittleFS);

    // Change label header text color and send button color based on staging state tracking
    tft.fillRect(0, 0, 250, 65, TFT_BLACK);
    if (isPreselected) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("STAGED: ", 80, 20);

    tft.fillRect(160, 260, 155, 60, TFT_RED);  //send button
    tft.drawRect(160, 260, 155, 60, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("SEND", 165 + (155/2), 274, 2);
    } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("PATCH:  ", 80, 20);

    tft.fillRect(160, 260, 155, 60, TFT_DARKGREEN); //send button
    tft.drawRect(160, 260, 155, 60, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawCentreString("SEND", 165 + (155/2), 274, 2);
    }
    
    tft.fillRect(230, 0, 145, 70, TFT_BLACK);  
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(displayBuffer, 250, 20);

    tft.fillRect(0, 70, 480, 180, currentBgColor);  
    tft.unloadFont(); 
  
    tft.loadFont("NotoSansJP-Bold56", LittleFS); 
    uint16_t textColor = (currentBgColor < 0x7BEF) ? TFT_WHITE : TFT_BLACK;
    tft.setTextColor(textColor, currentBgColor);

    int16_t xPadding = 10;  //Layout padding
    int16_t yPadding = 10;
    int16_t boxX = 0;
    int16_t boxY = 70;
    int16_t boxWidth = 480;
    int16_t boxHeight = 180;

    drawWordWrappedString(currentName, boxX, boxY, boxWidth, boxHeight, xPadding, yPadding); //WORD-WRAP
    tft.unloadFont(); 
   
}

// Bluetooth icon  (x, y, w, h)       //( w recommended: 10-14  h recommended: 18-24)
void drawBluetoothIcon(int x, int y, int w, int h, uint16_t color, uint16_t bgColor) {
  int cx = x + (w / 2);       // Center X (vertical spine)
  int q1 = y + (h / 4);       // Top triangle tip Y
  int q3 = y + (3 * h / 4);   // Bottom triangle tip Y
  int rightX = x + w;         // Rightmost point X
  int leftX = x;              // Leftmost point X
  float width = 3.0;          // 3.0px stroke width 

  tft.drawWideLine(cx, y, cx, y + h, width, color, bgColor); //Vertical center spine
  tft.drawWideLine(cx, y, rightX, q1, width, color, bgColor);  //Top-right loop
  tft.drawWideLine(rightX, q1, leftX, q3, width, color, bgColor);
  tft.drawWideLine(leftX, q1, rightX, q3, width, color, bgColor); //Bottom-right loop
  tft.drawWideLine(rightX, q3, cx, y + h, width, color, bgColor);
}

// Word-wrap for patchnames on TFT Display Center align
void drawWordWrappedString(String text, int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t padX, int16_t padY) {
  int16_t fontHeight = tft.fontHeight(); 
  int16_t maxX = bw - (padX * 2); // Max available width inside padding boundaries
  
  // A Bold56 font inside a 180px box can only hold ~2 to 3 lines max
  const int maxLines = 10; // Arrays to temporarily store our parsed lines
  String lines[maxLines];
  int16_t lineWidths[maxLines];
  int lineCount = 0;
  
  int startIndex = 0;
  int endIndex = text.indexOf(' ');
  String currentLineStr = "";
  int16_t currentLineWidth = 0;

  //Calculate word wrapping & group text into lines
  while (startIndex < text.length() && lineCount < maxLines) {
    String word;
    if (endIndex == -1) {
      word = text.substring(startIndex);
      startIndex = text.length(); 
    } else {
      word = text.substring(startIndex, endIndex);
      startIndex = endIndex + 1;
      endIndex = text.indexOf(' ', startIndex);
    }
    
    // Measure word width (with an assumed trailing space if it's not the last word)
    String wordWithSpace = word + ((startIndex < text.length()) ? " " : "");
    int16_t wordWidth = tft.textWidth(wordWithSpace);
    
    if (currentLineWidth + wordWidth > maxX && currentLineStr.length() > 0) {
      // Current line is full, save it
      lines[lineCount] = currentLineStr;
      lineWidths[lineCount] = tft.textWidth(currentLineStr); // Exact line pixel width
      lineCount++;
      
      currentLineStr = wordWithSpace; // Start a brand new line with the current word
      currentLineWidth = wordWidth;
    } else {
      currentLineStr += wordWithSpace; // Word fits on the current line
      currentLineWidth += wordWidth;
    }
  }
  
  if (currentLineStr.length() > 0 && lineCount < maxLines) {  // Catch the very last line remaining in buffer
    lines[lineCount] = currentLineStr;
    lineWidths[lineCount] = tft.textWidth(currentLineStr);
    lineCount++;
  }

  int16_t totalTextHeight = lineCount * fontHeight; //Calculate Centering & Render to Screen
  int16_t cursorY = by + ((bh - totalTextHeight) / 2); // Calculate dynamic vertical start coordinate (box height)
  
   if (cursorY < by + padY) cursorY = by + padY; // fallback if padding forces it too high

  for (int i = 0; i < lineCount; i++) {  // If the text line goes past the bottom boundary, drop out 
    if (cursorY + fontHeight > by + bh - padY) {
      break; 
    }
    
    lines[i].trim();  // Trim trailing spaces from the end of lines for centering
    lineWidths[i] = tft.textWidth(lines[i]);

    int16_t cursorX = bx + ((bw - lineWidths[i]) / 2); // Calculate dynamic horizontal start coordinate (box width)
    
    tft.setCursor(cursorX, cursorY);
    tft.print(lines[i]);
    
    cursorY += fontHeight; // downward by one line height for the next 
  }
}


// Handling MainUI Toughscreen
void handleMainScreenTouch() {
    uint16_t t_x = 0, t_y = 0;
    if (tft.getTouch(&t_x, &t_y)) {
        // WiFi Setup area 
        if (t_x >= 370 && t_x <= 480 && t_y >= 0 && t_y <= 80) {
            showWifiSetupUI = true;
            editingPassword = false;
            drawUI(); 
            delay(300);
            
            return;  
        }
        // Preselect buttons
        // PREVIOUS BUTTON 
       if (t_x >= 0 && t_x <= 150 && t_y >= 260 && t_y <= 320) 
       {
        if (preselectedMidiProgram > 0) {
        preselectedMidiProgram--;
        } else {
        preselectedMidiProgram = TOTAL_PATCHES - 1; // Loop true start
        }
        renderNewPatch(preselectedMidiProgram, true); // True stages the patch layout
        delay(250); // anti-bounce protection
        return;
        }

        // NEXT patch BUTTON 
        if (t_x >= 325 && t_x <= 480 && t_y >= 260 && t_y <= 320) 
        {
        if (preselectedMidiProgram < TOTAL_PATCHES - 1) {
        preselectedMidiProgram++;
        } else {
        preselectedMidiProgram = 0; // Loop back down to first index    
        }
        renderNewPatch(preselectedMidiProgram, true);
        delay(250);
        return;
        }

        // SEND BUTTON 
        if (t_x >= 160 && t_x <= 315 && t_y >= 260 && t_y <= 320)  
        {
        // Instantly transmit the staged selection straight down the BLE pipeline
        midi.triggerHardwareChange(preselectedMidiProgram);
        // Sync current active position tracking marker
        latestMidiProgram = preselectedMidiProgram;
        renderNewPatch(latestMidiProgram, false); // Switch back to active red theme layout
        delay(300);
        return;
        }
         
      }
}


////////////////////////////////////////////WiFi Setup UI/////////////////////////////////////////////

void drawUI() {  
  tft.fillScreen(TFT_SKYBLUE);
  tft.loadFont("NotoSansJP-Bold14", LittleFS);
  tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
  tft.drawString("(patch.local)", 195, 15);
  tft.unloadFont();
  tft.loadFont("NotoSansJP-Bold18", LittleFS);

  // Field boxes
  tft.drawRect(10, 50, 460, 35, editingPassword ? TFT_BLACK : TFT_RED);
  tft.fillRect(11, 51, 458, 33, TFT_LIGHTGREY);
  tft.drawString("SSID: " + ssid, 20, 58);

  tft.drawRect(10, 95, 460, 35, editingPassword ? TFT_RED : TFT_BLACK);
  tft.fillRect(11, 96, 458, 33, TFT_LIGHTGREY);
  tft.drawString("PASS: " + (editingPassword ? password : "*****"), 20, 103);

  // TEST Button
  tft.drawRect(300, 10, 80, 30, TFT_BLACK);
  tft.fillRect(301, 11, 78, 28, TFT_LIGHTGREY);
  tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
  tft.drawCentreString("Check", 300 + (80 / 2), 15, 2);

  // OK Button
  tft.drawRect(390, 10, 80, 30, TFT_BLACK);
  tft.fillRect(391, 11, 78, 28, TFT_LIGHTGREY);
  // Text OK string handled by updateWiFiStatusLine()
  
// Key matrix frame construction
  for (int r = 0; r < KEY_ROWS; r++) {
    for (int c = 0; c < KEY_COLS; c++) {
      int x = keyStartX + (c * keyW);
      int y = keyStartY + (r * keyH);
      tft.drawRect(x, y, keyW - 2, keyH - 2, TFT_BLACK);
      
      // Draw static non-changing keys right away
      char key = keys[r][c];
      if (key == '<') {
        tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
        tft.drawString("<-", x + 14, y + (keyH/2) - 8);
      } else if (key == ' ' || (key >= '0' && key <= '9') || key == '_') {
        tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
        String keyStr = String(key);
        tft.drawString(keyStr, x + (keyW/2) - 6, y + (keyH/2) - 8);
      }
    }
  }

redrawKeyFacesOnly(); // when Shift/Caps is toggled only change the involved Keys

  tft.unloadFont();
  tft.loadFont("NotoSansJP-Bold36", LittleFS);
}

// when Shift/Caps is toggled only change the involved Keys
void redrawKeyFacesOnly() {
  tft.loadFont("NotoSansJP-Bold18", LittleFS);
  
  for (int r = 0; r < KEY_ROWS; r++) {
    for (int c = 0; c < KEY_COLS; c++) {
      char key = keys[r][c];
      
      if ((key >= 'a' && key <= 'z') || key == '^') { //Only the keys that need to be changed
        int x = keyStartX + (c * keyW);
        int y = keyStartY + (r * keyH);
        
        tft.fillRect(x + 1, y + 1, keyW - 4, keyH - 4, TFT_SKYBLUE); // wipe previous keys
        
        tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
        if (key == '^') {
          // Redraw the shift arrow in the correct color 
          uint16_t arrowColor = isShiftActive ? TFT_RED : TFT_BLACK;
          int centerX = x + (keyW / 2);
          int centerY = y + (keyH / 2);
          
          tft.fillTriangle(centerX, centerY - 7, centerX - 7, centerY + 1, centerX + 7, centerY + 1, arrowColor);
          tft.fillRect(centerX - 3, centerY + 1, 6, 6, arrowColor);
        } else {
          // Convert the current key character to uppercase when shift active
          if (isShiftActive) {
            key = key - 32;
          }
          String keyStr = String(key);
          tft.drawString(keyStr, x + (keyW/2) - 6, y + (keyH/2) - 8);
        }
      }
    }
  }
  tft.unloadFont();

  updateWiFiStatusLine(); // WiFi connection statusbar update
}

// Handling WiFi Setup UI Toughscreen
void handleTouch() {
  uint16_t t_x = 0, t_y = 0;
  if (!tft.getTouch(&t_x, &t_y)) return;

  // Check Button
  if (t_x >= 300 && t_x <= 380 && t_y >= 10 && t_y <= 40) { 
          saveCredentials();
          WiFi.disconnect(true);
          delay(100);
          WiFi.begin(ssid.c_str(), password.c_str());
          
  }

  // OK Button -> Submit & exit
  if (t_x >= 390 && t_x <= 470 && t_y >= 10 && t_y <= 40) { 
          saveCredentials();
          WiFi.disconnect(true);
          delay(100);
          WiFi.begin(ssid.c_str(), password.c_str());

          showWifiSetupUI = false; 
          
          if (!BLEMidiClient.isConnected()) {
              postSetupResetRequired = true;
              drawMainUI(); 
          } else if (BLEMidiClient.isConnected()) {    
              drawMainUI();
              drawBluetoothIcon(440, 14, 14, 28, TFT_GREEN, TFT_BLACK);
             // renderNewPatch(latestMidiProgram,preselectedMidiProgram);
              renderNewPatch(latestMidiProgram, latestMidiProgram != preselectedMidiProgram);

          }
        
          delay(300);
          return; 
  }

  // Header Box Field Swaps (Requires full redraw because borders swap colors)
  if (t_x >= 10 && t_x <= 470) {
    if (t_y >= 45 && t_y <= 80)  { editingPassword = false; drawUI(); delay(200); return; }
    if (t_y >= 90 && t_y <= 125) { editingPassword = true;  drawUI(); delay(200); return; }
  }

  // Keyboard array mapping
  for (int r = 0; r < KEY_ROWS; r++) {
    for (int c = 0; c < KEY_COLS; c++) {
      int x = keyStartX + (c * keyW);
      int y = keyStartY + (r * keyH);

      if (t_x >= x && t_x <= (x + keyW) && t_y >= y && t_y <= (y + keyH)) {
        char key = keys[r][c];

        if (key == '^') { 
          isShiftActive = !isShiftActive;
          redrawKeyFacesOnly(); 
        }
        else if (key == '<') { 
          if (!editingPassword && ssid.length() > 0) ssid.remove(ssid.length() - 1);
          if (editingPassword && password.length() > 0) password.remove(password.length() - 1);
        }
        else { // Append key entry
          if (isShiftActive && key >= 'a' && key <= 'z') {
            key = key - 32; 
          }
          if (!editingPassword && ssid.length() < 32) ssid += key;
          if (editingPassword && password.length() < 64) password += key;
        }

        tft.loadFont("NotoSansJP-Bold18", LittleFS);
        tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);

        if (!editingPassword) {
          tft.fillRect(11, 51, 458, 33, TFT_LIGHTGREY); 
          tft.drawString("SSID: " + ssid, 20, 58);
        } else {
          tft.fillRect(11, 96, 458, 33, TFT_LIGHTGREY); 
          tft.drawString("PASS: " + password, 20, 103); 
        }
        tft.unloadFont();

        delay(200); 
        return;
      }
    }
  }
}

// WiFi connection statusbar update function
void updateWiFiStatusLine() {
  tft.loadFont("NotoSansJP-Bold18", LittleFS);
  tft.fillRect(10, 10, 180, 35, TFT_SKYBLUE);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
    String ipStr = "IP: " + WiFi.localIP().toString();
    tft.drawString(ipStr, 20, 15);
     tft.fillRect(391, 11, 78, 28, TFT_LIGHTGREY);
    tft.setTextColor(TFT_DARKGREEN, TFT_LIGHTGREY);
    tft.drawCentreString("OK", 390 + (80 / 2), 15, 2);

  } else if (WiFi.status() == WL_DISCONNECTED && ssid.length() > 0) {
    tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
    tft.drawString("Connecting...", 20, 15);
     tft.fillRect(391, 11, 78, 28, TFT_LIGHTGREY);
     tft.setTextColor(TFT_RED, TFT_LIGHTGREY);
     tft.drawCentreString("OK", 390 + (80 / 2), 15, 2);
  } else {
    tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
    tft.drawString("Not Connected", 20, 15);
    tft.fillRect(391, 11, 78, 28, TFT_LIGHTGREY);
    tft.setTextColor(TFT_RED, TFT_LIGHTGREY);
    tft.drawCentreString("OK", 390 + (80 / 2), 15, 2);
  }
  tft.unloadFont();
}

// Wifi connection Icon update function
void updateWiFiIcon() {
  if (WiFi.status() == WL_CONNECTED) {
    drawWifiIcon(400, 35, TFT_GREEN, TFT_BLACK);
  } else if (WiFi.status() == WL_DISCONNECTED){
    drawWifiIcon(400, 35, TFT_RED, TFT_BLACK);
  }
}

// Wi-Fi icon 
void drawWifiIcon(int x, int y,  uint16_t fgColor,  uint16_t bgColor) {
  
  tft.fillSmoothCircle(x, y, 2, fgColor, bgColor);
  tft.drawSmoothArc(x, y, 7, 5, 135, 225, fgColor, bgColor, true);
  tft.drawSmoothArc(x, y, 12, 10, 135, 225, fgColor, bgColor, true);
  tft.drawSmoothArc(x, y, 17, 15, 135, 225, fgColor, bgColor, true);
}

//NVS flash for WiFi SSID and Password save and load
void saveCredentials() {
  preferences.begin("wifi-cfg", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.end();
}

void loadCredentials() {
  preferences.begin("wifi-cfg", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  preferences.end();
}


///////////////////////////////////////////// WEB SERVER//////////////////////////////////////////////

// Function: Converts HTML hex string (#RRGGBB) to 16-bit 565 TFT color format
uint16_t hexTo565(String hexString) {
  if (hexString.startsWith("#")) {
    hexString.remove(0, 1);
  }
  // Fallback to white if the string length is invalid
  if (hexString.length() != 6) return TFT_WHITE; 
  long number = strtol(hexString.c_str(), NULL, 16);
  long r = (number >> 16) & 0xFF;
  long g = (number >> 8) & 0xFF;
  long b = number & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Function: Converts 16-bit 565 color values back to HTML Hex strings
String color565ToHex(uint16_t color) {
  // Extract RGB components from 565 format
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;

  // Scale components up to standard 8-bit depth
  r = (r * 527 + 23) >> 6;
  g = (g * 259 + 33) >> 6;
  b = (b * 527 + 23) >> 6;

  // Format as a 6-character hex string padded with zeros
  char hexBuffer[8];
  snprintf(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X", r, g, b);
  return String(hexBuffer);
}

 
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            break;
        
       case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                // Construct sync payload string: "INIT_NAMES:Name1|#color1,Name2|#color2..."
                String fullList = "INIT_NAMES:";
                for(int i = 0; i < TOTAL_PATCHES; i++) {
                    String hexStr = color565ToHex(patchColors[i]);
                    fullList += patchNames[i] + "|" + hexStr;
                    if (i < TOTAL_PATCHES - 1) {
                        fullList += ",";
                    }
                }
                webSocket.sendTXT(num, fullList);
            }
            break;

        case WStype_TEXT:
            {
                payload[length] = '\0';
                String text = String((char*)payload);
                text.trim();

                if (text.startsWith("UPDATE_PATCH|")) {
                    // format: UPDATE_PATCH|index|New Name|#HexColor
                    int firstPipe  = text.indexOf('|');
                    int secondPipe = text.indexOf('|', firstPipe + 1);
                    int thirdPipe  = text.indexOf('|', secondPipe + 1);
                    
                    if (secondPipe != -1 && thirdPipe != -1) {
                        int patchId = text.substring(firstPipe + 1, secondPipe).toInt();
                        String newName = text.substring(secondPipe + 1, thirdPipe);
                        String hexColor = text.substring(thirdPipe + 1);
                        
                        newName.trim();
                        hexColor.trim();

                        if (patchId >= 0 && patchId < TOTAL_PATCHES) {
                            patchNames[patchId] = newName; 
                            patchColors[patchId] = hexTo565(hexColor); // Converts to 16-bit format
                            
                            savePatchesToFlash(); // Saves changes to LittleFS

                            String echoMessage = "Saved Patch " + String(patchId + 1) + " Layout Success";
                            webSocket.sendTXT(num, echoMessage);
                        }
                    }
                } 
                else {
                    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
                    tft.drawString(text.c_str(), 10, 50);
                    
                    String echoMessage = "Displayed: " + text + " on Screen";
                    webSocket.sendTXT(num, echoMessage);
                }
            }
            break;
        default:
            break;
    }
}

// Safe Patches to Flash
void savePatchesToFlash() {
  File f = LittleFS.open("/patches.txt", FILE_WRITE); 
  if (!f) return;
  for (int i = 0; i < TOTAL_PATCHES; i++) {
    // name and hex color profile line item 
    f.println(patchNames[i] + "|" + String(patchColors[i], HEX));
  }
  f.close();
}


void handleRoot() {
    server.send(200, "text/html", HTML_CONTENT);
}


//TOUCH CALIBRATE
void touch_calibrate() {
  uint16_t calData[5];
  uint8_t calDataOK = 0;

  // If there is existing calibration data read it from LittleFS.
  if (!REPEAT_CAL) {
    if (LittleFS.exists(CALIBRATION_FILE)) {
      File f = LittleFS.open(CALIBRATION_FILE, "r");
      // Read 5 x 16-bit = 10 bytes then data is OK
      if (f) {
        if (f.readBytes((char *)calData, 10) == 10) {
          calDataOK = 1;
        }
        f.close();
      }
    }
  }

  // If calibration data is not OK restart calibration
  if (calDataOK && !REPEAT_CAL) {
    // Data has been successfully loaded
    tft.setTouch(calData);
  } else {
    // Data not OK restart calibration on screen
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(20, 20);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.println("Accurately tap the corners to calibrate.");

    // The built-in TFT_eSPI wizard (parameters: data array, color angle, background, arrow size)
    tft.calibrateTouch(calData, TFT_RED, TFT_BLACK, 15);

    //Save data to LittleFS
    File f = LittleFS.open(CALIBRATION_FILE, "w");
    if (f) {
      f.write((const uint8_t *)calData, 10);
      f.close();
    }
    
    tft.fillScreen(TFT_BLACK);
  }
}


