// Board: ESP32 DevKitC v4 (ESP32 Dev Module, 38-pin, ESP-WROOM-32)
// Use board: ESP32 Dev Module
// Upload settings: Default 4MB, 115200 baud

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSans9pt7b.h>
#include <XPT2046_Touchscreen.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <time.h>
#include <Stepper.h>

// ---------- TFT ----------
#define TFT_CS 5
#define TFT_DC 16
#define TFT_RST 4
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// ---------- TOUCH ----------
#define T_CS 26
#define T_IRQ 27
XPT2046_Touchscreen ts(T_CS, T_IRQ);

// ---------- COLORS ----------
#define ILI9341_WHITE 0xFFFF
#define ILI9341_BLACK 0x0000

// --- STEPPER CONFIGURATION ---
const int stepsPerRevolution = 2048;
// 1/20th of a circle = 2048 / 10 = ~204 steps
const int feedingSteps = 204;
// Sequence for 28BYJ-48: IN1, IN3, IN2, IN4
Stepper feederMotor(stepsPerRevolution, 13, 14, 12, 33);

bool alreadyFedThisMinute = false; // Prevents the motor from spinning for 60 seconds straight

// ---------- FEEDING SETTINGS ----------
int coloredSegments = 5;
int segments = 18;
String feedingTime = "08:00";
int lastMinute = -1;

// ---------- TIME PICKER STATE ----------
bool timePickerActive = false;
int editingHours = 0;
int editingMinutes = 0;
int selectedField = 0; // 0=hours, 1=minutes
unsigned long holdStartTime = 0;
int holdingField = -1; // -1=none, 0=hours up, 1=hours down, 2=mins up, 3=mins down
bool holdingActive = false;
bool firstHoldIncrement = false;
unsigned long lastHoldUpdateTime = 0;

// ---------- NEW UI LOGIC VARIABLES ----------
bool pendingServerSync = false;
unsigned long lastTouchTime = 0;
bool touchLatched = false; // Prevents "rapid fire" scrolling[cite: 3]
bool hasCachedSensorData = false;
float lastTempC = 0;
float lastWaterCm = 0;
int lastTdsValue = 0;
float lastTdsPpm = 0;
bool wifiConnectedLogged = false;

// ---------- BUTTONS ----------
struct Button
{
  int x, y, w, h;
  bool isCircle;
  int r;
};

Button setTimeBtn = {20, 35, 200, 30, false, 0};
Button feedBtn = {20, 70, 200, 30, false, 0};
Button leftButton = {40, 145, 0, 0, true, 35};
Button rightButton = {200, 145, 0, 0, true, 35};
Button fullEmptyBtn = {30, 185, 180, 30, false, 0};

// ---------- TEMPERATURE ----------
#define ONE_WIRE_BUS 25
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// ---------- WATER SENSOR ----------
#define ADDR_LOW 0x77
#define ADDR_HIGH 0x78
const int LOW_SIZE = 8;
const int HIGH_SIZE = 12;
const int TOTAL_SEGMENTS = LOW_SIZE + HIGH_SIZE;
const float BASE_HEIGHT_CM = 20.0;
const float STEP_CM = 0.5;
int waterRaw[20];
float waterHeightCm = 0;

// ---------- TDS ----------
#define TDS_PIN 34
int tdsValue = 0;
float tdsPpm = 0;

// ---------- TIMING ----------
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 60000;
unsigned long lastSegmentPoll = 0;
const unsigned long SEGMENT_INTERVAL = 2000;
unsigned long lastActivatePoll = 0;
unsigned long lastHeartbeat = 0;
int lastHandledActivateToken = -1;

// ---------- WIFI ----------
const char *ssid = "";
const char *password = "";
const char *dataUrl = "http://192.168.1.121:8000/data";
const char *activateUrl = "http://192.168.1.121:8000/activate";
const char *segmentsUrl = "http://192.168.1.121:8000/segments";

// ---------- FUNCTION DECLARATIONS ----------
void drawButtons();
bool insideButton(Button b, int x, int y);
void updateSegmentsText();
void displaySensorData(float tempC, float water, int tdsVal, float tdsPpm);
void sendSensorData(float tempC);
void pollSegments();
void pollActivate();
void sendActivateAck(int token);
void updateTimeDisplay(bool force = false);
void redrawMainScreen();
void drawTimePickerOverlay();
void handleTimePickerTouch(int x, int y);
void saveNewFeedingTime();
void initializeTimePickerFromCurrent();
void updateTimePickerValue();
int getTimePickerOkX();
void logCurrentTimeToSerial(const char *prefix);

