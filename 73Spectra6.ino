#include <SPI.h>
#include <WiFi.h>
#include <FS.h>
#include <SD.h>
#include "epd7in3f.h"
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include "esp_adc_cal.h"
#include "driver/adc.h"
#include <ESP32Servo.h>
#include <map>
#include <limits.h>
#include "driver/rtc_io.h"
#include "esp_system.h"


//This is the pin for the transistor that powers the external components
#define TRANSISTOR_PIN 26
#define WAKEUP_PIN 27
#define SERVO_PIN 12
#define SERVO_TRANS_PIN 04


Preferences preferences;

Epd epd;
unsigned long delta;                 // Variable to store the time it took to update the display for deep sleep calculations
unsigned long deltaSinceTimeObtain;  // Variable to store the time it took to update the display since the time was obtained for deep sleep calculations

#define SD_CS_PIN 22

struct ImageInfo {
  String name;
  uint32_t count;
};

const char *IMAGE_COUNT_FILE = "/COUNTS.JSN";
const uint32_t REFRESH_RESCAN_INTERVAL = 7;
const char *REFRESH_COUNTER_KEY = "refreshCount";

uint16_t width() {
  return EPD_WIDTH;
}
uint16_t height() {
  return EPD_HEIGHT;
}

SPIClass vspi(SPI);  // VSPI for SD card

// Color pallete for dithering. These are specific to the 7in3e waveshare display.
uint8_t colorPallete[6 * 3] = {
  0, 0, 0,
  255, 255, 255,
  255, 255, 0,
  255, 0, 0,
  0, 0, 255,
  0, 255, 0
};

// Fix #3: Read multi-byte values in a single SPI transaction instead of one byte at a time
uint16_t read16(fs::File &f) {
  uint8_t buf[2];
  f.read(buf, 2);
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

uint32_t read32(fs::File &f) {
  uint8_t buf[4];
  f.read(buf, 4);
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}


Servo servo;
bool rotateDisplay(bool isVert) {
  // Fix #1+#2: Read orientation once, and return early without touching the servo
  // if the frame is already in the correct orientation — saves 1.6s + servo current draw.
  if (!preferences.isKey("orientation")) {
    preferences.putString("orientation", "H");
  }
  String current = preferences.getString("orientation");  // read once, not 3-4 times

  bool alreadyCorrect = (current == "V") == isVert;
  if (alreadyCorrect) {
    Serial.println("Orientation already correct: " + current);
    return true;
  }

  // Turn on the transistor to power the servo
  pinMode(SERVO_TRANS_PIN, OUTPUT);
  digitalWrite(SERVO_TRANS_PIN, HIGH);
  delay(100);

  servo.setPeriodHertz(50);            // PWM frequency for SG90
  servo.attach(SERVO_PIN, 500, 2400);  // Minimum and maximum pulse width (in µs) to go from 0° to 180
  int speedDelay = 20;

  Serial.println("Orientation Start: " + current);
  if (current == "V" && !isVert) {
    Serial.println("Rotating to Horizontal");
    for (int pos = 0; pos <= 120; pos += 1) {
      servo.write(pos);
      delay(speedDelay);
    }
    preferences.putString("orientation", "H");
  } else if (current == "H" && isVert) {
    Serial.println("Rotating to Vertical");
    for (int pos = 120; pos >= 0; pos -= 1) {
      servo.write(pos);
      delay(speedDelay);
    }
    preferences.putString("orientation", "V");
  }

  delay(1500);
  servo.detach();
  digitalWrite(SERVO_TRANS_PIN, LOW);
  Serial.println("Orientation End: " + preferences.getString("orientation"));

  return true;
}


float readBattery() {
  uint32_t value = 0;
  int rounds = 11;
  esp_adc_cal_characteristics_t adc_chars;

  //battery voltage divided by 2 can be measured at GPIO34, which equals ADC1_CHANNEL6
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
  switch (esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars)) {
    case ESP_ADC_CAL_VAL_EFUSE_TP:
      Serial.println("Characterized using Two Point Value");
      break;
    case ESP_ADC_CAL_VAL_EFUSE_VREF:
      Serial.printf("Characterized using eFuse Vref (%d mV)\r\n", adc_chars.vref);
      break;
    default:
      Serial.printf("Characterized using Default Vref (%d mV)\r\n", 1100);
  }

  //to avoid noise, sample the pin several times and average the result
  for (int i = 1; i <= rounds; i++) {
    value += adc1_get_raw(ADC1_CHANNEL_6);
  }
  value /= (uint32_t)rounds;

  //due to the voltage divider (1M+1M) values must be multiplied by 2
  //and convert mV to V
  return esp_adc_cal_raw_to_voltage(value, &adc_chars) * 2.0 / 1000.0;
}

