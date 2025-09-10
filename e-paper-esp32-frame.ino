#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include "epd7in3f.h"
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include "esp_adc_cal.h"
#include "driver/adc.h"
#include <ESP32Servo.h>
#include <set>
#include <map>
#include <ArduinoJson.h>


//This is the pin for the transistor that powers the external components
#define TRANSISTOR_PIN 26
#define WAKEUP_PIN 27
#define SERVO_PIN 12
#define SERVO_TRANS_PIN 04


Preferences preferences;

const size_t JSON_DOC_SIZE = 4096;

Epd epd;
unsigned long delta;                 // Variable to store the time it took to update the display for deep sleep calculations
unsigned long deltaSinceTimeObtain;  // Variable to store the time it took to update the display since the time was obtained for deep sleep calculations

#define SD_CS_PIN 22

uint16_t width() {
  return EPD_WIDTH;
}
uint16_t height() {
  return EPD_HEIGHT;
}

SPIClass vspi(VSPI);  // VSPI for SD card

// Color pallete for dithering. These are specific to the 7in3e waveshare display.
uint8_t colorPallete[6 * 3] = {
  0, 0, 0,
  255, 255, 255,
  255, 255, 0,
  255, 0, 0,
  0, 0, 255,
  0, 255, 0
};

uint16_t read16(fs::File &f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read();  // LSB
  ((uint8_t *)&result)[1] = f.read();  // MSB
  return result;
}

uint32_t read32(fs::File &f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read();  // LSB
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read();  // MSB
  return result;
}