// Helper: draw text centered inside a rectangle using the FreeSans font
void drawCenteredText(const char *s, int boxX, int boxY, int boxW, int boxH, int textSize, uint16_t fg, uint16_t bg, const GFXfont *font = &FreeSans9pt7b)
{
  // If caller passes nullptr explicitly, use default pixel font; otherwise use provided font
  if (font)
    tft.setFont(font);
  else
    tft.setFont(nullptr);
  tft.setTextSize(textSize);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  int cx = boxX + (boxW - w) / 2;
  // y needs to place baseline so add h to top offset and center vertically
  int cy = boxY + (boxH - h) / 2 + h;
  tft.setTextColor(fg, bg);
  tft.setCursor(cx, cy);
  tft.print(s);
}

// Overload for String
void drawCenteredText(String s, int boxX, int boxY, int boxW, int boxH, int textSize, uint16_t fg, uint16_t bg, const GFXfont *font = &FreeSans9pt7b)
{
  drawCenteredText(s.c_str(), boxX, boxY, boxW, boxH, textSize, fg, bg, font);
}

const int TIME_PICKER_OK_X = 232;
const int TIME_PICKER_OK_Y = 198;
const int TIME_PICKER_OK_W = 88;
const int TIME_PICKER_OK_H = 42;

// ---------- WATER SENSOR FUNCTIONS ----------
void readWaterSensor()
{
  int i = 0;
  Wire.requestFrom(ADDR_LOW, LOW_SIZE);
  while (Wire.available() && i < TOTAL_SEGMENTS)
  {
    waterRaw[i++] = Wire.read();
  }
  Wire.requestFrom(ADDR_HIGH, HIGH_SIZE);
  while (Wire.available() && i < TOTAL_SEGMENTS)
  {
    waterRaw[i++] = Wire.read();
  }
}

int getWaterIndex()
{
  readWaterSensor();
  int highest = -1;
  for (int i = 0; i < TOTAL_SEGMENTS; i++)
  {
    if (waterRaw[i] > 100)
      highest = i;
  }
  return highest;
}

void updateWaterLevel()
{
  int idx = getWaterIndex();
  waterHeightCm = (idx == -1) ? 0 : BASE_HEIGHT_CM + ((idx + 1) * STEP_CM);
}

void runFeeder()
{
  Serial.println("[MOTOR] Feeding cycle started...");
  feederMotor.setSpeed(10); // 10 RPM is stable for this motor
  feederMotor.step(feedingSteps);

  // Power down pins to save energy and prevent driver heat
  digitalWrite(13, LOW);
  digitalWrite(14, LOW);
  digitalWrite(12, LOW);
  digitalWrite(33, LOW);
  Serial.println("[MOTOR] Feeding cycle complete.");
  // Ensure touch handling unlocks after manual feed
  touchLatched = false;
}

// ---------- SETUP ----------
void setup()
{
  Serial.begin(115200);
  Serial.println("[BOOT] ESP32 starting...");
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  tft.begin();
  tft.setRotation(0);
  tft.setFont(&FreeSans9pt7b);
  tft.fillScreen(ILI9341_BLACK);
  ts.begin();
  ts.setRotation(0);
  // Ensure motor pins are set as outputs and default LOW to avoid floating states
  pinMode(13, OUTPUT);
  pinMode(14, OUTPUT);
  pinMode(12, OUTPUT);
  pinMode(33, OUTPUT);
  digitalWrite(13, LOW);
  digitalWrite(14, LOW);
  digitalWrite(12, LOW);
  digitalWrite(33, LOW);
  drawButtons();
  updateSegmentsText();
  analogSetAttenuation(ADC_11db);
  tempSensor.begin();
  Wire.begin(21, 22);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (!wifiConnectedLogged)
  {
    Serial.println("[WIFI] Connected.");
    wifiConnectedLogged = true;
  }

  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  delay(300);
  logCurrentTimeToSerial("[TIME] NTP sync after boot");
  redrawMainScreen();
  updateTimeDisplay(true);
}

