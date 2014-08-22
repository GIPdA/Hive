/**
 * @author Benjamin Balga & Cédric Chrétien - gipda.bb@gmail.com
 * @date   August, 2014
 * @brief  Hive scale logger
 *
 * Designed to log weight of 4 scale sensors at specific hours.
 */


#include <Wire.h>
#include <stdio.h>
#include <PCF8583.h>  // RTC
#include <SD.h>
#include <avr/sleep.h>
#include "rtc.h"


#define DBG(CODE) CODE

#define NUMBER_OF_MEASURES  4

#define SENSORS_AVERAGE_SIZE  200


File sdFile;         // SD
char cBuffer[10];    // Global buffer used by SD-related functions

// Log name : hiveLogFileName + number + hiveLogFileExtension
// The number is automatically incremented.
String hiveLogFileName = "hive";
String hiveLogFileExtension = ".txt";
char hiveLogFilenameBuffer[15];
unsigned char hiveLogIndex = 0;

#define CurrentHiveLogFilename()  hiveLogFilenameBuffer

// Weight sensors pins
const int sensorPin0 = 0;
const int sensorPin1 = 1;
const int sensorPin2 = 2;
const int sensorPin3 = 3;

const float scaleCoef = .2104428673;  // Scale coef in kg.
float fScaleOffsetRaw = 12.;          // Offset, can be updated with the Tare button. Saved in SD card.
float fScaleAvgComputedRaw = 0.;

const byte scaleTarePin = 3;
volatile boolean bScaleISRFlag = 0;

const int button1Pin = 8;  // Tare & datetime update button


// RTC
PCF8583 rtc(0xA0);
const int rtc_inputPin = 2;
char srtTimeBuf[25];


#define NUMBER_OF_ALARMS  3
// Alarms are set in loop, in the order below (first to last).
// TODO: set alarms from file
RTC_Time rtcAlarms[NUMBER_OF_ALARMS] = 
{
  // hour, minute, second
  {5,0,0},  // 5h
  {22,0,0}  // 22h
};

byte rtcCurrentAlarm = 0;
volatile boolean bAlarmISRFlag = 0;



void vRTC_updateDateTimeFromFile(char *filename)
{
  if (SD.exists(filename))
  {
    sdFile = SD.open(filename, FILE_READ);

    byte i = 0, index = 0;
    while (sdFile.available() && i < 6) {
      cBuffer[i] = sdFile.read();

      // Check for date or time (specific order : dd/mm/yyyy  hh:mm:ss)
      if (i > 0 && (cBuffer[i] == '/' || cBuffer[i] == ':' || cBuffer[i] == '\n' || cBuffer[i] == '-' || !sdFile.available())) {
        cBuffer[i] = '\0';
        unsigned int nb = atoi(cBuffer);

        switch (index) {
          case 0:
            rtc.day = nb;
            break;
          case 1:
            rtc.month = nb;
            break;
          case 2:
            rtc.year = nb;
            break;

          case 3:
            rtc.hour = nb;
            break;
          case 4:
            rtc.minute = nb;
            break;
          case 5:
            rtc.second = nb;
            break;

          default:
            break;
        }

        i = 0;
        ++index;
      } else {
        ++i;
      }
    }
    sdFile.close();

    rtc.set_time();
  }
}


/*!
 * @brief Setup function. Init inputs/outputs, serial port, SD card and RTC.
 * Update the offset value from 'offset.txt' file.
 * Update date and time from file if button 1 pressed at start-up. Hold the button 
 * then release it when you want to set the date and time (allows synchronisation).
 * Create the log file. The number is incremented automatically based on the other files.
 */