Servo servo;
bool rotateDisplay(bool isVert) {

  // Turn on the transistor to power the servo
  pinMode(SERVO_TRANS_PIN, OUTPUT);
  digitalWrite(SERVO_TRANS_PIN, HIGH);
  delay(100);

  servo.setPeriodHertz(50);           // PWM frequency for SG90
  servo.attach(SERVO_PIN, 500, 2400);  // Minimum and maximum pulse width (in µs) to go from 0° to 180
  int speedDelay = 15;

  if (!preferences.isKey("orientation")) {
    preferences.putString("orientation", "H");
  }
  Serial.println("Orientation Start: " + preferences.getString("orientation"));
  if (preferences.getString("orientation") == "V" && !isVert) {
    Serial.println("Rotating to Horizontal");
    for (int pos = 0; pos <= 92; pos += 1) {
      servo.write(pos);
      Serial.println("rotate" + pos);
      delay(speedDelay);
    }
    preferences.putString("orientation", "H");
  } else if (preferences.getString("orientation") == "H" && isVert) {
    Serial.println("Rotating to Vertical");
    for (int pos = 92; pos >= 0; pos -= 1) {
      servo.write(pos);
      Serial.println("rotate" + pos);
      delay(speedDelay);
    }
    preferences.putString("orientation", "V");
  }

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



struct DataRow {
  String name;  // The string element
  int count;    // Number of integer values in this row
};

// Pointer to store the dynamically allocated array of DataRow structs
DataRow *fileStringDataSet = nullptr;  // Initialize to nullptr


void setup() {
  Serial.begin(115200);
  delta = millis();

  preferences.begin("e-paper", false);

  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from deep sleep due to timer.");
  } else {
    Serial.println("Did not wake up from deep sleep.");
  }

  // Turn on the transistor to power the external components
  pinMode(TRANSISTOR_PIN, OUTPUT);
  digitalWrite(TRANSISTOR_PIN, HIGH);
  delay(100);

  // Initialize the SD card
  while (!SD.begin(SD_CS_PIN, vspi)) {
    Serial.println("Card Mount Failed");
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

  checkSDFiles();  //Check if the SD files have changed and update the preferences if needed

  String file = getNextFile();  // Get the next file to display

  drawBmp(file.c_str());  // Display the file

  digitalWrite(TRANSISTOR_PIN, LOW);  // Turn off external components
  preferences.end();
}

void loop() {
  hibernate();
}

void hibernate() {
  Serial.println("start sleep");
  //esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(WAKEUP_PIN),1);
  esp_deep_sleep(static_cast<uint64_t>(86400) * 1e6);
}

// Function to check if the SD files have changed and update the preferences if needed
void checkSDFiles() {
  Serial.println("Checking for changes to .bmp files on the SD card...");
  
  // Step 1: Scan the SD card for the current list of .bmp files.
  std::vector<String> newBmpFiles;
  File root = SD.open("/");
  if (!root) {
      Serial.println("Error: Failed to open SD card root directory.");
      return;
  }
  
  while (true) {
    File entry = root.openNextFile();
    if (!entry) {
      // No more files, so we exit the loop.
      break;
    }
    String name = entry.name();
    // Check if the file has a '.bmp' extension (case-insensitive)
    if (name.length() >= 4 && name.substring(name.length() - 4).equalsIgnoreCase(".bmp")) {
      newBmpFiles.push_back(name);
    }
    entry.close();
  }
  root.close();

  // Sort the new list to ensure consistent ordering.
  std::sort(newBmpFiles.begin(), newBmpFiles.end());

  // Begin a Preferences session to read the last known state
  preferences.begin("my-app", false);
  
  // Step 2: Retrieve the old list of files from Preferences.
  // The list is stored as a JSON array string for easy storage and retrieval.
  std::vector<String> oldBmpFiles;
  String oldFileListJson = preferences.getString("fileList", "[]");
  
  // Use ArduinoJson to deserialize the string back into a list.
  DynamicJsonDocument doc(JSON_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, oldFileListJson);

  if (!error) {
    // If deserialization is successful, populate the oldBmpFiles vector.
    JsonArray fileArray = doc.as<JsonArray>();
    for (JsonVariant v : fileArray) {
      oldBmpFiles.push_back(v.as<String>());
    }
  }
  
  // Close the Preferences session before doing comparisons to avoid conflicts
  preferences.end();

  // Step 3: Compare the old and new lists to find additions and removals.
  // We use std::set for fast lookups.
  std::set<String> newSet(newBmpFiles.begin(), newBmpFiles.end());
  std::set<String> oldSet(oldBmpFiles.begin(), oldBmpFiles.end());

  bool changed = false;

  // Find files that were added
  for (const auto& file : newSet) {
    if (oldSet.find(file) == oldSet.end()) {
      Serial.println("Added: " + file);
      changed = true;
    }
  }

  // Find files that were removed
  for (const auto& file : oldSet) {
    if (newSet.find(file) == newSet.end()) {
      Serial.println("Removed: " + file);
      changed = true;
    }
  }

  // We only proceed if a change was detected.
  if (changed) {
    Serial.println("Change detected. Updating fileString.txt.");

    // Step 4: Read the existing fileString.txt to preserve existing values.
    std::map<String, int> existingFileValues;
    File existingFileString = SD.open("/fileString.txt", FILE_READ);
    if (existingFileString) {
      String content = existingFileString.readString();
      existingFileString.close();
      
      // Parse the content to populate our map
      int start = 0;
      int end = content.indexOf(',');
      while (end != -1) {
        String pair = content.substring(start, end);
        int separator = pair.indexOf('|');
        if (separator != -1) {
          String fileName = pair.substring(0, separator);
          int value = pair.substring(separator + 1).toInt();
          existingFileValues[fileName] = value;
        }
        start = end + 1;
        end = content.indexOf(',', start);
      }
    }
    
    // Step 5: Rebuild the fileString.txt content from the new list, retaining old values.
    String fileString = "";
    for (const auto& file : newBmpFiles) {
      fileString += file;
      if (existingFileValues.count(file)) {
        // Use the old value from the map
        fileString += "|" + String(existingFileValues[file]) + ",";
      } else {
        // It's a new file, so use 0
        fileString += "|0,";
      }
    }

    // Step 6: Update Preferences with the new state for the next run.
    preferences.begin("my-app", false);
    
    // Serialize the new list of files and store it in Preferences
    doc.clear();
    JsonArray newArray = doc.to<JsonArray>();
    for (const auto& file : newBmpFiles) {
      newArray.add(file);
    }
    String newFileListJson;
    serializeJson(doc, newFileListJson);
    preferences.putString("fileList", newFileListJson);
    preferences.end();
    
    // Step 7: Write the final, updated file string to the SD card.
    File file = SD.open("/fileString.txt", FILE_WRITE);
    if (!file) {
      Serial.println("Failed to open file for writing.");
      return;
    }
    file.print(fileString);
    file.close();

    Serial.println("Successfully updated fileString.txt.");
  } else {
    Serial.println("No change detected. No update required.");
  }
}



// Function to get the next file to display
String getNextFile() {
  // Read fileString from txt file into an array

  File file = SD.open("/fileString.txt");
  if (!file) {
    Serial.println("Failed to open file for reading");
    return "";
  }
  int fileStringIndex = 0;  //fileString index
  int numberRecords = 1000;

  fileStringDataSet = new DataRow[numberRecords];

  Serial.println("Starting to parse fileString.txt");
  String fileString = "";
  while (file.available()) {
    char character = file.read();
    //Serial.println("Current character: " + character);
    if (static_cast<char>(character) == ',') {
      //Serial.println(fileString);
      fileStringDataSet[fileStringIndex].name = fileString.substring(0, fileString.indexOf("|"));
      fileStringDataSet[fileStringIndex].count = fileString.substring(fileString.indexOf("|") + 1).toInt();
      //Serial.println(fileString.substring(fileString.indexOf("|") + 1).toInt());
      //Serial.println(fileStringDataSet[fileStringIndex].count);

      fileString = "";
      fileStringIndex++;
    } else {
      fileString += static_cast<char>(character);
      //Serial.print("Current fileString: ");
    }
  }
  file.close();


  //get the lowest count value
  int lowestCount = 99999;

  Serial.print("Number of files: ");
  Serial.println(fileStringIndex);
  for (int i = 0; i < fileStringIndex; i++) {
    if (fileStringDataSet[i].count < lowestCount) {
      lowestCount = fileStringDataSet[i].count;
      Serial.print("New lowestCount: ");
      Serial.println(lowestCount);
    }
  }

  std::vector<String> lowCountFiles;
  for (int i = 0; i < fileStringIndex; i++) {
    if (fileStringDataSet[i].count == lowestCount) {
      lowCountFiles.push_back(fileStringDataSet[i].name);
      //Serial.println("File found that matches the lowestCount: " + fileStringDataSet[i].name);
    }
  }
  std::sort(lowCountFiles.begin(), lowCountFiles.end());

  int imageIndex = random(lowCountFiles.size());
  //Serial.println("Image Index selected: " + imageIndex);
  //Serial.println("Taken from an array of size: " + arraySize);
  String nextFile = lowCountFiles[imageIndex];


  //Update the fileString to include the new count for the image
  //search through fileStringDataSet for a matching name
  fileString = "";
  for (int i = 0; i < fileStringIndex; i++) {
    fileString += fileStringDataSet[i].name + "|";
    if (fileStringDataSet[i].name == nextFile) {
      fileStringDataSet[i].count++;  //increment the count
      Serial.print("Count incremented for: ");
      Serial.print(fileStringDataSet[i].name);
      Serial.print(" to ");
      Serial.println(fileStringDataSet[i].count);
    }
    Serial.println(fileStringDataSet[i].name);
    Serial.println(fileStringDataSet[i].count);
    fileString += String(fileStringDataSet[i].count) + ",";
  }


  Serial.println(fileString);
  //save everything back to fileString.txt
  SD.remove("/fileString.txt");
  file = SD.open("/fileString.txt", FILE_WRITE);
  file.print(fileString);
  file.close();
  Serial.println("New Filestring.txt written");

  return "/" + nextFile;
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

      // Vodoo magic i don't understand
      uint32_t buf_location = (row * (width() / 2) + col / 2);
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