// ---------- LOOP ----------
void loop()
{
  // lightweight heartbeat so we can see the device is still running
  if (millis() - lastHeartbeat > 5000)
  {
    lastHeartbeat = millis();
    Serial.println("[LOOP] heartbeat");
  }
  updateTimeDisplay(false);

  // --- AUTOMATIC FEEDING CHECK ---
  struct tm timeinfo;
  if (getLocalTime(&timeinfo))
  {
    char currentTimeStr[6];
    strftime(currentTimeStr, sizeof(currentTimeStr), "%H:%M", &timeinfo);

    if (feedingTime == currentTimeStr)
    {
      if (!alreadyFedThisMinute)
      {
        runFeeder();
        alreadyFedThisMinute = true;
      }
    }
    else
    {
      alreadyFedThisMinute = false; // Reset once the minute passes
    }
  }

  if (timePickerActive)
  {
    // Update continuous hold behavior
    updateTimePickerValue();

    // When time picker is active, only handle time picker touches
    if (ts.touched())
    {
      lastTouchTime = millis();
      if (!touchLatched)
      {
        TS_Point p = ts.getPoint();
        int x = map(p.x, 300, 3800, 0, tft.width());
        int y = map(p.y, 300, 3800, tft.height(), 0);
        handleTimePickerTouch(x, y);
        touchLatched = true;
      }
    }
    else
    {
      touchLatched = false;
      holdingField = -1;
      holdingActive = false;
      firstHoldIncrement = false;
    }
    return; // Skip normal loop processing while time picker is active
  }

  if (ts.touched())
  {
    lastTouchTime = millis(); // Record that the screen is currently being used[cite: 3]

    // Only process the touch if we aren't already holding the finger down[cite: 3]
    if (!touchLatched)
    {
      TS_Point p = ts.getPoint();
      int x = map(p.x, 300, 3800, 0, tft.width());
      int y = map(p.y, 300, 3800, tft.height(), 0);
      bool changed = false;

      if (insideButton(setTimeBtn, x, y))
      {
        // Open time picker overlay
        initializeTimePickerFromCurrent();
        timePickerActive = true;
        drawTimePickerOverlay();
      }
      else if (insideButton(feedBtn, x, y))
      {
        // Decrement segments by 1
        if (coloredSegments > 0)
        {
          coloredSegments--;
          changed = true;
        }
        runFeeder(); // Trigger motor on manual button press
      }
      else if (insideButton(leftButton, x, y))
      {
        if (coloredSegments > 0)
        {
          coloredSegments--;
          changed = true;
        }
      }
      else if (insideButton(rightButton, x, y))
      {
        if (coloredSegments < segments)
        {
          coloredSegments++;
          changed = true;
        }
      }
      else if (insideButton(fullEmptyBtn, x, y))
      {
        coloredSegments = (coloredSegments <= 17) ? 18 : 0;
        changed = true;
      }

      if (changed)
      {
        updateSegmentsText();
        pendingServerSync = true;
      }
      touchLatched = true; // Lock further changes until finger is lifted[cite: 3]
    }
  }
  else
  {
    // Finger was lifted, unlock the buttons for the next tap[cite: 3]
    touchLatched = false;
  }

  // --- BACKGROUND SYNC ---
  // Only sync to server if screen hasn't been touched for 1.5 seconds[cite: 3]
  if (pendingServerSync && (millis() - lastTouchTime > 1500))
  {
    pendingServerSync = false;
    if (WiFi.status() == WL_CONNECTED)
    {
      HTTPClient http;
      http.setTimeout(500); // Very short timeout so UI stays snappy[cite: 3]
      http.begin(segmentsUrl);
      http.addHeader("Content-Type", "application/json");
      StaticJsonDocument<100> doc;
      doc["segments"] = coloredSegments;
      String json;
      serializeJson(doc, json);
      int status = http.POST(json);
      Serial.print("[SEGMENTS POST] status=");
      Serial.print(status);
      Serial.print(" payload=");
      Serial.println(json);
      http.end();
    }
  }

  // --- SENSOR READING ---
  if (millis() - lastSensorRead >= SENSOR_INTERVAL)
  {
    lastSensorRead = millis();
    updateWaterLevel();
    tempSensor.requestTemperatures();
    float tempC = tempSensor.getTempCByIndex(0);
    tdsValue = analogRead(TDS_PIN);
    tdsPpm = map(tdsValue * (3.3 / 4095.0) * 1000, 0, 2300, 0, 1000);
    lastTempC = tempC;
    lastWaterCm = waterHeightCm;
    lastTdsValue = tdsValue;
    lastTdsPpm = tdsPpm;
    hasCachedSensorData = true;
    displaySensorData(tempC, waterHeightCm, tdsValue, tdsPpm);

    // Only send data if user isn't currently using the screen[cite: 3]
    if (WiFi.status() == WL_CONNECTED && !pendingServerSync && !ts.touched())
    {
      sendSensorData(tempC);
    }
  }

  // --- POLL SEGMENTS ---
  if (millis() - lastSegmentPoll >= SEGMENT_INTERVAL)
  {
    lastSegmentPoll = millis();
    // Increase wait time to 5 seconds after touch before polling[cite: 3]
    if ((millis() - lastTouchTime > 5000) && !pendingServerSync && !ts.touched())
    {
      pollSegments();
    }
  }

  // --- POLL MANUAL FEED COMMAND ---
  if (millis() - lastActivatePoll >= SEGMENT_INTERVAL)
  {
    lastActivatePoll = millis();
    if (WiFi.status() == WL_CONNECTED)
    {
      pollActivate();
    }
  }
}