bool isBmpFile(const String &name) {
  return name.length() >= 4 && name.substring(name.length() - 4).equalsIgnoreCase(".bmp");
}

bool isExcludedFile(const String &name) {
  return name.equalsIgnoreCase("COUNTS.JSN") || name.equalsIgnoreCase("COUNTS.JSON") || name.equalsIgnoreCase("fileList.json") || name.equalsIgnoreCase("debug.log");
}

bool parseJsonCountEntry(const String &line, String &outName, uint32_t &outCount) {
  int firstQuote = line.indexOf('"');
  if (firstQuote < 0) {
    return false;
  }

  int secondQuote = line.indexOf('"', firstQuote + 1);
  if (secondQuote <= firstQuote + 1) {
    return false;
  }

  int colon = line.indexOf(':', secondQuote + 1);
  if (colon < 0) {
    return false;
  }

  outName = line.substring(firstQuote + 1, secondQuote);
  String valuePart = line.substring(colon + 1);
  valuePart.trim();
  if (valuePart.endsWith(",")) {
    valuePart.remove(valuePart.length() - 1);
  }
  outCount = static_cast<uint32_t>(valuePart.toInt());
  return true;
}

void scanSdImages(std::vector<String> &images) {
  images.clear();
  File root = SD.open("/");
  if (!root) {
    Serial.println("Error: Failed to open SD card root directory.");
    return;
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) {
      break;
    }

    String name = entry.name();
    if (!entry.isDirectory() && isBmpFile(name) && !isExcludedFile(name)) {
      images.push_back(name);
    }
    entry.close();
  }
  root.close();

  std::sort(images.begin(), images.end());
  Serial.println("Found " + String(images.size()) + " image(s) on SD card.");
}

void loadCountsFromJson(std::map<String, uint32_t> &counts) {
  counts.clear();

  File countFile = SD.open(IMAGE_COUNT_FILE, FILE_READ);
  if (!countFile) {
    Serial.println("No existing count file found, starting fresh.");
    return;
  }

  while (countFile.available()) {
    String line = countFile.readStringUntil('\n');
    line.trim();

    if (line.isEmpty() || line == "{" || line == "}") {
      continue;
    }

    String fileName;
    uint32_t count = 0;
    if (parseJsonCountEntry(line, fileName, count)) {
      counts[fileName] = count;
    }
  }

  countFile.close();
}

bool saveCountsToJson(const std::vector<ImageInfo> &images) {
  SD.remove(IMAGE_COUNT_FILE);
  File countFile = SD.open(IMAGE_COUNT_FILE, FILE_WRITE);
  if (!countFile) {
    Serial.println("Failed to open JSON count file for writing.");
    return false;
  }

  countFile.println("{");
  for (size_t i = 0; i < images.size(); i++) {
    countFile.print("  \"");
    countFile.print(images[i].name);
    countFile.print("\": ");
    countFile.print(images[i].count);
    if (i < images.size() - 1) {
      countFile.print(",");
    }
    countFile.println();
  }
  countFile.println("}");
  countFile.close();

  return true;
}

