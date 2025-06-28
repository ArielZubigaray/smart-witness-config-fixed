/*
 * Smart Witness ESP32-CAM - FIXED INTEGRATED BLE + AUTO TELEGRAM SYSTEM
 * 
 * FIXES APPLIED:
 * ✅ Enhanced BLE communication with proper Device ID notification
 * ✅ Extended timeouts for better user experience
 * ✅ Better status reporting to web interface
 * ✅ Improved error handling and logging
 * ✅ Enhanced serial debugging commands
 * 
 * INTEGRATION FEATURES:
 * ✅ BLE Configuration Phase (WiFi + Telegram User setup)
 * ✅ Auto Telegram Workflow (Personal → Group Detection → Photos)
 * ✅ Web BLE Interface for easy configuration
 * ✅ Auto-generated Device IDs
 * ✅ EEPROM Configuration Storage
 * ✅ Enhanced Auto-Response during BLE config
 * ✅ Professional state machine architecture
 * 
 * WORKFLOW:
 * 1. BLE Config Mode (if not configured)
 * 2. Personal Telegram Configuration
 * 3. Group Creation Instructions
 * 4. Automatic Group Detection
 * 5. Operational Mode with Photos
 * 
 * CONFIGURABLE VIA BLE:
 * - WiFi Credentials (SSID/Password)
 * - Telegram Username
 * - Device Name (auto-generated)
 * 
 * HARDCODED (Security):
 * - Bot Token
 * - Bot Name
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
// HARDCODED BOT CONFIGURATION (Security)
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
#define CHAR_CHAT_ID_UUID   "ffffffff-eeee-dddd-cccc-bbbbbbbbbbbb"

// ============================================
// SYSTEM STATES
// ============================================
enum SystemState {
    INIT,
    BLE_CONFIG,
    AUTO_PERSONAL_CONFIG,
    AUTO_GROUP_WAIT,
    AUTO_OPERATIONAL,
    ERROR
};

// ============================================
// CONFIGURATION STRUCTURES
// ============================================
struct SmartWitnessConfig {
    char wifiSSID[32];
    char wifiPassword[32];
    char telegramUser[32];        // @username format
    char deviceName[32];          // Auto-generated
    char personalChatId[20];      // From auto-config
    char groupChatId[20];         // From group detection
    char alertMsg[100];
    bool isConfigured;
    uint32_t configVersion;       // For future updates
};

// ============================================
// BLE SESSION MANAGEMENT
// ============================================
struct BLESession {
    String deviceId;              // Auto-generated for Telegram
    String expectedTelegramUser;  // Normalized @username
    String tempChatId;            // Temporary during config
    bool configReceived;
    bool wifiTested;
    bool telegramConfigured;
    bool isComplete;
    unsigned long startTime;
    
    // Auto-response capabilities
    bool autoResponseEnabled;
    bool isPollingActive;
    unsigned long lastPollTime;
    int pollAttempts;
    int autoResponsesSent;
    bool waitingForTelegramUser;
    unsigned long autoResponseTimeout;
} bleSession;

// ============================================
// AUTO TELEGRAM WORKFLOW STATES
// ============================================
enum AutoTelegramPhase {
    AUTO_PHASE_PERSONAL_CONFIG,
    AUTO_PHASE_WAITING_FOR_GROUP,
    AUTO_PHASE_GROUP_CONFIGURED,
    AUTO_PHASE_COMPLETE
};

struct AutoTelegramSession {
    AutoTelegramPhase currentPhase;
    unsigned long phaseStartTime;
    int pollAttempts;
    String personalChatId;
    String groupChatId;
} autoSession;

// ============================================
// ENHANCED TIMING CONFIGURATIONS (FIXED)
// ============================================
const unsigned long BLE_SESSION_TIMEOUT = 900000;        // 15 minutes (extended)
const unsigned long AUTO_RESPONSE_POLL_INTERVAL = 2000;  // 2 seconds
const unsigned long AUTO_RESPONSE_TIMEOUT = 600000;      // 10 minutes (extended)
const unsigned long PERSONAL_CONFIG_TIMEOUT = 180000;    // 3 minutes (extended)
const unsigned long GROUP_WAIT_TIMEOUT = 300000;         // 5 minutes
const int MAX_AUTO_RESPONSE_ATTEMPTS = 300;              // Extended attempts
const int MAX_AUTO_RESPONSES_PER_SESSION = 5;            // More responses allowed

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
BLECharacteristic* pChatIdCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// ============================================
// FORWARD DECLARATIONS
// ============================================
// System setup
void setupWatchdog();
void feedWatchdog();
bool initCamera();
void setupTelegram();

// Configuration management
void loadDefaultConfig();
void loadConfig();
void saveConfig();
bool validateConfiguration();
void showStoredConfig();

// BLE functions
void initBLE();
void startBLEConfiguration();
void processBLEConfiguration(String configData);
void processTelegramChatId(String chatId);
void updateBLEStatus(String status);
bool testWiFiCredentials(String ssid, String password);
void completeBLEConfiguration();

// FIXED: Enhanced Device ID notification
void notifyDeviceIdGenerated(String deviceId);

// Auto-response system
bool startTelegramAutoResponse();
void stopTelegramAutoResponse();
bool processTelegramAutoResponse();
bool handleAutoResponseMessage(String chat_id, String text, String userName, String userId);

// Auto Telegram workflow
void startPersonalConfigPhase();
void handlePersonalConfigPhase();
void startGroupWaitingPhase();
void handleGroupWaitingPhase();
bool pollForPersonalConfig();
bool pollForGroupDetection();
bool handlePersonalConfigMessage(String chat_id, String text, String userName);
void sendGroupCreationInstructions(String personal_chat_id);

// Camera and messaging
void enableFlash(bool enable);
bool captureAndSendTestPhoto(String chat_id, String context);
bool sendPhotoTelegram(String chat_id, camera_fb_t* fb);
void handlePhotoCapture();
bool safeSendMessage(String chat_id, String message, String keyboard = "");

// Utility functions
String generateDeviceId();
String normalizeTelegramUsername(String username);
bool validateTelegramUsername(String username);
String getMainKeyboard();
String getMenuKeyboard();
void handleSerialCommands();
void printTestSummary();

// State handlers
void handleInit();
void handleBLEConfig();
void handleAutoPersonalConfig();
void handleAutoGroupWait();
void handleAutoOperational();
void handleError();

// FIXED: Enhanced debugging
void printEnhancedStatus();
void logBLESessionInfo();

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

class ChatIdCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String chatId = pCharacteristic->getValue().c_str();
        Serial.println("💬 BLE Chat ID received: " + chatId);
        
        if (chatId.length() > 0) {
            processTelegramChatId(chatId);
        }
    }
};

// ============================================
// SETUP FUNCTION
// ============================================
void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n🚨🚨🚨 SMART WITNESS FIXED INTEGRATED SYSTEM 🚨🚨🚨");
    Serial.println("=== BLE Configuration + Auto Telegram Workflow (FIXED) ===");
    Serial.println("Web Interface: https://arielzubigaray.github.io/smart-witness-config-fixed/");
    Serial.println("Bot: " + String(BOT_NAME));
    Serial.println("🚀 FIXED: Enhanced timeouts and proper Device ID notification!");
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
    
    Serial.println("✅ Smart Witness FIXED Integrated System setup complete!");
    Serial.println("=================================================\n");
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
            
        case AUTO_PERSONAL_CONFIG:
            handleAutoPersonalConfig();
            break;
            
        case AUTO_GROUP_WAIT:
            handleAutoGroupWait();
            break;
            
        case AUTO_OPERATIONAL:
            handleAutoOperational();
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
        Serial.println("✅ Device configured - Starting Auto Telegram workflow");
        currentState = AUTO_PERSONAL_CONFIG;
        startPersonalConfigPhase();
    } else {
        Serial.println("❌ Device not configured - Starting BLE configuration");
        currentState = BLE_CONFIG;
    }
}

void handleBLEConfig() {
    static bool bleInitialized = false;
    
    if (!bleInitialized) {
        Serial.println("🔵 Starting FIXED BLE Configuration Mode");
        Serial.println("📱 Web Interface: https://arielzubigaray.github.io/smart-witness-config-fixed/");
        Serial.println("🚀 ENHANCED: Extended timeouts and better Device ID handling");
        
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
    
    // Handle session timeout (FIXED: Extended)
    if (millis() - bleSession.startTime > BLE_SESSION_TIMEOUT) {
        Serial.println("⏰ BLE session timeout - restarting");
        if (bleSession.autoResponseEnabled) {
            stopTelegramAutoResponse();
        }
        BLEDevice::deinit();
        delay(1000);
        initBLE();
        startBLEConfiguration();
        return;
    }
    
    // Process auto-response if enabled
    if (bleSession.autoResponseEnabled) {
        processTelegramAutoResponse();
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
        Serial.println("✅ BLE configuration complete - transitioning to Auto Telegram workflow");
        if (bleSession.autoResponseEnabled) {
            stopTelegramAutoResponse();
        }
        BLEDevice::deinit();
        digitalWrite(STATUS_LED, LOW);
        currentState = AUTO_PERSONAL_CONFIG;
        startPersonalConfigPhase();
        return;
    }
    
    // FIXED: Enhanced debug logging every 30 seconds
    static unsigned long lastDebugLog = 0;
    if (millis() - lastDebugLog > 30000) {
        lastDebugLog = millis();
        logBLESessionInfo();
    }
}

void handleAutoPersonalConfig() {
    // Check timeout (FIXED: Extended)
    if (millis() - autoSession.phaseStartTime > PERSONAL_CONFIG_TIMEOUT) {
        Serial.println("\n⏰ PERSONAL CONFIG TIMEOUT!");
        Serial.println("❌ Failed to configure personal chat");
        printTestSummary();
        currentState = ERROR;
        return;
    }
    
    // Poll for personal config response
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll >= 2000) { // Poll every 2 seconds
        lastPoll = millis();
        
        if (pollForPersonalConfig()) {
            // Personal config successful, move to group phase
            currentState = AUTO_GROUP_WAIT;
            startGroupWaitingPhase();
        }
    }
}

void handleAutoGroupWait() {
    // Check timeout
    if (millis() - autoSession.phaseStartTime > GROUP_WAIT_TIMEOUT) {
        Serial.println("\n⏰ GROUP WAITING TIMEOUT!");
        Serial.println("❌ No group detected - transitioning to operational mode with personal chat only");
        currentState = AUTO_OPERATIONAL;
        return;
    }
    
    // Poll for group detection
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll >= 2000) { // Poll every 2 seconds
        lastPoll = millis();
        
        if (pollForGroupDetection()) {
            // Group detected, send confirmation photo and transition
            delay(2000);
            if (captureAndSendTestPhoto(autoSession.groupChatId, "group confirmation")) {
                currentState = AUTO_OPERATIONAL;
                printTestSummary();
            }
        }
    }
}

void handleAutoOperational() {
    // Basic operational mode - handle Telegram messages and system health
    static unsigned long lastHealthCheck = 0;
    static unsigned long lastTelegramCheck = 0;
    
    // Check Telegram messages
    if (millis() - lastTelegramCheck > 5000) {
        if (WiFi.status() == WL_CONNECTED && bot) {
            int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
            if (numNewMessages > 0) {
                // Handle basic commands here if needed
                Serial.printf("📨 Received %d messages in operational mode\n", numNewMessages);
            }
        }
        lastTelegramCheck = millis();
    }
    
    // System health check
    if (millis() - lastHealthCheck > 60000) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("❌ WiFi disconnected in operational mode");
            // Try to reconnect
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
// BLE IMPLEMENTATION
// ============================================
void initBLE() {
    Serial.println("🔵 Initializing FIXED BLE Configuration Server");
    
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
    
    pChatIdCharacteristic = pService->createCharacteristic(
                                CHAR_CHAT_ID_UUID,
                                BLECharacteristic::PROPERTY_WRITE
                            );
    pChatIdCharacteristic->setCallbacks(new ChatIdCharacteristicCallbacks());
    
    pService->start();
    
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0);
    BLEDevice::startAdvertising();
    
    Serial.println("✅ FIXED BLE Server started and advertising");
    Serial.println("🔍 Device name: " + deviceName);
}

void startBLEConfiguration() {
    Serial.println("🔵 Starting FIXED BLE Configuration Session");
    
    // Reset session
    bleSession = {"", "", "", false, false, false, false, millis(),
                  false, false, 0, 0, 0, false, 0};
    
    updateBLEStatus("ready_for_config");
}

void processBLEConfiguration(String configData) {
    Serial.println("📝 Processing FIXED BLE configuration...");
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
    
    // Store configuration temporarily
    strncpy(config.wifiSSID, ssid.c_str(), sizeof(config.wifiSSID) - 1);
    strncpy(config.wifiPassword, password.c_str(), sizeof(config.wifiPassword) - 1);
    strncpy(config.telegramUser, normalizedUser.c_str(), sizeof(config.telegramUser) - 1);
    
    // Generate device name
    String deviceName = "SmartWitness_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    strncpy(config.deviceName, deviceName.c_str(), sizeof(config.deviceName) - 1);
    
    bleSession.expectedTelegramUser = normalizedUser;
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
        
        // Generate Device ID for Telegram auto-config
        String deviceId = generateDeviceId();
        bleSession.deviceId = deviceId;
        
        updateBLEStatus("device_id_generated");
        Serial.println("📱 Device ID generated: " + deviceId);
        
        // FIXED: Enhanced Device ID notification
        notifyDeviceIdGenerated(deviceId);
        
        // Start auto-response system
        delay(3000); // FIXED: Give more time for client to receive Device ID
        
        if (startTelegramAutoResponse()) {
            updateBLEStatus("auto_response_started");
            Serial.println("🤖 FIXED Auto-response system activated!");
        } else {
            updateBLEStatus("error_auto_response_failed");
            Serial.println("❌ Failed to start auto-response");
        }
        
    } else {
        Serial.println("❌ WiFi credentials test failed");
        updateBLEStatus("error_wifi_failed");
    }
}

// FIXED: Enhanced Device ID notification function
void notifyDeviceIdGenerated(String deviceId) {
    if (pDeviceIdCharacteristic != nullptr) {
        pDeviceIdCharacteristic->setValue(deviceId.c_str());
        
        if (deviceConnected) {
            // Send notification multiple times to ensure delivery
            for (int i = 0; i < 3; i++) {
                pDeviceIdCharacteristic->notify();
                delay(500);
            }
        }
        
        Serial.println("📡 FIXED: Device ID notification sent to BLE client");
        Serial.println("📱 Device ID: " + deviceId);
        Serial.println("🔗 Telegram Link: https://t.me/" + String(BOT_NAME).substring(1) + "?start=" + deviceId);
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

void processTelegramChatId(String chatId) {
    Serial.println("💬 Processing Telegram Chat ID: " + chatId);
    
    if (chatId.length() > 0) {
        strncpy(config.personalChatId, chatId.c_str(), sizeof(config.personalChatId) - 1);
        bleSession.tempChatId = chatId;
        bleSession.telegramConfigured = true;
        
        updateBLEStatus("telegram_configured");
        
        delay(1000);
        if (captureAndSendTestPhoto(chatId, "BLE Auto-Config")) {
            updateBLEStatus("test_photo_sent");
            completeBLEConfiguration();
        } else {
            updateBLEStatus("error_photo_failed");
            completeBLEConfiguration();
        }
    }
}

void completeBLEConfiguration() {
    Serial.println("✅ Completing FIXED BLE configuration...");
    
    // Stop auto-response if still active
    if (bleSession.autoResponseEnabled) {
        stopTelegramAutoResponse();
    }
    
    config.isConfigured = true;
    config.configVersion = 1;
    bleSession.isComplete = true;
    saveConfig();
    updateBLEStatus("configuration_complete");
    
    if (strlen(config.personalChatId) > 0) {
        String completeMsg = "🎉 Smart Witness Configurado Exitosamente!\n\n";
        completeMsg += "📱 Dispositivo: " + String(config.deviceName) + "\n";
        completeMsg += "👤 Usuario: " + String(config.telegramUser) + "\n";
        completeMsg += "🌐 WiFi: " + String(config.wifiSSID) + "\n";
        completeMsg += "💬 Chat Personal ID: " + String(config.personalChatId) + "\n\n";
        completeMsg += "🚨 El dispositivo está listo para operar!\n";
        completeMsg += "📷 Cada activación capturará fotos automáticamente.";
        
        safeSendMessage(config.personalChatId, completeMsg, getMenuKeyboard());
    }
    
    BLEDevice::getAdvertising()->stop();
    Serial.println("🔄 FIXED BLE configuration complete - ready for Auto Telegram workflow");
}

void updateBLEStatus(String status) {
    if (pStatusCharacteristic != nullptr) {
        pStatusCharacteristic->setValue(status.c_str());
        if (deviceConnected) {
            pStatusCharacteristic->notify();
        }
    }
    Serial.println("📊 FIXED Status: " + status);
}

// ============================================
// AUTO-RESPONSE SYSTEM (FIXED with extended timeouts)
// ============================================
bool startTelegramAutoResponse() {
    if (!bot || WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ Cannot start auto-response - no WiFi or bot");
        updateBLEStatus("error_no_wifi_for_auto_response");
        return false;
    }
    
    if (bleSession.deviceId.length() == 0) {
        Serial.println("❌ Cannot start auto-response - no Device ID");
        updateBLEStatus("error_no_device_id");
        return false;
    }
    
    // Reset auto-response state (FIXED: Extended timeouts)
    bleSession.autoResponseEnabled = true;
    bleSession.isPollingActive = true;
    bleSession.lastPollTime = millis();
    bleSession.pollAttempts = 0;
    bleSession.autoResponsesSent = 0;
    bleSession.waitingForTelegramUser = true;
    bleSession.autoResponseTimeout = millis() + AUTO_RESPONSE_TIMEOUT;
    
    // Clear any pending messages to start fresh
    bot->last_message_received = 0;
    int initialMessages = bot->getUpdates(0);
    
    Serial.println("🚀 ===== FIXED TELEGRAM AUTO-RESPONSE STARTED =====");
    Serial.printf("📱 Device ID: %s\n", bleSession.deviceId.c_str());
    Serial.printf("👤 Expected User: %s\n", bleSession.expectedTelegramUser.c_str());
    Serial.printf("📊 Cleared %d initial messages\n", initialMessages);
    Serial.printf("⏰ FIXED Timeout in: %lu seconds\n", AUTO_RESPONSE_TIMEOUT / 1000);
    Serial.printf("🔍 Listening for: '/start %s'\n", bleSession.deviceId.c_str());
    Serial.println("================================================");
    
    updateBLEStatus("auto_response_active");
    return true;
}

void stopTelegramAutoResponse() {
    if (!bleSession.autoResponseEnabled) return;
    
    bleSession.autoResponseEnabled = false;
    bleSession.isPollingActive = false;
    bleSession.waitingForTelegramUser = false;
    
    Serial.println("🛑 ===== FIXED TELEGRAM AUTO-RESPONSE STOPPED =====");
    Serial.printf("📊 Session Summary:\n");
    Serial.printf("   Poll Attempts: %d\n", bleSession.pollAttempts);
    Serial.printf("   Auto-responses Sent: %d\n", bleSession.autoResponsesSent);
    Serial.printf("   Duration: %lu seconds\n", (millis() - bleSession.lastPollTime) / 1000);
    Serial.println("==============================================");
    
    updateBLEStatus("auto_response_stopped");
}

bool processTelegramAutoResponse() {
    // Check if auto-response is enabled
    if (!bleSession.autoResponseEnabled || !bleSession.isPollingActive) {
        return false;
    }
    
    // Check timeout (FIXED: Extended)
    if (millis() > bleSession.autoResponseTimeout) {
        Serial.println("⏰ FIXED Auto-response timeout reached");
        stopTelegramAutoResponse();
        updateBLEStatus("auto_response_timeout");
        return false;
    }
    
    // Check max attempts (FIXED: Extended)
    if (bleSession.pollAttempts >= MAX_AUTO_RESPONSE_ATTEMPTS) {
        Serial.println("🔄 FIXED Max auto-response attempts reached");
        stopTelegramAutoResponse();
        updateBLEStatus("auto_response_max_attempts");
        return false;
    }
    
    // Check poll interval
    if (millis() - bleSession.lastPollTime < AUTO_RESPONSE_POLL_INTERVAL) {
        return false;
    }
    
    // Update poll timing
    bleSession.lastPollTime = millis();
    bleSession.pollAttempts++;
    
    // Log every 15 attempts to reduce spam (FIXED)
    if (bleSession.pollAttempts % 15 == 0) {
        Serial.printf("🔍 FIXED Auto-response polling: attempt %d/%d (waiting %lu more seconds)\n", 
                      bleSession.pollAttempts, MAX_AUTO_RESPONSE_ATTEMPTS,
                      (bleSession.autoResponseTimeout - millis()) / 1000);
    }
    
    // Get new messages
    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    
    if (numNewMessages < 0) {
        // Polling failed
        if (bleSession.pollAttempts % 10 == 0) {
            Serial.println("❌ Auto-response polling failed, continuing...");
        }
        return false;
    }
    
    if (numNewMessages == 0) {
        return false; // No new messages
    }
    
    Serial.printf("📨 FIXED Auto-response: received %d new messages\n", numNewMessages);
    
    // Process messages looking for our device ID
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = bot->messages[i].chat_id;
        String text = bot->messages[i].text;
        String userName = bot->messages[i].from_name;
        String userId = String(bot->messages[i].from_id);
        
        Serial.printf("📝 Message %d: '%s' from %s (chat: %s)\n", 
                      i+1, text.c_str(), userName.c_str(), chat_id.c_str());
        
        // Check if this is our auto-config command
        if (handleAutoResponseMessage(chat_id, text, userName, userId)) {
            // Successfully handled auto-config
            stopTelegramAutoResponse();
            return true;
        }
    }
    
    feedWatchdog();
    return false;
}

bool handleAutoResponseMessage(String chat_id, String text, String userName, String userId) {
    // Check for our device ID in start command
    String expectedCommand = "/start " + bleSession.deviceId;
    
    if (!text.startsWith("/start " + bleSession.deviceId)) {
        return false; // Not our command
    }
    
    Serial.println("🎯 ===== FIXED AUTO-CONFIG COMMAND DETECTED =====");
    Serial.printf("✅ Device ID match: %s\n", bleSession.deviceId.c_str());
    Serial.printf("👤 User: %s (ID: %s)\n", userName.c_str(), userId.c_str());
    Serial.printf("💬 Chat ID: %s\n", chat_id.c_str());
    
    // Prevent spam (FIXED: More responses allowed)
    if (bleSession.autoResponsesSent >= MAX_AUTO_RESPONSES_PER_SESSION) {
        Serial.println("⚠️ Max auto-responses reached for this session");
        return false;
    }
    
    // Send auto-response immediately
    Serial.println("📤 Sending FIXED auto-response...");
    
    String autoResponse = "🤖 Smart Witness Auto-Configuration (FIXED)\n\n";
    autoResponse += "✅ Device ID recognized: " + bleSession.deviceId + "\n";
    autoResponse += "📱 Device: " + String(config.deviceName) + "\n";
    autoResponse += "👤 Configuring for: " + userName + "\n";
    autoResponse += "💬 Chat ID: " + chat_id + "\n\n";
    autoResponse += "🔄 Setting up your device...\n";
    autoResponse += "📷 Test photo will be sent shortly!";
    
    bool responseSent = safeSendMessage(chat_id, autoResponse);
    
    if (responseSent) {
        bleSession.autoResponsesSent++;
        Serial.println("✅ FIXED Auto-response sent successfully!");
        
        // Store chat ID and complete configuration
        bleSession.tempChatId = chat_id;
        bleSession.telegramConfigured = true;
        
        // Send via BLE to web interface
        if (pChatIdCharacteristic != nullptr) {
            pChatIdCharacteristic->setValue(chat_id.c_str());
        }
        
        // Process the configuration
        processTelegramChatId(chat_id);
        
        Serial.println("🎉 FIXED Auto-configuration completed successfully!");
        return true;
        
    } else {
        Serial.println("❌ Failed to send auto-response");
        updateBLEStatus("auto_response_send_failed");
        return false;
    }
}

// ============================================
// AUTO TELEGRAM WORKFLOW (unchanged from original)
// ============================================
void startPersonalConfigPhase() {
    Serial.println("🚀 ===== PHASE 1: PERSONAL CONFIGURATION =====");
    autoSession.currentPhase = AUTO_PHASE_PERSONAL_CONFIG;
    autoSession.phaseStartTime = millis();
    autoSession.pollAttempts = 0;
    
    // Use stored device name and generate new device ID for this session
    String deviceId = generateDeviceId();
    
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
    
    // Store device ID for this session
    autoSession.personalChatId = ""; // Will be filled when user responds
}

void startGroupWaitingPhase() {
    Serial.println("\n🚀 ===== PHASE 2: WAITING FOR GROUP CREATION =====");
    autoSession.currentPhase = AUTO_PHASE_WAITING_FOR_GROUP;
    autoSession.phaseStartTime = millis();
    autoSession.pollAttempts = 0;
    
    // Send group creation instructions
    sendGroupCreationInstructions(autoSession.personalChatId);
    
    Serial.println("⏰ Waiting for group creation (timeout: " + String(GROUP_WAIT_TIMEOUT/1000) + " seconds)");
    Serial.println("🔍 Monitoring for bot addition to new group...");
    Serial.println();
}

bool pollForPersonalConfig() {
    autoSession.pollAttempts++;
    
    unsigned long remainingTime = (PERSONAL_CONFIG_TIMEOUT - (millis() - autoSession.phaseStartTime)) / 1000;
    
    Serial.printf("🔍 Personal config poll %d (timeout in %lu sec)...\n", autoSession.pollAttempts, remainingTime);
    
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

bool pollForGroupDetection() {
    autoSession.pollAttempts++;
    
    unsigned long remainingTime = (GROUP_WAIT_TIMEOUT - (millis() - autoSession.phaseStartTime)) / 1000;
    
    Serial.printf("🔍 Group detection poll %d (timeout in %lu sec)...\n", autoSession.pollAttempts, remainingTime);
    
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
                chat_id != autoSession.personalChatId) {  // Any message from a different chat
                
                Serial.println("\n🎯 ===== GROUP ADDITION DETECTED =====");
                Serial.println("✅ Bot was added to group!");
                Serial.println("💬 Group Chat ID: " + chat_id);
                Serial.println("📱 Detected via: Negative Chat ID");
                
                autoSession.groupChatId = chat_id;
                strncpy(config.groupChatId, chat_id.c_str(), sizeof(config.groupChatId) - 1);
                saveConfig(); // Save group chat ID
                
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
                
                safeSendMessage(autoSession.personalChatId, personalUpdateMsg);
                
                Serial.println("🎉 GROUP CONFIGURATION COMPLETED!");
                Serial.println("===============================\n");
                
                return true; // Group detection completed
            }
        }
    }
    
    return false; // Continue polling
}

bool handlePersonalConfigMessage(String chat_id, String text, String userName) {
    // Check if message starts with our device ID (we need to generate and check dynamically)
    // For now, check if it's a /start command and user matches expected user
    if (!text.startsWith("/start ")) {
        Serial.println("    ⏭️ Not a start command");
        return false;
    }
    
    // Validate user
    String normalizedUserName = normalizeTelegramUsername(userName);
    String expectedUser = String(config.telegramUser);
    
    // For basic validation, check if the user matches (you might want more sophisticated matching)
    if (!normalizedUserName.equals(expectedUser)) {
        Serial.println("    ⏭️ User mismatch: " + normalizedUserName + " vs " + expectedUser);
        return false;
    }
    
    Serial.println("\n🎯 ===== PERSONAL CONFIG MESSAGE DETECTED =====");
    Serial.println("✅ User match: " + userName);
    Serial.println("💬 Personal Chat ID: " + chat_id);
    
    // Save personal chat ID
    autoSession.personalChatId = chat_id;
    strncpy(config.personalChatId, chat_id.c_str(), sizeof(config.personalChatId) - 1);
    saveConfig(); // Save personal chat ID
    
    // Send personal confirmation
    String confirmMsg = "✅ Smart Witness - Configuración Personal Completada!\n\n";
    confirmMsg += "📱 Dispositivo: " + String(config.deviceName) + "\n";
    confirmMsg += "👤 Usuario: " + String(config.telegramUser) + "\n";
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

// ============================================
// CAMERA FUNCTIONS (unchanged)
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
        String boundary = "SmartWitnessFixed" + String(millis());
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
// TELEGRAM FUNCTIONS (unchanged)
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

String getMainKeyboard() {
    String keyboard = "[[";
    keyboard += "{\"text\":\"📷 Photo\",\"callback_data\":\"/photo\"},";
    keyboard += "{\"text\":\"📋 Menu\",\"callback_data\":\"/menu\"}]]";
    return keyboard;
}

String getMenuKeyboard() {
    String keyboard = "[[";
    keyboard += "{\"text\":\"📷 Photo\",\"callback_data\":\"/photo\"},";
    keyboard += "{\"text\":\"📊 Estado\",\"callback_data\":\"/status\"}],";
    keyboard += "[{\"text\":\"🔄 Reiniciar\",\"callback_data\":\"/restart\"}]]";
    return keyboard;
}

// ============================================
// CONFIGURATION FUNCTIONS (unchanged)
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
    
    Serial.printf("🔍 FIXED Configuration validation:\n");
    Serial.printf("   WiFi: %s\n", hasWiFi ? "✅" : "❌");
    Serial.printf("   Telegram User: %s\n", hasTelegramUser ? "✅" : "❌");
    Serial.printf("   Device Name: %s\n", hasDeviceName ? "✅" : "❌");
    Serial.printf("   Is Configured Flag: %s\n", config.isConfigured ? "✅" : "❌");
    
    return hasWiFi && hasTelegramUser && hasDeviceName && config.isConfigured;
}

void showStoredConfig() {
    Serial.println("\n=== Smart Witness FIXED Configuration ===");
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
// UTILITY FUNCTIONS (unchanged)
// ============================================
String generateDeviceId() {
    uint64_t mac = ESP.getEfuseMac();
    String id = "DEVICE_FIXED_";
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
    Serial.println("\n📊 ===== FIXED INTEGRATED SYSTEM SUMMARY =====");
    Serial.println("Device Name: " + String(config.deviceName));
    Serial.println("Current State: " + String(currentState));
    Serial.println("Auto Phase: " + String(autoSession.currentPhase));
    Serial.println("Personal Chat ID: " + autoSession.personalChatId);
    Serial.println("Group Chat ID: " + autoSession.groupChatId);
    Serial.println("Total Poll Attempts: " + String(autoSession.pollAttempts));
    
    if (currentState == AUTO_OPERATIONAL) {
        Serial.println("\n🎉 FIXED INTEGRATED WORKFLOW COMPLETED SUCCESSFULLY!");
        Serial.println("✅ BLE Configuration: COMPLETE");
        Serial.println("✅ Personal chat: CONFIGURED");
        Serial.println("✅ Auto Telegram workflow: COMPLETE");
        if (autoSession.groupChatId.length() > 0) {
            Serial.println("✅ Group detection: SUCCESS");
            Serial.println("✅ Group photo: SENT");
            Serial.println("🎯 Photos will be sent to group: " + autoSession.groupChatId);
        } else {
            Serial.println("⚠️ Group detection: TIMEOUT");
            Serial.println("🎯 Photos will be sent to personal: " + autoSession.personalChatId);
        }
        Serial.println("💾 Configuration saved to EEPROM");
        Serial.println("🔄 System ready for operational use!");
    } else {
        Serial.println("\n❌ FIXED INTEGRATED WORKFLOW INCOMPLETE");
        Serial.println("🔍 Check logs for specific failure point");
    }
    
    Serial.println("==========================================\n");
}

// ============================================
// FIXED: Enhanced debugging functions
// ============================================
void printEnhancedStatus() {
    Serial.println("\n📊 ===== FIXED ENHANCED STATUS =====");
    Serial.println("Uptime: " + String(millis()/1000) + " seconds");
    Serial.println("Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("Current State: " + String(currentState));
    Serial.println("WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi SSID: " + WiFi.SSID());
        Serial.println("WiFi IP: " + WiFi.localIP().toString());
        Serial.println("WiFi RSSI: " + String(WiFi.RSSI()) + " dBm");
    }
    Serial.println("================================\n");
}

void logBLESessionInfo() {
    Serial.println("🔵 ===== BLE SESSION INFO =====");
    Serial.printf("Connected: %s\n", deviceConnected ? "YES" : "NO");
    Serial.printf("Session Duration: %lu seconds\n", (millis() - bleSession.startTime) / 1000);
    Serial.printf("Config Received: %s\n", bleSession.configReceived ? "YES" : "NO");
    Serial.printf("WiFi Tested: %s\n", bleSession.wifiTested ? "YES" : "NO");
    Serial.printf("Device ID: %s\n", bleSession.deviceId.c_str());
    Serial.printf("Expected User: %s\n", bleSession.expectedTelegramUser.c_str());
    Serial.printf("Auto-response Active: %s\n", bleSession.autoResponseEnabled ? "YES" : "NO");
    if (bleSession.autoResponseEnabled) {
        Serial.printf("Poll Attempts: %d/%d\n", bleSession.pollAttempts, MAX_AUTO_RESPONSE_ATTEMPTS);
        Serial.printf("Responses Sent: %d/%d\n", bleSession.autoResponsesSent, MAX_AUTO_RESPONSES_PER_SESSION);
        Serial.printf("Timeout in: %lu seconds\n", (bleSession.autoResponseTimeout - millis()) / 1000);
    }
    Serial.println("==============================\n");
}

// ============================================
// FIXED: Enhanced serial commands
// ============================================
void handleSerialCommands() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        if (cmd == "reset") {
            ESP.restart();
        }
        else if (cmd == "status") {
            printEnhancedStatus();
        }
        else if (cmd == "config") {
            showStoredConfig();
        }
        else if (cmd == "photo") {
            if (strlen(config.personalChatId) > 0) {
                captureAndSendTestPhoto(config.personalChatId, "manual test");
            } else {
                Serial.println("❌ No personal chat ID configured");
            }
        }
        else if (cmd == "ble") {
            Serial.println("🚀 Forcing FIXED BLE configuration mode");
            if (bleSession.autoResponseEnabled) {
                stopTelegramAutoResponse();
            }
            if (currentState == BLE_CONFIG) {
                BLEDevice::deinit();
            }
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
        else if (cmd == "session") {
            logBLESessionInfo();
        }
        else if (cmd == "deviceid") {
            if (bleSession.deviceId.length() > 0) {
                Serial.println("📱 Current Device ID: " + bleSession.deviceId);
                Serial.println("🔗 Telegram Link: https://t.me/" + String(BOT_NAME).substring(1) + "?start=" + bleSession.deviceId);
            } else {
                Serial.println("❌ No Device ID generated yet");
            }
        }
        else if (cmd == "wifi") {
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("✅ WiFi Connected");
                Serial.println("SSID: " + WiFi.SSID());
                Serial.println("IP: " + WiFi.localIP().toString());
                Serial.println("RSSI: " + String(WiFi.RSSI()) + " dBm");
            } else {
                Serial.println("❌ WiFi Disconnected");
                Serial.println("Stored SSID: " + String(config.wifiSSID));
            }
        }
        else if (cmd == "help") {
            Serial.println("\n🆘 ===== FIXED COMMAND HELP =====");
            Serial.println("status    - Show enhanced system status");
            Serial.println("config    - Show stored configuration");
            Serial.println("session   - Show BLE session details");
            Serial.println("deviceid  - Show current Device ID and link");
            Serial.println("wifi      - Show WiFi connection status");
            Serial.println("photo     - Take manual test photo");
            Serial.println("ble       - Force BLE configuration mode");
            Serial.println("summary   - Show workflow summary");
            Serial.println("resetconfig - Reset to defaults and restart");
            Serial.println("reset     - Restart ESP32");
            Serial.println("help      - Show this help");
            Serial.println("===============================\n");
        }
    }
}