// ---------- HELPERS ----------
void sendSensorData(float tempC)
{
  HTTPClient http;
  http.setTimeout(800);
  http.begin(dataUrl);
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<200> doc;
  doc["tempC"] = tempC;
  doc["water"] = waterHeightCm;
  doc["tdsVal"] = (float)tdsValue;
  doc["tdsPpm"] = tdsPpm;
  doc["segments"] = coloredSegments;
  String json;
  serializeJson(doc, json);
  int status = http.POST(json);
  Serial.print("[SENSOR POST] status=");
  Serial.print(status);
  Serial.print(" payload=");
  Serial.println(json);
  http.end();
}

void logCurrentTimeToSerial(const char *prefix)
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 3000))
  {
    Serial.print(prefix);
    Serial.println(" FAILED (no NTP time yet)");
    return;
  }

  char timeStr[24];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  Serial.print(prefix);
  Serial.print(" OK: ");
  Serial.println(timeStr);
}

void pollSegments()
{
  HTTPClient http;
  http.setTimeout(1500);
  http.setConnectTimeout(1500);
  http.begin(segmentsUrl);
  int status = http.GET();
  Serial.print("[SEGMENTS GET] status=");
  Serial.println(status);
  if (status == 200)
  {
    StaticJsonDocument<200> resp;
    DeserializationError err = deserializeJson(resp, http.getStream());
    if (err)
    {
      Serial.print("[SEGMENTS GET] JSON parse error: ");
      Serial.println(err.c_str());
    }
    else
    {
      Serial.print("[SEGMENTS GET] body={\"segments\":");
      Serial.print((int)(resp["segments"] | 0));
      Serial.println("}");
      int newSegments = resp["segments"] | 0;
      if (newSegments != coloredSegments)
      {
        coloredSegments = newSegments;
        updateSegmentsText();
      }
    }
  }
  else
  {
    Serial.println("[SEGMENTS GET] failed or non-200 response");
  }
  http.end();
}

void pollActivate()
{
  HTTPClient http;
  http.setTimeout(1500);
  http.setConnectTimeout(1500);
  http.begin(activateUrl);
  int status = http.GET();
  Serial.print("[ACTIVATE GET] status=");
  Serial.println(status);
  if (status == 200)
  {
    StaticJsonDocument<200> resp;
    DeserializationError err = deserializeJson(resp, http.getStream());
    if (err)
    {
      Serial.print("[ACTIVATE GET] JSON parse error: ");
      Serial.println(err.c_str());
    }
    else
    {
      bool activate = resp["activate"] | false;
      int token = resp["token"] | 0;
      Serial.print("[ACTIVATE GET] body={\"activate\":");
      Serial.print(activate ? "true" : "false");
      Serial.print(",\"token\":");
      Serial.print(token);
      Serial.println("}");
      if (activate && token != lastHandledActivateToken)
      {
        lastHandledActivateToken = token;
        Serial.println("[FEED] Manual feeding requested from web");
        if (coloredSegments > 0)
        {
          coloredSegments--;
          updateSegmentsText();
        }
        runFeeder();
        sendActivateAck(token);
      }
    }
  }
  else
  {
    Serial.println("[ACTIVATE GET] failed or non-200 response");
  }
  http.end();
}