void setup()
{
  // Setup debug UART
  DBG( Serial.begin(9600);)
  pinMode(button1Pin, INPUT);
  pinMode(10, OUTPUT);  // SD card CS pin

  delay(2000);

  // Init RTC
  rtc.init();
  rtc.get_time();

  // Init SD card
  if (!SD.begin(10)) 
  {
    DBG( Serial.println("Error, SD init failed!");)
    delay(300);
    // Reset ?
    while(1) {
      sleepNow();
    }
    return;
  }


  // Set date & time from file if button pressed
  if (digitalRead(button1Pin) == 0) {
    while (!digitalRead(button1Pin)); // Wait for release (allow synchronisation)
    // Update date and time from file
    DBG( Serial.println("Updating date & time...");)
    vRTC_updateDateTimeFromFile("datetime.txt");
    bScaleISRFlag = 0;
    delay(300);
  }
  

  // Get weight offset if file exists
  if (SD.exists("offset.txt"))
  {
    DBG( Serial.println("Using offset...");)

    sdFile = SD.open("offset.txt", FILE_READ);
    byte i = 0;
    while (sdFile.available() && i < 5) {
      cBuffer[i++] = sdFile.read();
    }
    cBuffer[i] = '\0';
    sdFile.close();

    fScaleOffsetRaw = atof(cBuffer);

    DBG( Serial.print("Offset: ");)
    DBG( Serial.println(fScaleOffsetRaw);)
  }


  // Find first usable log number (not used)
  vComputeHiveLogFilename();
  while (SD.exists(CurrentHiveLogFilename()) && hiveLogIndex < 250) {
    ++hiveLogIndex;
    vComputeHiveLogFilename();
  }

  if (hiveLogIndex >= 250) {
    DBG( Serial.println("Too much logs, please empty the SD card.");)
    delay(300);
    // Reset ?
    while(1) {
      sleepNow();
    }
  }


  DBG( Serial.print("Using filename ");)
  DBG( Serial.println(CurrentHiveLogFilename());)

  // Create the log file
  sdFile = SD.open(CurrentHiveLogFilename(), FILE_WRITE);

  if (!sdFile) {
    // Error, can't create/open file
    sdFile.close();
    DBG( Serial.println("Error, unable to create hive log!");)
    delay(300);
    // Reset ?
    while(1) {
      sleepNow();
    }
  }
  sdFile.close();

  DBG( Serial.println("Log file created, logging now...");)

  // Setup alarm input and IT on falling edge.
  // Alarm is open-drain -> enable pull-up.
  pinMode(rtc_inputPin, INPUT_PULLUP);
  attachInterrupt(0, vRTC_AlarmISR, FALLING);

  pinMode(scaleTarePin, INPUT);
  attachInterrupt(1, vScale_TareISR, FALLING);

  // Start logging
  //updateAlarm();
}


/*!
 * @brief Compute the hive log filename using base name, current file number and extension.
 */
void vComputeHiveLogFilename()
{
  String filename = hiveLogFileName + String(hiveLogIndex) + hiveLogFileExtension;
  filename.toCharArray(hiveLogFilenameBuffer, min(filename.length()+1, 15));
}


/*!
 * @brief Alarm interrupt ISR
 */
void vRTC_AlarmISR()
{
  bAlarmISRFlag = 1;
}

/*!
 * @brief Button interrupt ISR
 */
void vScale_TareISR()
{
  bScaleISRFlag = 1;
}


/*!
 * @brief Set time and date in the RTC
 */
void vRTC_setTimeAndDate(byte second, byte minute, byte hour,
                         byte day, byte month, byte year)
{
  rtc.hour = hour;
  rtc.minute = minute;
  rtc.second = second;
  rtc.year = year;
  rtc.month = month;
  rtc.day = day;
  rtc.set_time();
}

/*!
 * @brief Setup the daily alarm of the RTC. An active low interrupt will be triggered.
 */
void vRTC_setDailyAlarm(byte second, byte minute, byte hour)
{
  rtc.alarm_second = second;
  rtc.alarm_minute = minute;
  rtc.alarm_hour = hour;
  rtc.set_daily_alarm();
}


/*!
 * @brief Setup the daily alarm of the RTC. An active low interrupt will be triggered.
 */
void vRTC_setDailyAlarm(RTC_Time &time)
{
  rtc.alarm_second = time.second;
  rtc.alarm_minute = time.minute;
  rtc.alarm_hour = time.hour;
  rtc.set_daily_alarm();
}


/*!
 * @brief Format date and time in dd/mm/yy hh:mm:ss manner. Leading '\0' included.
 */
char *pcRTC_formatDateAndTime()
{
  rtc.get_time();

  sprintf(srtTimeBuf, "%02d/%02d/%02d %02d:%02d:%02d", rtc.day, rtc.month, rtc.year, rtc.hour, rtc.minute, rtc.second);

  return srtTimeBuf;
}

/*!
 * @brief Format date and time in dd/mm/yy hh:mm:ss manner. Leading '\0' included.
 */
char *pcRTC_formatDate()
{
  rtc.get_time();
  sprintf(srtTimeBuf, "%02d.%02d.%02d", rtc.day, rtc.month, rtc.year);
  return srtTimeBuf;
}


/*!
 * @brief Save the current weight offset in the SD card for reuse after reset.
 */
void vScale_saveOffset()
{
  if (SD.exists("offset.txt"))
  {
    DBG( Serial.print("Removing offset.txt...");)
    if (SD.remove("offset.txt")) {
      DBG( Serial.println(" success.");)
    } else {
      DBG( Serial.println(" failed!");)
      return;
    }
  }


  sdFile = SD.open("offset.txt", FILE_WRITE);

  if (sdFile) {
    sdFile.print(fScaleOffsetRaw);
  } else {
    // Error, can't create/open file
    DBG( Serial.println("Error, unable to create offset.txt!");)
  }
  sdFile.close();

  DBG( Serial.println("Scale offset updated.");)
}


