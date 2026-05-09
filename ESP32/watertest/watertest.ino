#define BLINKER_WIFI

#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <Blinker.h>
#include <U8g2lib.h>
#include <Adafruit_MAX31855.h>

// ===================== 可按需修改的引脚与参数 =====================
// MAX31855 (SPI, 只需要 SCK / CS / DO)
const int PIN_MAX31855_SCK = 13;
const int PIN_MAX31855_CS = 12;
const int PIN_MAX31855_DO = 14;

// 漏水检测引脚
const int PIN_WATER_LEAK = 25;
const int WATER_ACTIVE_LEVEL = LOW; // 传感器触发电平, 常见为 LOW

// 蜂鸣器引脚
const int PIN_BUZZER = 2;
const int BUZZER_PWM_CHANNEL = 0;
const int BUZZER_PWM_RESOLUTION = 8;
const int BUZZER_TONE_FREQ = 2200;
const unsigned long BUZZER_ON_MS = 200;
const unsigned long BUZZER_OFF_MS = 800;

// OLED 128x64 I2C
const int PIN_OLED_SDA = 33;
const int PIN_OLED_SCL = 32;
const uint8_t OLED_I2C_ADDR = 0x3C; // 常见 0x3C / 0x3D

// 刷新周期 (ms)
const unsigned long SAMPLE_INTERVAL_MS = 500;
const unsigned long BLINKER_UPLOAD_INTERVAL_MS = 1000;

// 热端温度拟合：根据“显示60°C时实际约23°C、显示230°C时实际约90°C”做线性拟合
const double HOT_TEMP_FIT_SLOPE = (90.0 - 23.0) / (230.0 - 60.0);
const double HOT_TEMP_FIT_INTERCEPT = 23.0 - HOT_TEMP_FIT_SLOPE * 60.0;

// NVS/Preferences 配置
const uint32_t EEPROM_MAGIC = 0x57415452;
const float DEFAULT_TEMP_THRESHOLD_C = 60.0f;

const char DEFAULT_BLINKER_AUTH[] = "156e262d8e93";
const char DEFAULT_WIFI_SSID[] = "Pura70";
const char DEFAULT_WIFI_PSWD[] = "12345678";

const size_t BLINKER_AUTH_MAX_LEN = 64;
const size_t WIFI_SSID_MAX_LEN = 32;
const size_t WIFI_PSWD_MAX_LEN = 64;
const uint32_t CONFIG_VERSION = 1;

char blinkerAuth[BLINKER_AUTH_MAX_LEN + 1] = "";
char wifiSsid[WIFI_SSID_MAX_LEN + 1] = "";
char wifiPswd[WIFI_PSWD_MAX_LEN + 1] = "";

char NUM_TEM_KEY[] = "num-tem";
char NUM_HHP_KEY[] = "num-hhp";
char NUM_THR_KEY[] = "ran-bdm";
Preferences devicePrefs;

// ===================== 对象定义 =====================
Adafruit_MAX31855 thermocouple(PIN_MAX31855_SCK, PIN_MAX31855_CS, PIN_MAX31855_DO);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, PIN_OLED_SCL, PIN_OLED_SDA);
BlinkerNumber NumberTem(NUM_TEM_KEY);
BlinkerNumber NumberHhp(NUM_HHP_KEY);
BlinkerNumber NumberThreshold(NUM_THR_KEY);

unsigned long lastSampleMs = 0;
unsigned long lastBlinkerUploadMs = 0;
double lastTcTempC = NAN;
double lastCjTempC = NAN;
bool lastLeak = false;
bool lastTcOk = false;
float tempThresholdC = DEFAULT_TEMP_THRESHOLD_C;
bool lastTempHigh = false;
bool lastLeakAlert = false;
bool buzzerSilenced = false;
bool buzzerOutputEnabled = false;
unsigned long lastBuzzerToggleMs = 0;
bool buzzerPhaseOn = false;

struct DeviceConfig
{
    uint32_t magic;
    uint32_t version;
    float tempThresholdC;
    char blinkerAuth[BLINKER_AUTH_MAX_LEN + 1];
    char wifiSsid[WIFI_SSID_MAX_LEN + 1];
    char wifiPswd[WIFI_PSWD_MAX_LEN + 1];
    uint32_t checksum;
};