void sendActivateAck(int token)
{
  HTTPClient http;
  http.setTimeout(1500);
  http.setConnectTimeout(1500);
  http.begin("http://192.168.1.121:8000/activate_ack");
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<100> doc;
  doc["token"] = token;
  String json;
  serializeJson(doc, json);
  int status = http.POST(json);
  Serial.print("[ACTIVATE ACK] status=");
  Serial.print(status);
  Serial.print(" payload=");
  Serial.println(json);
  http.end();
}

void updateTimeDisplay(bool force)
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return;
  if (timeinfo.tm_min != lastMinute || force)
  {
    lastMinute = timeinfo.tm_min;
    tft.fillRect(0, 0, tft.width(), 30, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    char timeStr[10];
    strftime(timeStr, 10, "%H:%M", &timeinfo);
    const int topLabelH = 9;
    const int topValueY = 16;
    const int leftW = 120;
    const int rightW = tft.width() - leftW;

    // Two stacked columns: label on top, value below
    drawCenteredText("CURRENT TIME", 0, 0, leftW, topLabelH, 0, ILI9341_WHITE, ILI9341_BLACK, nullptr);
    drawCenteredText(timeStr, 0, topValueY, leftW, 14, 0, ILI9341_WHITE, ILI9341_BLACK, &FreeSans9pt7b);
    drawCenteredText("NEXT FEEDING:", leftW, 0, rightW, topLabelH, 0, ILI9341_WHITE, ILI9341_BLACK, nullptr);
    drawCenteredText(feedingTime, leftW, topValueY, rightW, 14, 0, ILI9341_WHITE, ILI9341_BLACK, &FreeSans9pt7b);
  }
}

void drawButtons()
{
  tft.fillRect(setTimeBtn.x, setTimeBtn.y, setTimeBtn.w, setTimeBtn.h, ILI9341_WHITE);
  tft.drawRect(setTimeBtn.x, setTimeBtn.y, setTimeBtn.w, setTimeBtn.h, ILI9341_BLACK);
  // Single-line label using same FreeSans font at size 1 (matches FULL/EMPTY)
  drawCenteredText("SET FEEDING TIME", setTimeBtn.x, setTimeBtn.y, setTimeBtn.w, setTimeBtn.h, 1, ILI9341_BLACK, ILI9341_WHITE);

  // Auto-size FEED button to text width (centered inside original area)
  tft.setFont(&FreeSans9pt7b);
  tft.setTextSize(1);
  int16_t bx, by;
  uint16_t bw, bh;
  tft.getTextBounds("FEED", 0, 0, &bx, &by, &bw, &bh);
  int padX = 12;
  int newFeedW = bw + padX * 2;
  int newFeedX = feedBtn.x + (feedBtn.w - newFeedW) / 2;
  feedBtn.x = newFeedX;
  feedBtn.w = newFeedW;
  tft.fillRect(feedBtn.x, feedBtn.y, feedBtn.w, feedBtn.h, ILI9341_WHITE);
  tft.drawRect(feedBtn.x, feedBtn.y, feedBtn.w, feedBtn.h, ILI9341_BLACK);
  drawCenteredText("FEED", feedBtn.x, feedBtn.y, feedBtn.w, feedBtn.h, 1, ILI9341_BLACK, ILI9341_WHITE);

  // Draw segment counter first (background layer)
  tft.fillRect(70, 112, 100, 35, ILI9341_BLACK);
  // Center the dynamic segment counter text so its width doesn't shift layout
  char segbuf[16];
  sprintf(segbuf, "%d/%d", coloredSegments, segments);
  drawCenteredText(segbuf, 70, 112, 100, 35, 2, ILI9341_WHITE, ILI9341_BLACK);

  // Draw +/- buttons on top layer (after counter)
  // Draw +/- buttons on top layer (after counter)
  const int cr = 20;
  tft.fillCircle(leftButton.x, leftButton.y, cr, ILI9341_WHITE);
  tft.drawCircle(leftButton.x, leftButton.y, cr, ILI9341_BLACK);
  tft.fillCircle(rightButton.x, rightButton.y, cr, ILI9341_WHITE);
  tft.drawCircle(rightButton.x, rightButton.y, cr, ILI9341_BLACK);
  drawCenteredText("-", leftButton.x - cr, leftButton.y - cr - 12, cr * 2, cr * 2, 2, ILI9341_BLACK, ILI9341_WHITE, nullptr);
  drawCenteredText("+", rightButton.x - cr, rightButton.y - cr - 12, cr * 2, cr * 2, 2, ILI9341_BLACK, ILI9341_WHITE, nullptr);

  // Auto-size FULL/EMPTY button to text width
  tft.setFont(&FreeSans9pt7b);
  tft.setTextSize(1);
  tft.getTextBounds("FULL/EMPTY", 0, 0, &bx, &by, &bw, &bh);
  int newFullW = bw + padX * 2;
  int newFullX = fullEmptyBtn.x + (fullEmptyBtn.w - newFullW) / 2;
  fullEmptyBtn.x = newFullX;
  fullEmptyBtn.w = newFullW;
  tft.fillRect(fullEmptyBtn.x, fullEmptyBtn.y, fullEmptyBtn.w, fullEmptyBtn.h, ILI9341_WHITE);
  tft.drawRect(fullEmptyBtn.x, fullEmptyBtn.y, fullEmptyBtn.w, fullEmptyBtn.h, ILI9341_BLACK);
  drawCenteredText("FULL/EMPTY", fullEmptyBtn.x, fullEmptyBtn.y, fullEmptyBtn.w, fullEmptyBtn.h, 1, ILI9341_BLACK, ILI9341_WHITE);
}