bool buildImageList(std::vector<ImageInfo> &images, bool forceRescan = false) {
  std::map<String, uint32_t> counts;

  // Skip loading the JSON entirely if we know we're forcing a full rescan
  // (we still need it to preserve existing counts for unchanged files)
  loadCountsFromJson(counts);

  bool shouldRescan = forceRescan || counts.empty();
  if (!shouldRescan) {
    uint32_t refreshCount = preferences.getUInt(REFRESH_COUNTER_KEY, 0);
    refreshCount++;
    preferences.putUInt(REFRESH_COUNTER_KEY, refreshCount);

    if ((refreshCount % REFRESH_RESCAN_INTERVAL) == 0) {
      shouldRescan = true;
      Serial.println("Periodic SD rescan triggered (every 7th refresh).");
    }
  }

  if (!shouldRescan) {
    images.clear();
    images.reserve(counts.size());
    for (const auto &kv : counts) {
      ImageInfo info;
      info.name = kv.first;
      info.count = kv.second;
      images.push_back(info);
    }

    std::sort(images.begin(), images.end(), [](const ImageInfo &a, const ImageInfo &b) {
      return a.name < b.name;
    });

    Serial.println("Loaded " + String(images.size()) + " image(s) from JSON.");
    return !images.empty();
  }

  std::vector<String> currentImages;
  scanSdImages(currentImages);

  if (currentImages.empty()) {
    Serial.println("No .bmp images found on SD card.");
    images.clear();
    return false;
  }

  int added = 0;
  int removed = 0;
  for (const auto &name : currentImages) {
    if (!counts.count(name)) {
      added++;
    }
  }
  for (const auto &kv : counts) {
    if (!std::binary_search(currentImages.begin(), currentImages.end(), kv.first)) {
      removed++;
    }
  }
  if (added > 0) {
    Serial.println(String(added) + " new image(s) added.");
  }
  if (removed > 0) {
    Serial.println(String(removed) + " image(s) removed.");
  }
  bool listChanged = (added > 0 || removed > 0);
  if (!listChanged && !counts.empty()) {
    Serial.println("Image list unchanged, skipping SD write.");
  }

  images.clear();
  images.reserve(currentImages.size());
  for (const auto &name : currentImages) {
    ImageInfo info;
    info.name = name;
    info.count = counts.count(name) ? counts[name] : 0;
    images.push_back(info);
  }

  // Only rewrite the JSON file if the image list actually changed
  if (listChanged) {
    if (!saveCountsToJson(images)) {
      return false;
    }
  }

  return true;
}


void setup() {
  Serial.begin(115200);
  delta = millis();

  // Seed the RNG using the ESP32 hardware true-RNG so image selection is genuinely random
  randomSeed(esp_random());

  // Turn off WiFi on boot to save ~100mA during offline execution
  WiFi.mode(WIFI_OFF);

  preferences.begin("e-paper", false);

  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from deep sleep due to timer.");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up from deep sleep due to button press.");
  } else {
    Serial.println("Did not wake up from deep sleep.");
  }

  // Turn on the transistor to power the external components
  pinMode(TRANSISTOR_PIN, OUTPUT);
  digitalWrite(TRANSISTOR_PIN, HIGH);
  delay(100);

  // Fix #6: Use if instead of while — hibernate() calls esp_deep_sleep() and never returns,
  // so the while loop was misleading (it never actually retried).
  if (!SD.begin(SD_CS_PIN, vspi)) {
    Serial.println("Card Mount Failed — SD card missing or faulty. Sleeping for 24h.");
    hibernate();
  }


  deltaSinceTimeObtain = millis();

  // Initialize the e-paper display
  if (epd.Init() != 0) {
    Serial.println("eP init F");
    hibernate();
  } else {
    Serial.println("eP init no F");
  }

  String file = getNextFile();  // Get the next file to display
  if (file == "") {
    Serial.println("No image selected. Going to sleep.");
    digitalWrite(TRANSISTOR_PIN, LOW);
    preferences.end();
    hibernate();
  }

  drawBmp(file.c_str());  // Display the file

  digitalWrite(TRANSISTOR_PIN, LOW);  // Turn off external components
  preferences.end();
}

void loop() {
  hibernate();
}