void copyCString(char *dest, size_t destSize, const char *src)
{
    if (destSize == 0)
    {
        return;
    }

    size_t i = 0;
    for (; i + 1 < destSize && src[i] != '\0'; ++i)
    {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

bool hasTerminatorWithin(const char *text, size_t maxLen)
{
    for (size_t i = 0; i < maxLen; ++i)
    {
        if (text[i] == '\0')
        {
            return true;
        }
    }
    return false;
}

uint32_t hashBytes(uint32_t seed, const uint8_t *data, size_t length)
{
    uint32_t hash = seed;
    for (size_t i = 0; i < length; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

uint32_t computeConfigChecksum(const DeviceConfig &config)
{
    uint32_t hash = 2166136261u;
    hash = hashBytes(hash, (const uint8_t *)&config.magic, sizeof(config.magic));
    hash = hashBytes(hash, (const uint8_t *)&config.version, sizeof(config.version));
    hash = hashBytes(hash, (const uint8_t *)&config.tempThresholdC, sizeof(config.tempThresholdC));
    hash = hashBytes(hash, (const uint8_t *)config.blinkerAuth, sizeof(config.blinkerAuth));
    hash = hashBytes(hash, (const uint8_t *)config.wifiSsid, sizeof(config.wifiSsid));
    hash = hashBytes(hash, (const uint8_t *)config.wifiPswd, sizeof(config.wifiPswd));
    return hash;
}

bool isSameConfig(const DeviceConfig &left, const DeviceConfig &right)
{
    return left.magic == right.magic &&
           left.version == right.version &&
           left.tempThresholdC == right.tempThresholdC &&
           left.checksum == right.checksum &&
           memcmp(left.blinkerAuth, right.blinkerAuth, sizeof(left.blinkerAuth)) == 0 &&
           memcmp(left.wifiSsid, right.wifiSsid, sizeof(left.wifiSsid)) == 0 &&
           memcmp(left.wifiPswd, right.wifiPswd, sizeof(left.wifiPswd)) == 0;
}

void dumpConfig(const char *label, const DeviceConfig &config)
{
    Serial.println(label);
    Serial.print("  magic=0x");
    Serial.println(config.magic, HEX);
    Serial.print("  version=");
    Serial.println(config.version);
    Serial.print("  threshold=");
    Serial.println(config.tempThresholdC, 2);
    Serial.print("  ssid=");
    Serial.println(config.wifiSsid);
    Serial.print("  auth=");
    Serial.println(config.blinkerAuth);
    Serial.print("  checksum=0x");
    Serial.println(config.checksum, HEX);
}

double getTcDisplayTemp(double tcTempC)
{
    return tcTempC * HOT_TEMP_FIT_SLOPE + HOT_TEMP_FIT_INTERCEPT;
}

bool isWaterLeak()
{
    return digitalRead(PIN_WATER_LEAK) == WATER_ACTIVE_LEVEL;
}

void drawCenteredText(const String &text, int16_t y)
{
    int16_t width = display.getUTF8Width(text.c_str());
    int16_t x = (128 - width) / 2;
    display.drawUTF8(x, y, text.c_str());
}

bool saveConfig()
{
    DeviceConfig config = {};
    config.magic = EEPROM_MAGIC;
    config.version = CONFIG_VERSION;
    config.tempThresholdC = tempThresholdC;
    copyCString(config.blinkerAuth, sizeof(config.blinkerAuth), blinkerAuth);
    copyCString(config.wifiSsid, sizeof(config.wifiSsid), wifiSsid);
    copyCString(config.wifiPswd, sizeof(config.wifiPswd), wifiPswd);
    config.checksum = computeConfigChecksum(config);

    bool writeOk = devicePrefs.putBytes("cfg", &config, sizeof(config)) == sizeof(config);

    if (!writeOk)
    {
        Serial.println("NVS write failed.");
        return false;
    }

    DeviceConfig verifyConfig = {};
    size_t readBytes = devicePrefs.getBytes("cfg", &verifyConfig, sizeof(verifyConfig));

    bool verifyOk = readBytes == sizeof(verifyConfig) &&
                    isSameConfig(config, verifyConfig) &&
                    verifyConfig.checksum == computeConfigChecksum(verifyConfig) &&
                    verifyConfig.magic == EEPROM_MAGIC &&
                    verifyConfig.version == CONFIG_VERSION;

    if (verifyOk)
    {
        Serial.println("Config saved to NVS and verified by readback.");
    }
    else
    {
        Serial.println("NVS verify failed after save.");
        Serial.print("Written threshold: ");
        Serial.println(config.tempThresholdC, 2);
        Serial.print("Readback threshold: ");
        Serial.println(verifyConfig.tempThresholdC, 2);
        Serial.print("Written WiFi SSID: ");
        Serial.println(config.wifiSsid);
        Serial.print("Readback WiFi SSID: ");
        Serial.println(verifyConfig.wifiSsid);
        Serial.print("Written Blinker AUTH: ");
        Serial.println(config.blinkerAuth);
        Serial.print("Readback Blinker AUTH: ");
        Serial.println(verifyConfig.blinkerAuth);
    }

    return verifyOk;
}

void loadConfig()
{
    DeviceConfig config = {};
    size_t readBytes = devicePrefs.getBytes("cfg", &config, sizeof(config));
    dumpConfig("NVS raw read on load:", config);

    if (readBytes == sizeof(config) &&
        config.magic == EEPROM_MAGIC &&
        config.version == CONFIG_VERSION &&
        isfinite(config.tempThresholdC) &&
        config.tempThresholdC > -100.0f &&
        config.tempThresholdC < 200.0f &&
        config.checksum == computeConfigChecksum(config))
    {
        tempThresholdC = config.tempThresholdC;
        copyCString(blinkerAuth, sizeof(blinkerAuth), config.blinkerAuth);
        copyCString(wifiSsid, sizeof(wifiSsid), config.wifiSsid);
        copyCString(wifiPswd, sizeof(wifiPswd), config.wifiPswd);
        Serial.println("Config loaded from NVS.");
    }
    else
    {
        tempThresholdC = DEFAULT_TEMP_THRESHOLD_C;
        copyCString(blinkerAuth, sizeof(blinkerAuth), DEFAULT_BLINKER_AUTH);
        copyCString(wifiSsid, sizeof(wifiSsid), DEFAULT_WIFI_SSID);
        copyCString(wifiPswd, sizeof(wifiPswd), DEFAULT_WIFI_PSWD);
        saveConfig();
        Serial.println("NVS config invalid, loaded defaults and re-saved.");
    }
}

String nextSerialToken(const String &text, int &index)
{
    while (index < text.length() && (text[index] == ' ' || text[index] == '\t'))
    {
        index++;
    }

    if (index >= text.length())
    {
        return String();
    }

    char quote = 0;
    if (text[index] == '"' || text[index] == '\'')
    {
        quote = text[index++];
    }

    String token;
    while (index < text.length())
    {
        char ch = text[index++];
        if (quote != 0)
        {
            if (ch == quote)
            {
                break;
            }
            token += ch;
        }
        else
        {
            if (ch == ' ' || ch == '\t')
            {
                break;
            }
            token += ch;
        }
    }

    return token;
}

String serialTokenLower(const String &text)
{
    String token = text;
    token.toLowerCase();
    return token;
}

void printSerialHelp()
{
    Serial.println();
    Serial.println("=== Serial Commands ===");
    Serial.println("help");
    Serial.println("show");
    Serial.println("cfg <ssid> <password> <blinkerauth>");
    Serial.println("set wifi <ssid> <password>");
    Serial.println("set auth <blinkerauth>");
    Serial.println("set all <ssid> <password> <blinkerauth>");
    Serial.println("Use quotes if a value contains spaces.");
    Serial.println("After changing wifi/auth, the device saves and restarts automatically.");
}

void printSerialConfig()
{
    Serial.println();
    Serial.println("=== Current Config ===");
    Serial.print("WiFi SSID: ");
    Serial.println(wifiSsid);
    Serial.print("WiFi PSWD: ");
    Serial.println(wifiPswd);
    Serial.print("Blinker AUTH: ");
    Serial.println(blinkerAuth);
    Serial.print("Temp Threshold: ");
    Serial.println(tempThresholdC, 2);
}

void restartToApplyNetworkConfig()
{
    Serial.println("Config saved. Restarting to apply new WiFi/Blinker settings...");
    Serial.flush();
    delay(500);
    ESP.restart();
}

void applyNetworkConfig(const String &newSsid, const String &newPassword, const String &newAuth)
{
    copyCString(wifiSsid, sizeof(wifiSsid), newSsid.c_str());
    copyCString(wifiPswd, sizeof(wifiPswd), newPassword.c_str());
    copyCString(blinkerAuth, sizeof(blinkerAuth), newAuth.c_str());
    if (saveConfig())
    {
        printSerialConfig();
        restartToApplyNetworkConfig();
    }
    else
    {
        Serial.println("Config not applied because EEPROM verification failed.");
    }
}

void handleSerialCommand(String line)
{
    line.trim();
    if (line.length() == 0)
    {
        return;
    }

    int index = 0;
    String command = serialTokenLower(nextSerialToken(line, index));

    if (command == "help" || command == "?")
    {
        printSerialHelp();
        return;
    }

    if (command == "show" || command == "status")
    {
        printSerialConfig();
        return;
    }

    if (command == "cfg")
    {
        String ssid = nextSerialToken(line, index);
        String password = nextSerialToken(line, index);
        String auth = nextSerialToken(line, index);

        if (ssid.length() == 0 || password.length() == 0 || auth.length() == 0)
        {
            Serial.println("Usage: cfg <ssid> <password> <blinkerauth>");
            return;
        }

        applyNetworkConfig(ssid, password, auth);
        return;
    }

    if (command == "set")
    {
        String subCommand = serialTokenLower(nextSerialToken(line, index));

        if (subCommand == "wifi")
        {
            String ssid = nextSerialToken(line, index);
            String password = nextSerialToken(line, index);

            if (ssid.length() == 0 || password.length() == 0)
            {
                Serial.println("Usage: set wifi <ssid> <password>");
                return;
            }

            applyNetworkConfig(ssid, password, blinkerAuth);
            return;
        }

        if (subCommand == "auth")
        {
            String auth = nextSerialToken(line, index);

            if (auth.length() == 0)
            {
                Serial.println("Usage: set auth <blinkerauth>");
                return;
            }

            applyNetworkConfig(wifiSsid, wifiPswd, auth);
            return;
        }

        if (subCommand == "all")
        {
            String ssid = nextSerialToken(line, index);
            String password = nextSerialToken(line, index);
            String auth = nextSerialToken(line, index);

            if (ssid.length() == 0 || password.length() == 0 || auth.length() == 0)
            {
                Serial.println("Usage: set all <ssid> <password> <blinkerauth>");
                return;
            }

            applyNetworkConfig(ssid, password, auth);
            return;
        }

        Serial.println("Usage: set wifi <ssid> <password> | set auth <blinkerauth> | set all <ssid> <password> <blinkerauth>");
        return;
    }

    Serial.println("Unknown command. Type help for usage.");
}

void pollSerialCommands()
{
    static char lineBuffer[180];
    static size_t lineLength = 0;

    while (Serial.available() > 0)
    {
        char ch = (char)Serial.read();

        if (ch == '\r')
        {
            continue;
        }

        if (ch == '\n')
        {
            lineBuffer[lineLength] = '\0';
            if (lineLength > 0)
            {
                handleSerialCommand(String(lineBuffer));
            }
            lineLength = 0;
        }
        else if (lineLength < sizeof(lineBuffer) - 1)
        {
            lineBuffer[lineLength++] = ch;
        }
    }
}

void syncThresholdToApp()
{
    NumberThreshold.print(tempThresholdC);
}

void setBuzzerOutput(bool on)
{
    if (on)
    {
        ledcWriteTone(BUZZER_PWM_CHANNEL, BUZZER_TONE_FREQ);
        ledcWrite(BUZZER_PWM_CHANNEL, 128);
    }
    else
    {
        ledcWrite(BUZZER_PWM_CHANNEL, 0);
        ledcWriteTone(BUZZER_PWM_CHANNEL, 0);
    }
    buzzerOutputEnabled = on;
}

void updateBuzzer(bool alarmActive)
{
    if (!alarmActive || buzzerSilenced)
    {
        buzzerPhaseOn = false;
        setBuzzerOutput(false);
        return;
    }

    unsigned long interval = buzzerPhaseOn ? BUZZER_ON_MS : BUZZER_OFF_MS;
    if (lastBuzzerToggleMs == 0 || millis() - lastBuzzerToggleMs >= interval)
    {
        buzzerPhaseOn = !buzzerPhaseOn;
        lastBuzzerToggleMs = millis();
        setBuzzerOutput(buzzerPhaseOn);
    }
}

void handleAppData(const String &data)
{
    Serial.print("APP data: ");
    Serial.println(data);

    int silenceIndex = data.indexOf("btn-bs");
    if (silenceIndex >= 0)
    {
        buzzerSilenced = true;
        setBuzzerOutput(false);
        Serial.println("Alarm silenced by btn-bs");
    }

    int keyIndex = data.indexOf("ran-bdm");
    float newThreshold = tempThresholdC;
    bool parsed = false;

    if (keyIndex >= 0)
    {
        int colonIndex = data.indexOf(':', keyIndex);
        if (colonIndex >= 0)
        {
            int valueStart = colonIndex + 1;
            while (valueStart < data.length() && (data[valueStart] == ' ' || data[valueStart] == '"'))
            {
                valueStart++;
            }

            int valueEnd = valueStart;
            while (valueEnd < data.length())
            {
                char ch = data[valueEnd];
                if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.')
                {
                    valueEnd++;
                }
                else
                {
                    break;
                }
            }

            if (valueEnd > valueStart)
            {
                newThreshold = data.substring(valueStart, valueEnd).toFloat();
                parsed = true;
            }
        }
    }

    if (!parsed)
    {
        char *endPtr = nullptr;
        newThreshold = strtof(data.c_str(), &endPtr);
        parsed = (endPtr != data.c_str());
    }

    if (parsed && isfinite(newThreshold) && newThreshold > -100.0f && newThreshold < 200.0f)
    {
        tempThresholdC = newThreshold;
        if (saveConfig())
        {
            syncThresholdToApp();
            Serial.print("Temp threshold updated: ");
            Serial.println(tempThresholdC, 2);
        }
        else
        {
            Serial.println("Temp threshold not saved because EEPROM verification failed.");
        }
    }
}

void showBootScreen()
{
    display.clearBuffer();
    display.drawFrame(0, 0, 128, 64);
    display.drawHLine(0, 14, 128);
    display.setFont(u8g2_font_helvB12_tr);
    display.setFontPosTop();
    drawCenteredText("WATER", 4);
    drawCenteredText("TEST", 22);
    display.setFont(u8g2_font_6x12_tr);
    display.setFontPosTop();
    drawCenteredText("ESP32 + MAX31855E", 48);
    display.sendBuffer();
}

void showAlertScreen(const String &title, const String &line1, const String &line2)
{
    display.clearBuffer();
    display.drawFrame(0, 0, 128, 64);
    display.drawHLine(0, 14, 128);
    display.setFont(u8g2_font_helvB12_tr);
    display.setFontPosTop();
    drawCenteredText(title, 2);

    display.setFont(u8g2_font_helvB14_tr);
    display.setFontPosTop();
    drawCenteredText(line1, 20);

    display.setFont(u8g2_font_6x12_tr);
    display.setFontPosTop();
    drawCenteredText(line2, 46);
    display.sendBuffer();
}

void showAlertState(bool leak, bool tempHigh)
{
    if (leak)
    {
        showAlertScreen("ALERT", "LEAK DET", "CHECK THE PIPE");
    }
    else if (tempHigh)
    {
        showAlertScreen("ALERT", "TEMP HIGH", "CHECK THE PIPE");
    }
}

void showDataScreen(double tcTempC, double cjTempC, bool leak, bool tcOk)
{
    display.clearBuffer();
    display.drawFrame(0, 0, 128, 64);
    display.drawHLine(0, 14, 128);

    display.setFont(u8g2_font_6x12_tr);
    display.setFontPosTop();
    display.drawStr(4, 3, "THERMOCOUPLE");
    display.drawStr(92, 3, "TH");
    display.setCursor(104, 3);
    display.print((int)tempThresholdC);

    if (tcOk)
    {
        display.setFont(u8g2_font_logisoso24_tn);
        display.setFontPosTop();
        display.drawStr(4, 16, "H");
        display.setCursor(22, 18);
        display.print(getTcDisplayTemp(tcTempC), 1);
        display.print("C");

        display.setFont(u8g2_font_helvB12_tr);
        display.setFontPosTop();
        display.drawStr(4, 44, "C");
        display.setCursor(22, 44);
        display.print(cjTempC, 1);
        display.print("C");
    }
    else
    {
        display.setFont(u8g2_font_helvB14_tr);
        display.setFontPosTop();
        drawCenteredText("SENSOR", 22);
        drawCenteredText("ERROR", 40);
    }

    display.setFont(u8g2_font_6x12_tr);
    display.setFontPosTop();
    display.drawStr(92, 22, "TH");
    display.setCursor(88, 34);
    display.print(tempThresholdC, 0);
    display.print("C");

    display.drawStr(92, 46, "LK");
    display.drawStr(110, 46, leak ? "Y" : "N");

    display.drawStr(4, 56, (tcOk && getTcDisplayTemp(tcTempC) > tempThresholdC) ? "TEMP HIGH" : "OK");
    display.sendBuffer();
}

void uploadBlinkerData()
{
    if (lastTcOk)
    {
        NumberTem.print(getTcDisplayTemp(lastTcTempC));
        NumberHhp.print(lastCjTempC);
    }
    else
    {
        NumberTem.print(0);
        NumberHhp.print(0);
    }

    NumberThreshold.print(tempThresholdC);
    Blinker.print("water_leak", lastLeak ? 1 : 0);
}

void notifyIfNeeded(bool leak, bool tempHigh)
{
    if (tempHigh && !lastTempHigh)
    {
        buzzerSilenced = false;
        Blinker.notify("温度过高，请检查水管");
    }

    if (leak && !lastLeakAlert)
    {
        buzzerSilenced = false;
        Blinker.notify("检测到漏水，请检查水管");
    }

    lastTempHigh = tempHigh;
    lastLeakAlert = leak;
}

void setup()
{
    Serial.begin(115200);

    bool nvsReady = devicePrefs.begin("wtcfg", false);
    Serial.print("Preferences.begin result: ");
    Serial.println(nvsReady ? "OK" : "FAIL");
    loadConfig();
    printSerialHelp();
    printSerialConfig();

    Blinker.begin(blinkerAuth, wifiSsid, wifiPswd);
    Blinker.attachData(handleAppData);

    pinMode(PIN_WATER_LEAK, INPUT_PULLUP);
    pinMode(PIN_BUZZER, OUTPUT);
    ledcSetup(BUZZER_PWM_CHANNEL, 2000, BUZZER_PWM_RESOLUTION);
    ledcAttachPin(PIN_BUZZER, BUZZER_PWM_CHANNEL);
    setBuzzerOutput(false);

    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    display.begin();
    display.setBusClock(400000);
    display.enableUTF8Print();

    showBootScreen();
    delay(1200);

    // 首次读取用于确认 MAX31855 是否正常返回
    double t = thermocouple.readCelsius();
    if (isnan(t))
    {
        Serial.println("MAX31855 read failed. Check wiring/chip type.");
    }
    else
    {
        Serial.print("MAX31855 ready, TC(C): ");
        Serial.println(t);
    }

    syncThresholdToApp();
}

void loop()
{
    pollSerialCommands();
    Blinker.run();

    unsigned long now = millis();
    if (now - lastSampleMs < SAMPLE_INTERVAL_MS)
    {
        if (now - lastBlinkerUploadMs >= BLINKER_UPLOAD_INTERVAL_MS)
        {
            lastBlinkerUploadMs = now;
            uploadBlinkerData();
        }
        return;
    }
    lastSampleMs = now;

    bool leak = isWaterLeak();
    double tcTempC = thermocouple.readCelsius();
    double cjTempC = thermocouple.readInternal();
    bool tcOk = !isnan(tcTempC) && !isnan(cjTempC);

    lastLeak = leak;
    lastTcTempC = tcTempC;
    lastCjTempC = cjTempC;
    lastTcOk = tcOk;

    bool tempHigh = tcOk && getTcDisplayTemp(tcTempC) > tempThresholdC;

    // if (tcOk)
    // {
    //     Serial.print("TC: ");
    //     Serial.print(tcTempC, 2);
    //     Serial.print(" C, CJ: ");
    //     Serial.print(cjTempC, 2);
    //     Serial.print(" C, Leak: ");
    //     Serial.println(leak ? "YES" : "NO");
    // }
    // else
    // {
    //     Serial.print("TC read error, Leak: ");
    //     Serial.println(leak ? "YES" : "NO");
    // }

    if (leak || tempHigh)
    {
        showAlertState(leak, tempHigh);
    }
    else
    {
        showDataScreen(tcTempC, cjTempC, leak, tcOk);
    }
    notifyIfNeeded(leak, tempHigh);
    updateBuzzer(leak || tempHigh);

    if (now - lastBlinkerUploadMs >= BLINKER_UPLOAD_INTERVAL_MS)
    {
        lastBlinkerUploadMs = now;
        uploadBlinkerData();
    }
}