bool insideButton(Button b, int x, int y)
{
  if (b.isCircle)
  {
    int dx = x - b.x;
    int dy = y - b.y;
    return dx * dx + dy * dy <= b.r * b.r;
  }
  return (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h);
}

void updateSegmentsText()
{
  // Draw segment counter with new styling (black bg, white text)
  tft.fillRect(70, 112, 100, 35, ILI9341_BLACK);
  char segbuf[16];
  sprintf(segbuf, "%d/%d", coloredSegments, segments);
  drawCenteredText(segbuf, 70, 112, 100, 35, 2, ILI9341_WHITE, ILI9341_BLACK);

  // Redraw +/- buttons on top layer to avoid being covered
  const int cr = 20;
  tft.fillCircle(leftButton.x, leftButton.y, cr, ILI9341_WHITE);
  tft.drawCircle(leftButton.x, leftButton.y, cr, ILI9341_BLACK);
  tft.fillCircle(rightButton.x, rightButton.y, cr, ILI9341_WHITE);
  tft.drawCircle(rightButton.x, rightButton.y, cr, ILI9341_BLACK);
  drawCenteredText("-", leftButton.x - cr, leftButton.y - cr - 12, cr * 2, cr * 2, 2, ILI9341_BLACK, ILI9341_WHITE, nullptr);
  drawCenteredText("+", rightButton.x - cr, rightButton.y - cr - 12, cr * 2, cr * 2, 2, ILI9341_BLACK, ILI9341_WHITE, nullptr);
}