void hibernate() {
  Serial.println("start sleep");

  // 1. Make sure transistors are actively turned off
  digitalWrite(TRANSISTOR_PIN, LOW);
  digitalWrite(SERVO_TRANS_PIN, LOW);

  // 2. Prevent floating pins from turning on the transistors during deep sleep
  rtc_gpio_pulldown_en(static_cast<gpio_num_t>(TRANSISTOR_PIN));
  rtc_gpio_pulldown_en(static_cast<gpio_num_t>(SERVO_TRANS_PIN));

  // 3. Prevent parasitic power leak (current leaking into unpowered SD/e-paper via data pins)
  pinMode(SD_CS_PIN, INPUT);
  pinMode(16, INPUT); // E-paper CS
  pinMode(17, INPUT); // E-paper DC
  pinMode(3, INPUT);  // E-paper RST
  pinMode(18, INPUT); // VSPI SCK
  pinMode(19, INPUT); // VSPI MISO
  pinMode(23, INPUT); // VSPI MOSI
  pinMode(21, INPUT); // HSPI SCK
  pinMode(SERVO_PIN, INPUT); // Servo PWM signal

  // Enable internal pull-up for the wake-up pin during deep sleep
  rtc_gpio_pullup_en(static_cast<gpio_num_t>(WAKEUP_PIN));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(WAKEUP_PIN));

  // Wake up when the user button (GPIO 27) is pressed (pulled LOW)
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(WAKEUP_PIN), 0);

  // Shut down unused ESP32 power domains to squeeze out extra microamps
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);

  esp_deep_sleep(static_cast<uint64_t>(86400) * 1e6);
}

// Function to get the next file to display
String getNextFile() {
  std::vector<ImageInfo> images;
  if (!buildImageList(images, false)) {
    return "";
  }

  uint32_t minCount = UINT_MAX;
  for (const auto &image : images) {
    if (image.count < minCount) {
      minCount = image.count;
    }
  }

  std::vector<size_t> minCountIndices;
  for (size_t i = 0; i < images.size(); i++) {
    if (images[i].count == minCount) {
      minCountIndices.push_back(i);
    }
  }

  if (minCountIndices.empty()) {
    return "";
  }

  size_t randomIndex = static_cast<size_t>(random(minCountIndices.size()));
  size_t selectedIndex = minCountIndices[randomIndex];
  String selectedPath = "/" + images[selectedIndex].name;

  if (!SD.exists(selectedPath.c_str())) {
    Serial.println("Expected file missing: " + selectedPath + ". Forcing SD rescan...");
    if (!buildImageList(images, true)) {
      return "";
    }

    minCount = UINT_MAX;
    for (const auto &image : images) {
      if (image.count < minCount) {
        minCount = image.count;
      }
    }

    minCountIndices.clear();
    for (size_t i = 0; i < images.size(); i++) {
      if (images[i].count == minCount) {
        minCountIndices.push_back(i);
      }
    }
    if (minCountIndices.empty()) {
      return "";
    }

    randomIndex = static_cast<size_t>(random(minCountIndices.size()));
    selectedIndex = minCountIndices[randomIndex];
    selectedPath = "/" + images[selectedIndex].name;
  }

  Serial.println("Displaying: " + images[selectedIndex].name +
                 " (shown " + String(images[selectedIndex].count) + " time(s))");
  images[selectedIndex].count++;

  if (!saveCountsToJson(images)) {
    return "";
  }

  return selectedPath;
}


