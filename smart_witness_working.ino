/*
 * Smart Witness ESP32-CAM - WORKING VERSION (BLE + AUTO TELEGRAM)
 * 
 * ARCHITECTURE:
 * Phase 1: BLE Configuration (WiFi + Telegram User)
 * Phase 2: AUTO TELEGRAM WORKFLOW (exact copy from working code)
 * 
 * This version combines:
 * ✅ BLE Configuration system (for initial setup)
 * ✅ AUTO Telegram OK workflow (proven working code)
 * ✅ Enhanced UI (improved web interface)
 * 
 * Key principle: Don't mix architectures - use BLE for config, then switch to AUTO Telegram
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <esp_task_wdt.h>
#include "esp_camera.h"
#include "SPIFFS.h"

// BLE INCLUDES
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEUtils.h"
#include "BLE2902.h"

// ============================================
// ESP32-CAM Pin Definitions
// ============================================
#define STATUS_LED 33
#define FLASH_LED  4

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ============================================
// HARDCODED BOT CONFIGURATION
// ============================================
const char* BOT_TOKEN = "7978858388:AAHmzB_9812cG0sW49nmIE9KHWSmfZW7Ui4";
const char* BOT_NAME = "@SmartWitnessBot";

// ============================================
// BLE CONFIGURATION UUIDs
// ============================================
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHAR_CONFIG_UUID    "87654321-4321-4321-4321-cba987654321"
#define CHAR_STATUS_UUID    "11111111-2222-3333-4444-555555555555"
#define CHAR_DEVICE_ID_UUID "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"

// ============================================
// SYSTEM STATES  
// ============================================
enum SystemState {
    INIT,
    BLE_CONFIG,
    AUTO_TELEGRAM_WORKFLOW,
    OPERATIONAL,
    ERROR
};

// ============================================
// AUTO TELEGRAM PHASES (from working code)
// ============================================
enum TestPhase {
    PHASE_PERSONAL_CONFIG,
    PHASE_WAITING_FOR_GROUP,
    PHASE_GROUP_CONFIGURED,
    PHASE_COMPLETE
};

// ============================================
// CONFIGURATION STRUCTURES
// ============================================
struct SmartWitnessConfig {
    char wifiSSID[32];
    char wifiPassword[32];
    char telegramUser[32];
    char deviceName[32];
    char personalChatId[20];
    char groupChatId[20];
    char alertMsg[100];
    bool isConfigured;
    uint32_t configVersion;
};

struct BLESession {
    bool configReceived;
    bool wifiTested;
    bool isComplete;
    unsigned long startTime;
} bleSession;

// ============================================
// AUTO TELEGRAM VARIABLES (from working code)
// ============================================
String deviceId = "";                    // Global Device ID for Telegram
String personalChatId = "";              // Personal chat ID
String groupChatId = "";                 // Group chat ID  
TestPhase currentPhase = PHASE_PERSONAL_CONFIG;
unsigned long phaseStartTime = 0;
int pollAttempts = 0;

// Timing from working code
const unsigned long POLL_INTERVAL = 2000;
const unsigned long PERSONAL_CONFIG_TIMEOUT = 120000;
const unsigned long GROUP_WAIT_TIMEOUT = 300000;
const int MAX_POLL_ATTEMPTS = 150;

// ============================================
// GLOBAL VARIABLES
// ============================================
SystemState currentState = INIT;
SmartWitnessConfig config;
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr;

// BLE Objects
BLEServer* pServer = nullptr;
BLEService* pService = nullptr;
BLECharacteristic* pConfigCharacteristic = nullptr;
BLECharacteristic* pStatusCharacteristic = nullptr;
BLECharacteristic* pDeviceIdCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// ============================================
// FORWARD DECLARATIONS
// ============================================
void setupWatchdog();
void feedWatchdog();
bool initCamera();
void setupTelegram();

// Configuration
void loadDefaultConfig();
void loadConfig();
void saveConfig();
bool validateConfiguration();
void showStoredConfig();

// BLE functions
void initBLE();
void startBLEConfiguration();
void processBLEConfiguration(String configData);
void updateBLEStatus(String status);
bool testWiFiCredentials(String ssid, String password);
void completeBLEConfiguration();

// AUTO TELEGRAM FUNCTIONS (exact copy from working code)
void startPersonalConfigPhase();
void handlePersonalConfigPhase();
void startGroupWaitingPhase();
void handleGroupWaitingPhase();
bool pollForPersonalConfig();
bool pollForGroupDetection();
bool handlePersonalConfigMessage(String chat_id, String text, String userName);
void sendGroupCreationInstructions(String personal_chat_id);
void saveChatIdsToEEPROM(String personal_id, String group_id);

// Camera and messaging
void enableFlash(bool enable);
bool captureAndSendTestPhoto(String chat_id, String context);
bool sendPhotoTelegram(String chat_id, camera_fb_t* fb);
bool safeSendMessage(String chat_id, String message, String keyboard = "");

// Utility functions  
String generateDeviceId();
String normalizeTelegramUsername(String username);
bool validateTelegramUsername(String username);
void handleSerialCommands();
void printTestSummary();

// State handlers
void handleInit();
void handleBLEConfig();
void handleAutoTelegramWorkflow();
void handleOperational();
void handleError();

// ============================================
// BLE SERVER CALLBACKS
// ============================================
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("✅ BLE Client connected");
        digitalWrite(STATUS_LED, HIGH);
    };

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("❌ BLE Client disconnected");
        digitalWrite(STATUS_LED, LOW);
        
        if (!bleSession.isComplete) {
            BLEDevice::startAdvertising();
            Serial.println("🔄 BLE Advertising restarted");
        }
    }
};

class ConfigCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String value = pCharacteristic->getValue().c_str();
        Serial.println("📝 BLE Config received: " + value);
        
        if (value.length() > 0) {
            processBLEConfiguration(value);
        }
    }
};

// ============================================
// SETUP FUNCTION
// ============================================
void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n🚨🚨🚨 SMART WITNESS WORKING VERSION 🚨🚨🚨");
    Serial.println("=== BLE Configuration + AUTO Telegram (WORKING) ===");
    Serial.println("Architecture: BLE Config → AUTO Telegram Workflow");
    Serial.println("Web Interface: https://arielzubigaray.github.io/smart-witness-config-fixed/");
    Serial.println("🎯 Using proven AUTO Telegram logic that works!");
    Serial.println("===============================================\n");
    
    setupWatchdog();
    
    // Configure LEDs
    pinMode(STATUS_LED, OUTPUT);
    pinMode(FLASH_LED, OUTPUT);
    digitalWrite(STATUS_LED, LOW);
    digitalWrite(FLASH_LED, LOW);
    
    // Initialize SPIFFS
    SPIFFS.begin(true);
    
    // Initialize camera
    if (!initCamera()) {
        Serial.println("❌ Camera initialization failed - continuing without photo capability");
    }
    
    // Load configuration
    loadConfig();
    showStoredConfig();
    
    // Initialize secured client
    secured_client.setInsecure();
    
    // Set initial state
    currentState = INIT;
    
    Serial.println("✅ Smart Witness WORKING system setup complete!");
    Serial.println("================================================\n");
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
    feedWatchdog();
    
    // Main state machine
    switch (currentState) {
        case INIT:
            handleInit();
            break;
            
        case BLE_CONFIG:
            handleBLEConfig();
            break;
            
        case AUTO_TELEGRAM_WORKFLOW:
            handleAutoTelegramWorkflow();
            break;
            
        case OPERATIONAL:
            handleOperational();
            break;
            
        case ERROR:
            handleError();
            break;
    }
    
    // Handle serial commands
    handleSerialCommands();
    
    delay(100);
}

// ============================================
// STATE HANDLERS
// ============================================
void handleInit() {
    if (validateConfiguration()) {
        Serial.println("✅ Device configured - Starting AUTO Telegram workflow");
        currentState = AUTO_TELEGRAM_WORKFLOW;
        startPersonalConfigPhase();
    } else {
        Serial.println("❌ Device not configured - Starting BLE configuration");
        currentState = BLE_CONFIG;
    }
}

void handleBLEConfig() {
    static bool bleInitialized = false;
    
    if (!bleInitialized) {
        Serial.println("🔵 Starting BLE Configuration Mode");
        Serial.println("📱 Web Interface: https://arielzubigaray.github.io/smart-witness-config-fixed/");
        
        WiFi.mode(WIFI_OFF);
        initBLE();
        startBLEConfiguration();
        bleInitialized = true;
        digitalWrite(STATUS_LED, HIGH);
    }
    
    // Status LED blinking during config
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 1000) {
        lastBlink = millis();
        digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
    }
    
    // Handle BLE connection status
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        if (!bleSession.isComplete) {
            BLEDevice::startAdvertising();
            Serial.println("🔄 BLE advertising restarted");
        }
        oldDeviceConnected = deviceConnected;
    }
    
    if (deviceConnected && !oldDeviceConnected) {
        Serial.println("✅ BLE client connected");
        digitalWrite(STATUS_LED, HIGH);
        oldDeviceConnected = deviceConnected;
    }
    
    if (bleSession.isComplete) {
        Serial.println("✅ BLE configuration complete - transitioning to AUTO Telegram workflow");
        BLEDevice::deinit();
        digitalWrite(STATUS_LED, LOW);
        currentState = AUTO_TELEGRAM_WORKFLOW;
        startPersonalConfigPhase();
        return;
    }
}

void handleAutoTelegramWorkflow() {
    // Handle different phases using exact logic from working code
    switch (currentPhase) {
        case PHASE_PERSONAL_CONFIG:
            handlePersonalConfigPhase();
            break;
            
        case PHASE_WAITING_FOR_GROUP:
            handleGroupWaitingPhase();
            break;
            
        case PHASE_GROUP_CONFIGURED:
            // Transitional phase
            Serial.println("📱 Group configured, sending confirmation photo...");
            currentPhase = PHASE_COMPLETE;
            break;
            
        case PHASE_COMPLETE:
            printTestSummary();
            currentState = OPERATIONAL;
            break;
    }
}

void handleOperational() {
    // Basic operational mode
    static unsigned long lastHealthCheck = 0;
    
    if (millis() - lastHealthCheck > 60000) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("❌ WiFi disconnected in operational mode");
            WiFi.begin(config.wifiSSID, config.wifiPassword);
        }
        lastHealthCheck = millis();
    }
}

void handleError() {
    Serial.println("=== ERROR STATE ===");
    Serial.println("System will restart in 10 seconds...");
    delay(10000);
    ESP.restart();
}

// ============================================
// BLE IMPLEMENTATION (Simplified)
// ============================================
void initBLE() {
    Serial.println("🔵 Initializing BLE Configuration Server");
    
    String deviceName = "SmartWitness_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    BLEDevice::init(deviceName.c_str());
    
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    pService = pServer->createService(SERVICE_UUID);
    
    pConfigCharacteristic = pService->createCharacteristic(
                                CHAR_CONFIG_UUID,
                                BLECharacteristic::PROPERTY_READ |
                                BLECharacteristic::PROPERTY_WRITE
                            );
    pConfigCharacteristic->setCallbacks(new ConfigCharacteristicCallbacks());
    
    pStatusCharacteristic = pService->createCharacteristic(
                                CHAR_STATUS_UUID,
                                BLECharacteristic::PROPERTY_READ |
                                BLECharacteristic::PROPERTY_NOTIFY
                            );
    pStatusCharacteristic->addDescriptor(new BLE2902());
    
    pDeviceIdCharacteristic = pService->createCharacteristic(
                                CHAR_DEVICE_ID_UUID,
                                BLECharacteristic::PROPERTY_READ |
                                BLECharacteristic::PROPERTY_NOTIFY
                            );
    pDeviceIdCharacteristic->addDescriptor(new BLE2902());
    
    pService->start();
    
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0);
    BLEDevice::startAdvertising();
    
    Serial.println("✅ BLE Server started and advertising");
    Serial.println("🔍 Device name: " + deviceName);
}

void startBLEConfiguration() {
    Serial.println("🔵 Starting BLE Configuration Session");
    
    // Reset session
    bleSession = {false, false, false, millis()};
    
    updateBLEStatus("ready_for_config");
}

void processBLEConfiguration(String configData) {
    Serial.println("📝 Processing BLE configuration...");
    updateBLEStatus("processing_config");
    
    // Parse JSON configuration
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, configData);
    
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    String telegramUser = doc["telegramUser"] | "";
    
    // Validation
    if (ssid.length() == 0 || password.length() == 0 || telegramUser.length() == 0) {
        Serial.println("❌ Invalid configuration data - missing required fields");
        updateBLEStatus("error_invalid_data");
        return;
    }
    
    if (!validateTelegramUsername(telegramUser)) {
        Serial.println("❌ Invalid Telegram username format: " + telegramUser);
        updateBLEStatus("error_invalid_telegram_user");
        return;
    }
    
    String normalizedUser = normalizeTelegramUsername(telegramUser);
    
    // Store configuration
    strncpy(config.wifiSSID, ssid.c_str(), sizeof(config.wifiSSID) - 1);
    strncpy(config.wifiPassword, password.c_str(), sizeof(config.wifiPassword) - 1);
    strncpy(config.telegramUser, normalizedUser.c_str(), sizeof(config.telegramUser) - 1);
    
    // Generate device name
    String deviceName = "SmartWitness_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    strncpy(config.deviceName, deviceName.c_str(), sizeof(config.deviceName) - 1);
    
    bleSession.configReceived = true;
    updateBLEStatus("config_received");
    
    Serial.printf("📝 Configuration parsed successfully:\n");
    Serial.printf("   Device: %s\n", deviceName.c_str());
    Serial.printf("   Telegram User: %s\n", normalizedUser.c_str());
    Serial.printf("   WiFi: %s\n", ssid.c_str());
    
    // Test WiFi connectivity
    if (testWiFiCredentials(ssid, password)) {
        bleSession.wifiTested = true;
        updateBLEStatus("wifi_tested_ok");
        
        // Generate Device ID using EXACT same logic as working code
        deviceId = generateDeviceId();
        
        // Send Device ID to web interface
        if (pDeviceIdCharacteristic != nullptr) {
            pDeviceIdCharacteristic->setValue(deviceId.c_str());
            for (int i = 0; i < 3; i++) {
                pDeviceIdCharacteristic->notify();
                delay(500);
            }
        }
        
        updateBLEStatus("device_id_generated");
        Serial.println("📱 Device ID generated: " + deviceId);
        
        // Complete BLE configuration
        completeBLEConfiguration();
        
    } else {
        Serial.println("❌ WiFi credentials test failed");
        updateBLEStatus("error_wifi_failed");
    }
}

bool testWiFiCredentials(String ssid, String password) {
    Serial.println("🌐 Testing WiFi credentials: " + ssid);
    updateBLEStatus("testing_wifi");
    
    WiFi.disconnect();
    delay(1000);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
        delay(500);
        feedWatchdog();
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi test successful");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        setupTelegram();
        return true;
    } else {
        Serial.println("\n❌ WiFi test failed");
        WiFi.disconnect();
        return false;
    }
}

void completeBLEConfiguration() {
    Serial.println("✅ Completing BLE configuration...");
    
    config.isConfigured = true;
    config.configVersion = 1;
    bleSession.isComplete = true;
    saveConfig();
    updateBLEStatus("configuration_complete");
    
    Serial.println("🔄 BLE configuration complete - ready for AUTO Telegram workflow");
}

void updateBLEStatus(String status) {
    if (pStatusCharacteristic != nullptr) {
        pStatusCharacteristic->setValue(status.c_str());
        if (deviceConnected) {
            pStatusCharacteristic->notify();
        }
    }
    Serial.println("📊 Status: " + status);
}

// ============================================
// AUTO TELEGRAM FUNCTIONS (EXACT COPY from working code)
// ============================================

void startPersonalConfigPhase() {
    Serial.println("🚀 ===== PHASE 1: PERSONAL CONFIGURATION =====");
    currentPhase = PHASE_PERSONAL_CONFIG;
    phaseStartTime = millis();
    pollAttempts = 0;
    
    Serial.println("📋 CONFIGURATION:");
    Serial.println("Device Name: " + String(config.deviceName));
    Serial.println("Expected User: " + String(config.telegramUser));
    Serial.println("Device ID: " + deviceId);
    Serial.println();
    
    Serial.println("🔗 CONFIGURATION LINK:");
    Serial.println("https://t.me/" + String(BOT_NAME).substring(1) + "?start=" + deviceId);
    Serial.println();
    
    Serial.println("⏰ Starting personal config phase (timeout: " + String(PERSONAL_CONFIG_TIMEOUT/1000) + " seconds)");
    Serial.println("🔍 Waiting for personal chat configuration...");
    Serial.println();
}

void handlePersonalConfigPhase() {
    // Check timeout
    if (millis() - phaseStartTime > PERSONAL_CONFIG_TIMEOUT) {
        Serial.println("\n⏰ PERSONAL CONFIG TIMEOUT!");
        Serial.println("❌ Failed to configure personal chat");
        printTestSummary();
        currentState = ERROR;
        return;
    }
    
    // Poll for personal config response
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll >= POLL_INTERVAL) {
        lastPoll = millis();
        
        if (pollForPersonalConfig()) {
            // Personal config successful, move to group phase
            startGroupWaitingPhase();
        }
    }
}

void startGroupWaitingPhase() {
    Serial.println("\n🚀 ===== PHASE 2: WAITING FOR GROUP CREATION =====");
    currentPhase = PHASE_WAITING_FOR_GROUP;
    phaseStartTime = millis();
    pollAttempts = 0;
    
    // Send group creation instructions
    sendGroupCreationInstructions(personalChatId);
    
    Serial.println("⏰ Waiting for group creation (timeout: " + String(GROUP_WAIT_TIMEOUT/1000) + " seconds)");
    Serial.println("🔍 Monitoring for bot addition to new group...");
    Serial.println();
}

void handleGroupWaitingPhase() {
    // Check timeout
    if (millis() - phaseStartTime > GROUP_WAIT_TIMEOUT) {
        Serial.println("\n⏰ GROUP WAITING TIMEOUT!");
        Serial.println("❌ No group detected - transitioning to operational mode with personal chat only");
        currentPhase = PHASE_COMPLETE;
        return;
    }
    
    // Poll for group detection
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll >= POLL_INTERVAL) {
        lastPoll = millis();
        
        if (pollForGroupDetection()) {
            // Group detected, send confirmation photo and transition
            delay(2000);
            if (captureAndSendTestPhoto(groupChatId, "group confirmation")) {
                currentPhase = PHASE_COMPLETE;
            }
        }
    }
}

// EXACT COPY from working code
bool pollForPersonalConfig() {
    pollAttempts++;
    
    unsigned long remainingTime = (PERSONAL_CONFIG_TIMEOUT - (millis() - phaseStartTime)) / 1000;
    
    Serial.printf("🔍 Personal config poll %d (timeout in %lu sec)...\n", pollAttempts, remainingTime);
    
    // Get new messages
    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    
    if (numNewMessages <= 0) {
        if (numNewMessages < 0) {
            Serial.println("❌ Polling failed (network error)");
        } else {
            Serial.println("📭 No new messages");
        }
        return false;
    }
    
    Serial.println("📨 Received " + String(numNewMessages) + " messages:");
    
    // Process each message looking for personal config
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = bot->messages[i].chat_id;
        String text = bot->messages[i].text;
        String userName = bot->messages[i].from_name;
        
        Serial.printf("  Message %d: '%s' from %s (chat: %s)\n", 
                      i+1, text.c_str(), userName.c_str(), chat_id.c_str());
        
        // Check if this is our personal config message
        if (handlePersonalConfigMessage(chat_id, text, userName)) {
            return true; // Personal config completed
        }
    }
    
    return false; // Continue polling
}

// EXACT COPY from working code
bool handlePersonalConfigMessage(String chat_id, String text, String userName) {
    // Check if message starts with our device ID
    String expectedCommand = "/start " + deviceId;
    
    if (!text.equals(expectedCommand)) {
        Serial.println("    ⏭️ Not our auto-config command");
        return false;
    }
    
    Serial.println("\n🎯 ===== PERSONAL CONFIG MESSAGE DETECTED =====");
    Serial.println("✅ Device ID match: " + deviceId);
    Serial.println("👤 User: " + userName);
    Serial.println("💬 Personal Chat ID: " + chat_id);
    
    // Save personal chat ID
    personalChatId = chat_id;
    strncpy(config.personalChatId, chat_id.c_str(), sizeof(config.personalChatId) - 1);
    
    // Send personal confirmation
    String confirmMsg = "✅ Smart Witness - Configuración Personal Completada!\n\n";
    confirmMsg += "📱 Dispositivo: " + String(config.deviceName) + "\n";
    confirmMsg += "🆔 Device ID: " + deviceId + "\n";
    confirmMsg += "👤 Usuario: " + userName + "\n";
    confirmMsg += "💬 Chat Personal ID: " + chat_id + "\n\n";
    confirmMsg += "📷 Enviando foto de prueba personal...";
    
    safeSendMessage(chat_id, confirmMsg);
    
    // Send test photo to personal chat
    delay(2000);
    captureAndSendTestPhoto(chat_id, "personal confirmation");
    
    Serial.println("🎉 PERSONAL CONFIGURATION COMPLETED!");
    Serial.println("===================================\n");
    
    return true;
}

// EXACT COPY from working code
bool pollForGroupDetection() {
    pollAttempts++;
    
    unsigned long remainingTime = (GROUP_WAIT_TIMEOUT - (millis() - phaseStartTime)) / 1000;
    
    Serial.printf("🔍 Group detection poll %d (timeout in %lu sec)...\n", pollAttempts, remainingTime);
    
    // Get new messages
    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    
    if (numNewMessages <= 0) {
        if (numNewMessages < 0) Serial.println("❌ Polling failed");
        else Serial.println("📭 No new messages");
        return false;
    }
    
    Serial.println("📨 Received " + String(numNewMessages) + " new messages:");
    
    // Process each message looking for group addition
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = bot->messages[i].chat_id;
        String text = bot->messages[i].text;
        String from_name = bot->messages[i].from_name;
        
        Serial.printf("  Message %d:\n", i+1);
        Serial.printf("    Chat ID: %s\n", chat_id.c_str());
        Serial.printf("    From: %s\n", from_name.c_str());
        Serial.printf("    Text: '%s'\n", text.c_str());
        
        // Detect if this is a group by chat_id (groups have negative IDs)
        bool isGroup = (chat_id.startsWith("-"));
        
        Serial.printf("    Is Group: %s\n", isGroup ? "YES" : "NO");
        
        // Check if this is a group and we were just added
        if (isGroup) {
            // Check for "bot was added" indicators or any message in new group
            if (text.indexOf("added") != -1 || text.indexOf("joined") != -1 || 
                text == "/start" || text.startsWith("/start@") || 
                chat_id != personalChatId) {  // Any message from a different chat
                
                Serial.println("\n🎯 ===== GROUP ADDITION DETECTED =====");
                Serial.println("✅ Bot was added to group!");
                Serial.println("💬 Group Chat ID: " + chat_id);
                Serial.println("📱 Detected via: Negative Chat ID");
                
                groupChatId = chat_id;
                strncpy(config.groupChatId, chat_id.c_str(), sizeof(config.groupChatId) - 1);
                saveChatIdsToEEPROM(personalChatId, groupChatId);
                
                // Send confirmation to both personal and group
                String groupConfirmMsg = "🎉 Smart Witness configurado en grupo!\n\n";
                groupConfirmMsg += "📱 Dispositivo: " + String(config.deviceName) + "\n";
                groupConfirmMsg += "💬 Group Chat ID: " + chat_id + "\n";
                groupConfirmMsg += "👥 Ahora todos los miembros recibirán fotos\n";
                groupConfirmMsg += "📷 Enviando foto de confirmación...";
                
                safeSendMessage(chat_id, groupConfirmMsg);
                
                delay(1000);
                
                String personalUpdateMsg = "✅ ¡Grupo configurado exitosamente!\n\n";
                personalUpdateMsg += "💬 Group Chat ID: " + chat_id + "\n";
                personalUpdateMsg += "👥 Las fotos ahora se enviarán al grupo\n";
                personalUpdateMsg += "🔄 Configuración completada";
                
                safeSendMessage(personalChatId, personalUpdateMsg);
                
                Serial.println("🎉 GROUP CONFIGURATION COMPLETED!");
                Serial.println("===============================\n");
                
                return true; // Group detection completed
            }
        }
    }
    
    return false; // Continue polling
}

void sendGroupCreationInstructions(String personal_chat_id) {
    Serial.println("📋 Sending group creation instructions...");
    
    String instructions = "🏁 ¡Configuración Personal Completada!\n\n";
    instructions += "🚀 SIGUIENTE PASO: Crear Grupo Compartido\n\n";
    instructions += "📋 INSTRUCCIONES:\n";
    instructions += "1️⃣ Crea un nuevo grupo en Telegram\n";
    instructions += "2️⃣ Ponle un nombre (ej: 'Smart Witness Casa')\n";
    instructions += "3️⃣ Invita a " + String(BOT_NAME) + " al grupo\n";
    instructions += "4️⃣ Invita a otros usuarios que quieras\n";
    instructions += "5️⃣ ¡Listo! El bot se configurará automáticamente\n\n";
    instructions += "⏰ Tienes 5 minutos para crear el grupo\n";
    instructions += "💡 Si no creas grupo, seguirá funcionando en chat personal\n\n";
    instructions += "🔍 Esperando detección de grupo...";
    
    bool sent = safeSendMessage(personal_chat_id, instructions);
    
    if (sent) {
        Serial.println("✅ Group creation instructions sent");
    } else {
        Serial.println("❌ Failed to send instructions");
    }
}

void saveChatIdsToEEPROM(String personal_id, String group_id) {
    Serial.println("💾 Saving chat IDs to EEPROM...");
    saveConfig(); // Save the full config
    Serial.println("✅ Chat IDs saved");
}

// ============================================
// CAMERA FUNCTIONS
// ============================================
bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    
    if(psramFound()){
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("❌ Camera init failed: 0x%x\n", err);
        return false;
    }
    
    Serial.println("✅ Camera initialized");
    return true;
}

void enableFlash(bool enable) {
    digitalWrite(FLASH_LED, enable ? HIGH : LOW);
}

bool captureAndSendTestPhoto(String chat_id, String context) {
    Serial.println("\n📷 ===== PHOTO CAPTURE (" + context + ") =====");
    
    enableFlash(true);
    delay(200);
    
    // Clear buffer
    for (int i = 0; i < 3; i++) {
        camera_fb_t * old_fb = esp_camera_fb_get();
        if (old_fb) esp_camera_fb_return(old_fb);
    }
    
    delay(500);
    camera_fb_t * fb = esp_camera_fb_get();
    enableFlash(false);
    
    if (!fb) {
        Serial.println("❌ Capture failed");
        return false;
    }
    
    Serial.printf("✅ Photo captured: %d bytes\n", fb->len);
    
    bool success = sendPhotoTelegram(chat_id, fb);
    esp_camera_fb_return(fb);
    
    if (success) {
        Serial.println("✅ Photo sent successfully!");
        
        String followUpMsg = "🎉 Smart Witness - Test de " + context + " exitoso!\n\n";
        followUpMsg += "📊 Estado del sistema:\n";
        followUpMsg += "✅ Configuración: COMPLETA\n";
        followUpMsg += "✅ Cámara: FUNCIONANDO\n";
        followUpMsg += "✅ Envío de fotos: FUNCIONANDO\n\n";
        followUpMsg += "🚀 Dispositivo listo para uso!";
        
        safeSendMessage(chat_id, followUpMsg);
        
    } else {
        Serial.println("❌ Photo send failed");
    }
    
    Serial.println("================================\n");
    return success;
}

bool sendPhotoTelegram(String chat_id, camera_fb_t* fb) {
    WiFiClientSecure client_tcp;
    client_tcp.setInsecure();

    if (client_tcp.connect("api.telegram.org", 443)) {
        String boundary = "SmartWitnessWorking" + String(millis());
        String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"chat_id\"; \r\n\r\n" + chat_id + "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"smartwitness.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
        String tail = "\r\n--" + boundary + "--\r\n";

        uint32_t totalLen = fb->len + head.length() + tail.length();

        client_tcp.println("POST /bot" + String(BOT_TOKEN) + "/sendPhoto HTTP/1.1");
        client_tcp.println("Host: api.telegram.org");
        client_tcp.println("Content-Length: " + String(totalLen));
        client_tcp.println("Content-Type: multipart/form-data; boundary=" + boundary);
        client_tcp.println();
        client_tcp.print(head);

        // Send image data
        uint8_t *fbBuf = fb->buf;
        size_t fbLen = fb->len;
        
        for (size_t n = 0; n < fbLen; n += 1024) {
            size_t chunk = (n + 1024 < fbLen) ? 1024 : fbLen % 1024;
            client_tcp.write(fbBuf, chunk);
            fbBuf += chunk;
            if (n % 10240 == 0) feedWatchdog();
        }
        
        client_tcp.print(tail);

        // Read response
        String response = "";
        unsigned long timeout = millis() + 10000;
        while (millis() < timeout && client_tcp.connected()) {
            if (client_tcp.available()) {
                response = client_tcp.readString();
                break;
            }
            delay(10);
        }
        
        client_tcp.stop();
        return response.indexOf("\"ok\":true") != -1;
    }
    
    return false;
}

// ============================================
// TELEGRAM FUNCTIONS
// ============================================
void setupTelegram() {
    if (bot != nullptr) {
        delete bot;
        bot = nullptr;
    }
    
    secured_client.setInsecure();
    bot = new UniversalTelegramBot(BOT_TOKEN, secured_client);
    bot->last_message_received = 0;
    Serial.println("✅ Telegram bot initialized");
}

bool safeSendMessage(String chat_id, String message, String keyboard) {
    if (!bot) return false;
    
    bool success = false;
    if (keyboard != "") {
        success = bot->sendMessageWithInlineKeyboard(chat_id, message, "", keyboard);
    } else {
        success = bot->sendMessage(chat_id, message, "");
    }
    
    return success;
}

// ============================================
// CONFIGURATION FUNCTIONS
// ============================================
void loadDefaultConfig() {
    memset(&config, 0, sizeof(config));
    strncpy(config.deviceName, "SmartWitness_Default", sizeof(config.deviceName) - 1);
    strncpy(config.alertMsg, "ALERTA DE SEGURIDAD ACTIVADA", sizeof(config.alertMsg) - 1);
    config.isConfigured = false;
    config.configVersion = 1;
}

void loadConfig() {
    EEPROM.begin(sizeof(config));
    EEPROM.get(0, config);
    EEPROM.end();
    
    // Validate configuration
    if (config.configVersion != 1 || strlen(config.deviceName) == 0 || strlen(config.deviceName) > 31) {
        Serial.println("Invalid or missing config found, loading defaults");
        loadDefaultConfig();
        saveConfig();
    }
}

void saveConfig() {
    EEPROM.begin(sizeof(config));
    EEPROM.put(0, config);
    EEPROM.commit();
    EEPROM.end();
    Serial.println("✅ Configuration saved to EEPROM");
}

bool validateConfiguration() {
    bool hasWiFi = (strlen(config.wifiSSID) > 0 && strlen(config.wifiPassword) > 0);
    bool hasTelegramUser = strlen(config.telegramUser) > 0;
    bool hasDeviceName = strlen(config.deviceName) > 0;
    
    Serial.printf("🔍 Configuration validation:\n");
    Serial.printf("   WiFi: %s\n", hasWiFi ? "✅" : "❌");
    Serial.printf("   Telegram User: %s\n", hasTelegramUser ? "✅" : "❌");
    Serial.printf("   Device Name: %s\n", hasDeviceName ? "✅" : "❌");
    Serial.printf("   Is Configured Flag: %s\n", config.isConfigured ? "✅" : "❌");
    
    return hasWiFi && hasTelegramUser && hasDeviceName && config.isConfigured;
}

void showStoredConfig() {
    Serial.println("\n=== Smart Witness WORKING Configuration ===");
    Serial.println("Device Name: [" + String(config.deviceName) + "]");
    Serial.println("WiFi SSID: [" + String(config.wifiSSID) + "]");
    Serial.println("Telegram User: [" + String(config.telegramUser) + "]");
    Serial.println("Personal Chat: [" + String(config.personalChatId) + "]");
    Serial.println("Group Chat: [" + String(config.groupChatId) + "]");
    Serial.println("Alert Message: [" + String(config.alertMsg) + "]");
    Serial.println("Configured: " + String(config.isConfigured ? "YES" : "NO"));
    Serial.println("Valid Config: " + String(validateConfiguration() ? "YES" : "NO"));
    Serial.println("Config Version: " + String(config.configVersion));
    Serial.println("============================================\n");
}

// ============================================
// UTILITY FUNCTIONS
// ============================================

// EXACT COPY from working code
String generateDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    String id = "DEVICE_GROUP_";
    id += String((uint32_t)(mac >> 32), HEX);
    id += String((uint32_t)mac, HEX);
    id += "_";
    id += String(millis());
    id.toUpperCase();
    return id;
}

String normalizeTelegramUsername(String username) {
    username.trim();
    username.toLowerCase();
    if (!username.startsWith("@")) {
        username = "@" + username;
    }
    return username;
}

bool validateTelegramUsername(String username) {
    username = normalizeTelegramUsername(username);
    
    if (username.length() < 2) return false;
    if (username.length() > 33) return false;
    if (!username.startsWith("@")) return false;
    
    for (int i = 1; i < username.length(); i++) {
        char c = username.charAt(i);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    
    return true;
}

void setupWatchdog() {
    esp_task_wdt_init(8000, true);
    esp_task_wdt_add(NULL);
}

void feedWatchdog() {
    esp_task_wdt_reset();
}

void printTestSummary() {
    Serial.println("\n📊 ===== WORKING VERSION TEST SUMMARY =====");
    Serial.println("Device Name: " + String(config.deviceName));
    Serial.println("Current State: " + String(currentState));
    Serial.println("Current Phase: " + String(currentPhase));
    Serial.println("Personal Chat ID: " + personalChatId);
    Serial.println("Group Chat ID: " + groupChatId);
    Serial.println("Total Poll Attempts: " + String(pollAttempts));
    
    if (currentPhase == PHASE_COMPLETE) {
        Serial.println("\n🎉 WORKING VERSION COMPLETED SUCCESSFULLY!");
        Serial.println("✅ BLE Configuration: COMPLETE");
        Serial.println("✅ Personal chat: CONFIGURED");
        Serial.println("✅ AUTO Telegram workflow: COMPLETE");
        if (groupChatId.length() > 0) {
            Serial.println("✅ Group detection: SUCCESS");
            Serial.println("✅ Group photo: SENT");
            Serial.println("🎯 Photos will be sent to group: " + groupChatId);
        } else {
            Serial.println("⚠️ Group detection: TIMEOUT");
            Serial.println("🎯 Photos will be sent to personal: " + personalChatId);
        }
        Serial.println("💾 Configuration saved to EEPROM");
        Serial.println("🔄 System ready for operational use!");
    } else {
        Serial.println("\n❌ WORKING VERSION INCOMPLETE");
        Serial.println("🔍 Check logs for specific failure point");
    }
    
    Serial.println("==========================================\n");
}

void handleSerialCommands() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        if (cmd == "reset") {
            ESP.restart();
        }
        else if (cmd == "status") {
            Serial.println("\n📊 WORKING VERSION STATUS:");
            Serial.println("State: " + String(currentState));
            Serial.println("Phase: " + String(currentPhase));
            Serial.println("Device ID: " + deviceId);
            Serial.println("Personal Chat: " + personalChatId);
            Serial.println("Group Chat: " + groupChatId);
            Serial.println("Poll attempts: " + String(pollAttempts));
            Serial.println("WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
            Serial.println();
        }
        else if (cmd == "config") {
            showStoredConfig();
        }
        else if (cmd == "deviceid") {
            if (deviceId.length() > 0) {
                Serial.println("📱 Device ID: " + deviceId);
                Serial.println("🔗 Telegram Link: https://t.me/" + String(BOT_NAME).substring(1) + "?start=" + deviceId);
            } else {
                Serial.println("❌ No Device ID generated yet");
            }
        }
        else if (cmd == "photo") {
            if (personalChatId.length() > 0) {
                captureAndSendTestPhoto(personalChatId, "manual test");
            } else {
                Serial.println("❌ No personal chat ID configured");
            }
        }
        else if (cmd == "ble") {
            Serial.println("🚀 Forcing BLE configuration mode");
            currentState = BLE_CONFIG;
        }
        else if (cmd == "resetconfig") {
            Serial.println("Resetting configuration to defaults");
            loadDefaultConfig();
            saveConfig();
            ESP.restart();
        }
        else if (cmd == "summary") {
            printTestSummary();
        }
        else if (cmd == "help") {
            Serial.println("\n🆘 ===== WORKING VERSION COMMANDS =====");
            Serial.println("status    - Show system status");
            Serial.println("config    - Show stored configuration");
            Serial.println("deviceid  - Show Device ID and link");
            Serial.println("photo     - Take manual test photo");
            Serial.println("ble       - Force BLE configuration mode");
            Serial.println("summary   - Show workflow summary");
            Serial.println("resetconfig - Reset to defaults and restart");
            Serial.println("reset     - Restart ESP32");
            Serial.println("help      - Show this help");
            Serial.println("====================================\n");
        }
    }
}