void displaySensorData(float tempC, float water, int tdsVal, float tdsPpm)
{
  // Move sensor block down to avoid colliding with FULL/EMPTY button
  tft.fillRect(0, 235, tft.width(), 85, ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(1); // match FULL/EMPTY size
  tft.setCursor(10, 245);
  tft.print("Temperature: ");
  tft.print(tempC, 1);
  tft.print(" C");
  tft.setCursor(10, 265);
  tft.print("Water: ");
  tft.print(water, 1);
  tft.print(" cm");
  tft.setCursor(10, 285);
  tft.print("TDS: ");
  tft.print(tdsVal);
  tft.print(" (~");
  tft.print(tdsPpm, 0);
  tft.print(" ppm)");
}

void redrawMainScreen()
{
  tft.fillScreen(ILI9341_BLACK);
  drawButtons();
  updateSegmentsText();
  updateTimeDisplay(true);
  if (hasCachedSensorData)
  {
    displaySensorData(lastTempC, lastWaterCm, lastTdsValue, lastTdsPpm);
  }
}

// ---------- TIME PICKER FUNCTIONS ----------
void initializeTimePickerFromCurrent()
{
  // Parse current feeding time (format: "HH:MM")
  editingHours = (feedingTime[0] - '0') * 10 + (feedingTime[1] - '0');
  editingMinutes = (feedingTime[3] - '0') * 10 + (feedingTime[4] - '0');
  selectedField = 0;
  holdingField = -1;
  holdingActive = false;
}

void drawTimePickerOverlay()
{
  tft.fillScreen(ILI9341_BLACK);

  drawCenteredText("SET FEEDING TIME", 0, 12, tft.width(), 24, 1, ILI9341_WHITE, ILI9341_BLACK);

  const int topY = 80;
  const int hoursX = 40;
  const int minutesX = 136;
  const int boxWidth = 72; // narrower so the right box fits on-screen
  const int boxHeight = 52;
  const int valueY = topY + 35;
  const int arrowTopY = topY + 8;
  const int arrowBottomY = topY + 90;

  tft.drawRect(hoursX, topY + 30, boxWidth, boxHeight, ILI9341_WHITE);
  if (selectedField == 0)
  {
    tft.drawRect(hoursX - 2, topY + 28, boxWidth + 4, boxHeight + 4, ILI9341_WHITE);
  }
  // Draw hours value centered in the hours box
  char hoursBuf[4];
  sprintf(hoursBuf, "%02d", editingHours);
  drawCenteredText(hoursBuf, hoursX, topY + 30, boxWidth, boxHeight, 2, ILI9341_WHITE, ILI9341_BLACK);

  tft.drawRect(minutesX, topY + 30, boxWidth, boxHeight, ILI9341_WHITE);
  if (selectedField == 1)
  {
    tft.drawRect(minutesX - 2, topY + 28, boxWidth + 4, boxHeight + 4, ILI9341_WHITE);
  }
  // Draw minutes value centered in the minutes box
  char minsBuf[4];
  sprintf(minsBuf, "%02d", editingMinutes);
  drawCenteredText(minsBuf, minutesX, topY + 30, boxWidth, boxHeight, 2, ILI9341_WHITE, ILI9341_BLACK);

  // Draw colon between hour and minute boxes (centered between boxes)
  int colonX = hoursX + boxWidth + 6;
  drawCenteredText(":", colonX, valueY - 16, 16, 32, 2, ILI9341_WHITE, ILI9341_BLACK);

  // Draw up/down arrows centered above/below each box
  int hx = hoursX + boxWidth / 2;
  int mx = minutesX + boxWidth / 2;
  const int aw = 10; // half-width of arrow
  // top arrows (pointing up)
  tft.drawLine(hx - aw, arrowTopY + 10, hx, arrowTopY, ILI9341_WHITE);
  tft.drawLine(hx, arrowTopY, hx + aw, arrowTopY + 10, ILI9341_WHITE);
  tft.drawLine(hx + aw, arrowTopY + 10, hx - aw, arrowTopY + 10, ILI9341_WHITE);
  tft.drawLine(mx - aw, arrowTopY + 10, mx, arrowTopY, ILI9341_WHITE);
  tft.drawLine(mx, arrowTopY, mx + aw, arrowTopY + 10, ILI9341_WHITE);
  tft.drawLine(mx + aw, arrowTopY + 10, mx - aw, arrowTopY + 10, ILI9341_WHITE);
  // bottom arrows (pointing down)
  tft.drawLine(hx - aw, arrowBottomY - 10, hx, arrowBottomY, ILI9341_WHITE);
  tft.drawLine(hx, arrowBottomY, hx + aw, arrowBottomY - 10, ILI9341_WHITE);
  tft.drawLine(hx + aw, arrowBottomY - 10, hx - aw, arrowBottomY - 10, ILI9341_WHITE);
  tft.drawLine(mx - aw, arrowBottomY - 10, mx, arrowBottomY, ILI9341_WHITE);
  tft.drawLine(mx, arrowBottomY, mx + aw, arrowBottomY - 10, ILI9341_WHITE);
  tft.drawLine(mx + aw, arrowBottomY - 10, mx - aw, arrowBottomY - 10, ILI9341_WHITE);

  // Ensure OK button is fully on-screen; compute safe X position
  int okX = getTimePickerOkX();
  tft.drawRect(okX, TIME_PICKER_OK_Y, TIME_PICKER_OK_W, TIME_PICKER_OK_H, ILI9341_WHITE);
  drawCenteredText("OK", okX, TIME_PICKER_OK_Y, TIME_PICKER_OK_W, TIME_PICKER_OK_H, 1, ILI9341_WHITE, ILI9341_BLACK);
}

void handleTimePickerTouch(int x, int y)
{
  const int hoursX = 34;
  const int minutesX = 168;
  const int topY = 80;
  const int boxWidth = 88;
  const int boxHeight = 52;
  const int arrowTopY = topY + 8;
  const int arrowBottomY = topY + 90;

  auto applyStep = [&](int field, int direction)
  {
    if (field == 0)
    {
      editingHours += direction;
      if (editingHours > 23)
      {
        editingHours = 0;
      }
      else if (editingHours < 0)
      {
        editingHours = 23;
      }
    }
    else
    {
      editingMinutes += direction;
      if (editingMinutes > 59)
      {
        editingMinutes = 0;
      }
      else if (editingMinutes < 0)
      {
        editingMinutes = 59;
      }
    }
    drawTimePickerOverlay();
  };

  int hx = hoursX + boxWidth / 2;
  int mx = minutesX + boxWidth / 2;
  int arrowHitHalf = 12;
  if (x >= hx - arrowHitHalf && x <= hx + arrowHitHalf && y >= arrowTopY && y <= arrowTopY + 18)
  {
    selectedField = 0;
    holdingField = 0;
    holdStartTime = millis();
    holdingActive = true;
    firstHoldIncrement = false;
    lastHoldUpdateTime = holdStartTime;
    applyStep(0, 1);
    return;
  }
  if (x >= hx - arrowHitHalf && x <= hx + arrowHitHalf && y >= arrowBottomY - 18 && y <= arrowBottomY)
  {
    selectedField = 0;
    holdingField = 1;
    holdStartTime = millis();
    holdingActive = true;
    firstHoldIncrement = false;
    lastHoldUpdateTime = holdStartTime;
    applyStep(0, -1);
    return;
  }
  if (x >= mx - arrowHitHalf && x <= mx + arrowHitHalf && y >= arrowTopY && y <= arrowTopY + 18)
  {
    selectedField = 1;
    holdingField = 2;
    holdStartTime = millis();
    holdingActive = true;
    firstHoldIncrement = false;
    lastHoldUpdateTime = holdStartTime;
    applyStep(1, 1);
    return;
  }
  if (x >= mx - arrowHitHalf && x <= mx + arrowHitHalf && y >= arrowBottomY - 18 && y <= arrowBottomY)
  {
    selectedField = 1;
    holdingField = 3;
    holdStartTime = millis();
    holdingActive = true;
    firstHoldIncrement = false;
    lastHoldUpdateTime = holdStartTime;
    applyStep(1, -1);
    return;
  }

  int okX = getTimePickerOkX();
  if (x >= okX && x <= okX + TIME_PICKER_OK_W &&
      y >= TIME_PICKER_OK_Y && y <= TIME_PICKER_OK_Y + TIME_PICKER_OK_H)
  {
    Serial.println("TimePicker: OK pressed");
    saveNewFeedingTime();
    // Clear overlay explicitly before returning to main screen
    tft.fillScreen(ILI9341_BLACK);
    timePickerActive = false;
    holdingField = -1;
    holdingActive = false;
    firstHoldIncrement = false;
    touchLatched = false;
    redrawMainScreen();
    return;
  }

  if (x >= hoursX && x <= hoursX + boxWidth && y >= topY + 30 && y <= topY + 30 + boxHeight)
  {
    selectedField = 0;
    drawTimePickerOverlay();
    return;
  }

  if (x >= minutesX && x <= minutesX + boxWidth && y >= topY + 30 && y <= topY + 30 + boxHeight)
  {
    selectedField = 1;
    drawTimePickerOverlay();
    return;
  }
}

void updateTimePickerValue()
{
  if (!holdingActive || holdingField == -1)
  {
    return;
  }

  unsigned long elapsed = millis() - holdStartTime;
  if (elapsed < 2000)
  {
    return;
  }

  if (millis() - lastHoldUpdateTime < 200)
  {
    return;
  }

  lastHoldUpdateTime = millis();
  if (holdingField == 0)
  {
    editingHours = (editingHours < 23) ? editingHours + 1 : 0;
  }
  else if (holdingField == 1)
  {
    editingHours = (editingHours > 0) ? editingHours - 1 : 23;
  }
  else if (holdingField == 2)
  {
    editingMinutes = (editingMinutes < 59) ? editingMinutes + 1 : 0;
  }
  else if (holdingField == 3)
  {
    editingMinutes = (editingMinutes > 0) ? editingMinutes - 1 : 59;
  }
  drawTimePickerOverlay();
}

void saveNewFeedingTime()
{
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", editingHours, editingMinutes);
  feedingTime = String(timeStr);
}

int getTimePickerOkX()
{
  int okX = TIME_PICKER_OK_X;
  if (okX + TIME_PICKER_OK_W > tft.width() - 4)
  {
    okX = tft.width() - TIME_PICKER_OK_W - 8;
  }
  return okX;
}