/*!
 * @brief Calcule la somme de tous les capteurs.
 * @return Somme de la valeur mesurée des 4 capteurs.
 */
unsigned int u16Scale_readSensors()
{
  unsigned int value = 0;
  value  = analogRead(sensorPin0);
  value += analogRead(sensorPin1);
  value += analogRead(sensorPin2);
  value += analogRead(sensorPin3);

  return value;
}

/*!
 * @brief Calcule une moyenne de la somme des capteurs.
 */
float fScale_computeRaw(unsigned char size)
{
  unsigned long avgSum = 0;
  unsigned char avgCount = 0;

  // Compute average
  for (unsigned char i = size; i; --i) {
    avgSum += u16Scale_readSensors();
    ++avgCount;
  }

  return (float)avgSum / (float)avgCount;
}


/*!
 * @brief Compute weight over 10 measurements on 3 seconds.
 */
float fScale_computeScaleWeight()
{
  float avgSum = 0;
  unsigned char avgCount = 0;

  // Do a 3-sec, 10-points averaged measure
  for (unsigned char i = 10; i; --i) {
    avgSum += fScale_computeRaw(SENSORS_AVERAGE_SIZE);
    ++avgCount;

    delay(300);
  }

  fScaleAvgComputedRaw = avgSum / (float)avgCount;

  return (fScaleAvgComputedRaw-fScaleOffsetRaw)*scaleCoef;
}


/*!
 * @brief Put µC into deep sleep. Wake-up by pin interrupt only.
 */
void sleepNow()
{
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  sleep_mode();
  // wake up here
  sleep_disable();
}


/*!
 * @brief Update the RTC's alarm.
 */
void updateAlarm()
{
  rtc.get_time();

  rtc.alarm_second = rtcAlarms[rtcCurrentAlarm].second;
  rtc.alarm_minute = rtcAlarms[rtcCurrentAlarm].minute;
  rtc.alarm_hour = rtcAlarms[rtcCurrentAlarm].hour;

  // Next alarm
  if (++rtcCurrentAlarm >= NUMBER_OF_ALARMS) {
    rtcCurrentAlarm = 0;
  }

  rtc.set_daily_alarm();
}


/*!
 * @brief Main loop. Measure weight, write down in the log file, and sleep.
 * Wake-up with RTC's alarm or Tare button.
 */
void loop()
{
  static byte u8MeasurementsCounter = NUMBER_OF_MEASURES;

  // Do a measurement
  float w = fScale_computeScaleWeight();

  // Update offset with current measurement if flag set
  if (bScaleISRFlag) {
    fScaleOffsetRaw = fScaleAvgComputedRaw;
    
    // Save offset on the SD card
    vScale_saveOffset();

    DBG( Serial.print("TARE - ");)
    DBG( Serial.print(pcRTC_formatDateAndTime());)
    DBG( Serial.print("  ");)
    DBG( Serial.println(fScaleOffsetRaw);)

    w = fScale_computeScaleWeight();
  }

  // Debug
  DBG( Serial.print(pcRTC_formatDateAndTime());)
  DBG( Serial.print("  ");)
  DBG( Serial.print(w);)
  DBG( Serial.print("  ");)
  DBG( Serial.println(fScaleAvgComputedRaw);)

  // Log weight
  sdFile = SD.open(CurrentHiveLogFilename(), FILE_WRITE);

  if (sdFile) {
    if (bScaleISRFlag) {
      sdFile.print("TARE - ");
      sdFile.print(pcRTC_formatDateAndTime());
      sdFile.print("  ");
      sdFile.println(fScaleOffsetRaw);
    }
    sdFile.print(pcRTC_formatDateAndTime());
    sdFile.print("  ");
    sdFile.print(w);
    sdFile.print("  ");
    sdFile.println(fScaleAvgComputedRaw);
  } else {
    // If the file didn't open, print an error:
    DBG( Serial.print("Error, logging failed at ");)
    DBG( Serial.println(pcRTC_formatDateAndTime());)
  }
  sdFile.close();

  bScaleISRFlag = 0;

  if (--u8MeasurementsCounter <= 0) {
    // Update alarm and sleep for it.
    u8MeasurementsCounter = NUMBER_OF_MEASURES;
    if (bAlarmISRFlag) {
      updateAlarm();
      bAlarmISRFlag = 0;
    }
    delay(300);

    sleepNow();
  }
}