// Function to draw a BMP image on the e-paper display
bool drawBmp(const char *filename) {
  Serial.println("Drawing bitmap file: " + String(filename));
  fs::File bmpFS;
  bmpFS = SD.open(filename);  // Open requested file on SD card
  uint32_t seekOffset, headerSize, paletteSize = 0;
  int16_t row;
  uint8_t r, g, b;
  uint16_t bitDepth;
  uint16_t magic = read16(bmpFS);

  if (magic != ('B' | ('M' << 8))) {  // File not found or not a BMP
    Serial.println(F("BMP not found!"));
    bmpFS.close();
    return false;
  }

  read32(bmpFS);               // filesize in bytes
  read32(bmpFS);               // reserved
  seekOffset = read32(bmpFS);  // start of bitmap
  headerSize = read32(bmpFS);  // header size
  uint32_t w = read32(bmpFS);  // width
  uint32_t h = read32(bmpFS);  // height
  read16(bmpFS);               // color planes (must be 1)
  bitDepth = read16(bmpFS);

  // Check if the BMP is valid
  if (read32(bmpFS) != 0 || (bitDepth != 24 && bitDepth != 1 && bitDepth != 4 && bitDepth != 8)) {
    Serial.println(F("BMP format not recognized."));
    bmpFS.close();
    return false;
  }

  uint32_t palette[256];
  if (bitDepth <= 8)  // 1,4,8 bit bitmap: read color palette
  {
    read32(bmpFS);
    read32(bmpFS);
    read32(bmpFS);  // size, w resolution, h resolution
    paletteSize = read32(bmpFS);
    if (paletteSize == 0) paletteSize = bitDepth * bitDepth;  //if 0, size is 2^bitDepth
    bmpFS.seek(14 + headerSize);                              // start of color palette
    for (uint16_t i = 0; i < paletteSize; i++) {
      palette[i] = read32(bmpFS);
    }
  }

  // draw img that is shorter than display in the middle
  uint16_t x = (width() - w) / 2;
  uint16_t y = (height() - h) / 2;

  bmpFS.seek(seekOffset);

  uint32_t lineSize = ((bitDepth * w + 31) >> 5) * 4;
  uint8_t lineBuffer[lineSize];
  uint8_t nextLineBuffer[lineSize];

  epd.SendCommand(0x10);  // start data frame

  epd.EPD_7IN3F_Draw_Blank(y, width(), EPD_7IN3E_WHITE);  // fill area on top of pic white

  // row is decremented as the BMP image is drawn bottom up
  bmpFS.read(lineBuffer, sizeof(lineBuffer));
  //reverse linBuffer with the alorithm library
  std::reverse(lineBuffer, lineBuffer + sizeof(lineBuffer));

  float batteryVolts = readBattery();
  Serial.println("Battery voltage: " + String(batteryVolts) + "V");

  for (row = h - 1; row >= 0; row--) {
    epd.EPD_7IN3F_Draw_Blank(1, x, EPD_7IN3E_WHITE);  // fill area on the left of pic white

    if (row != 0) {
      bmpFS.read(nextLineBuffer, sizeof(nextLineBuffer));
      std::reverse(nextLineBuffer, nextLineBuffer + sizeof(nextLineBuffer));
    }
    uint8_t *bptr = lineBuffer;
    uint8_t *bnptr = nextLineBuffer;

    uint8_t output = 0;

    for (uint16_t col = 0; col < w; col++) {
      // Get r g b values for the next pixel
      if (bitDepth == 24) {
        r = *bptr++;
        g = *bptr++;
        b = *bptr++;
        bnptr += 3;
      } else {
        uint32_t c = 0;
        if (bitDepth == 8) {
          c = palette[*bptr++];
        } else if (bitDepth == 4) {
          c = palette[(*bptr >> ((col & 0x01) ? 0 : 4)) & 0x0F];
          if (col & 0x01) bptr++;
        } else {  // bitDepth == 1
          c = palette[(*bptr >> (7 - (col & 0x07))) & 0x01];
          if ((col & 0x07) == 0x07) bptr++;
        }
        b = c;
        g = c >> 8;
        r = c >> 16;
      }

      // Floyd-Steinberg dithering is used to dither the image
      uint8_t color;
      int indexColor;
      int errorR;
      int errorG;
      int errorB;

      indexColor = depalette(r, g, b);  // Get the index of the color in the colorPallete
      errorR = r - colorPallete[indexColor * 3 + 0];
      errorG = g - colorPallete[indexColor * 3 + 1];
      errorB = b - colorPallete[indexColor * 3 + 2];

      if (col < w - 1) {
        bptr[0] = constrain(bptr[0] + (7 * errorR / 16), 0, 255);
        bptr[1] = constrain(bptr[1] + (7 * errorG / 16), 0, 255);
        bptr[2] = constrain(bptr[2] + (7 * errorB / 16), 0, 255);
      }

      if (row > 0) {
        if (col > 0) {
          bnptr[-4] = constrain(bnptr[-4] + (3 * errorR / 16), 0, 255);
          bnptr[-5] = constrain(bnptr[-5] + (3 * errorG / 16), 0, 255);
          bnptr[-6] = constrain(bnptr[-6] + (3 * errorB / 16), 0, 255);
        }
        bnptr[-1] = constrain(bnptr[-1] + (5 * errorR / 16), 0, 255);
        bnptr[-2] = constrain(bnptr[-2] + (5 * errorG / 16), 0, 255);
        bnptr[-3] = constrain(bnptr[-3] + (5 * errorB / 16), 0, 255);

        if (col < w - 1) {
          bnptr[0] = constrain(bnptr[0] + (1 * errorR / 16), 0, 255);
          bnptr[1] = constrain(bnptr[1] + (1 * errorG / 16), 0, 255);
          bnptr[2] = constrain(bnptr[2] + (1 * errorB / 16), 0, 255);
        }
      }

      // Set the color based on the indexColor
      switch (indexColor) {
        case 0:
          color = EPD_7IN3E_BLACK;
          break;
        case 1:
          color = EPD_7IN3E_WHITE;
          break;
        case 2:
          color = EPD_7IN3E_YELLOW;
          break;
        case 3:
          color = EPD_7IN3E_RED;
          break;
        case 4:
          color = EPD_7IN3E_BLUE;
          break;
        case 5:
          color = EPD_7IN3E_GREEN;
          break;
      }

      if (batteryVolts <= 3.3 && col <= 50 && row >= h - 50) {
        color = EPD_7IN3E_RED;
        if (batteryVolts < 3.1) {
          Serial.println("Battery critically low, hibernating...");

          // Fix #5: Close the file handle before sleeping to avoid leaving the SD in a dirty state
          bmpFS.close();

          //switch off everything that might consume power
          esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF);
          //esp_sleep_pd_config(ESP_PD_DOMAIN_CPU, ESP_PD_OPTION_OFF);

          //disable all wakeup sources
          esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

          digitalWrite(2, LOW);
          esp_deep_sleep_start();

          Serial.println("This should never get printed");
          return false;
        }
      }

      // Fix #4: Removed dead 'buf_location' variable — it was computed every pixel but never used
      if (col & 0x01) {
        output |= color;
        epd.SendData(output);
      } else {
        output = color << 4;
      }
    }

    epd.EPD_7IN3F_Draw_Blank(1, x, EPD_7IN3E_WHITE);  // fill area on the right of pic white
    memcpy(lineBuffer, nextLineBuffer, sizeof(lineBuffer));
  }

  epd.EPD_7IN3F_Draw_Blank(y, width(), EPD_7IN3E_WHITE);  // fill area below the pic white

  //Rotate the unit if required
  bool isVert = (strstr(filename, "VERTICAL") != NULL);
  Serial.println("IS a Vertical  image:" + String(isVert));
  rotateDisplay(isVert);


  bmpFS.close();        // Close the file
  epd.TurnOnDisplay();  // Turn on the display
  return true;
}

// Function to depalette the image
int depalette(uint8_t r, uint8_t g, uint8_t b) {
  int p;
  int mindiff = 100000000;
  int bestc = 0;

  // Find the color in the colorPallete that is closest to the r g b values
  for (p = 0; p < sizeof(colorPallete) / 3; p++) {
    int diffr = ((int)r) - ((int)colorPallete[p * 3 + 0]);
    int diffg = ((int)g) - ((int)colorPallete[p * 3 + 1]);
    int diffb = ((int)b) - ((int)colorPallete[p * 3 + 2]);
    int diff = (diffr * diffr) + (diffg * diffg) + (diffb * diffb);
    if (diff < mindiff) {
      mindiff = diff;
      bestc = p;
    }
  }
  return bestc;
}