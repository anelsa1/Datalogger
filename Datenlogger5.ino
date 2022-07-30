#include <avr/sleep.h>
#include <ADS1X15.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <RTC8564.h>
#include <TimeLib.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>

//ADCs
ADS1115 ADS1(0x48); //inputs 1 bis 4
ADS1115 ADS2(0x49); //inputs 5 bis 8

int interrupt_pin = 2; //This pin on the atmega1284p works as interrupt pin; in standard layout this corresponds to pin 2

//time variables
tmElements_t te;  //Time elements structure
time_t alarmTimeUnix;
unsigned int year_rtc;
unsigned char month_rtc,day_rtc,hour_rtc,minute_rtc,second_rtc;

File datafile;

//configfile (setting) parameter
int check;
int interval; // interval in which data is recorded. unit: minutes
float gain1;  // gain of adc 1
float gain2;  // gain of adc 2
int NrInputs;
long in;      // position of occupied sensor inputs  
time_t unixTime; // "current" time specified by the user (time of first measurement)
int arraySize;   // minimum = NrInputs + 1(for the timestamp)

// other variables
long int* buffer_array = NULL;
int value; // raw ADC value
float value_volt; 
int counter = 0;
int* ADC1_Inputs = NULL;
int* ADC2_Inputs = NULL;
int maxNrInputs = 4; // number of inputs for each ADC (used for array size of ADCX_Inputs)
int length_ADC1_Inputs = 0;
int length_ADC2_Inputs = 0;

unsigned long myTime; 

int checkConfigFile(File configfile);
struct dateTime dt;

void setup() {
  Serial.begin(19200);
  pinMode(20, OUTPUT); // switch that powers the SD Card
  digitalWrite(20, LOW);

  // unused (floating) pins set LOW 
  int ouput_pins[] = {24,25,26,27,28,29,30,31,1,3,18,19,21,22,23,10,11,12,13,14,15};
  for(int i = 0; i< 21 ; i++){
    pinMode(ouput_pins[i], OUTPUT);
    digitalWrite(ouput_pins[i], LOW);
  }

  // ADCs setup
  ADS1.begin();
  ADS2.begin();
  ADS1.setMode(1);     // single shot mode 
  ADS2.setMode(1);     // single shot mode
  ADS1.setDataRate(7); // datarate 860SPS
  ADS2.setDataRate(7); // datarate 860SPS

  // initialisation SD Card
  // to enable SD Card: vcc high -> spi enable -> cs high
  digitalWrite(20, HIGH);
  SPCR |= 0B01000000;  // enable SPI
  pinMode(4, OUTPUT);  // CS PIN
  delay(100);
  
  if (!SD.begin(4)) //Pin 44 of uC is connected to the CS Pin of the SD Card, in standard Layout this correspondes to pin 4 
  {
    Serial.println("Initialization failed!");
    return; 
  }
  else
  {
    Serial.println("Initialization successful!");
  }

  // call function to check for configfile
  File configfile;
  if (checkConfigFile(configfile) == 1)
  {
    //if no setting.txt file exists these parameters will be used by default:
    interval = 5;
    gain1 = 1; // this corresponds to a FSR of +/- 4.096V
    gain2 = 1;
    ADS1.setGain(1);
    ADS2.setGain(1);
    NrInputs = 1;
    in = 1;
    unixTime = 946684800; //default time 01.01.2000 00:00:00 GMT
    arraySize = 9;
  }

  // create a data file (if it doesn't already exists)
  if (!SD.exists("data.csv"))
  {
    datafile = SD.open("data.csv", FILE_WRITE);
    Serial.println("opened file");
    datafile.print("Zeit");
    datafile.print(",");
    for(int i = 0; i < NrInputs; i++)
    {
      datafile.print("Messwert");
      
      if (i < NrInputs-1){
        datafile.print(i+1);
        datafile.print(",");
      }
      else
        datafile.println(i+1);
    }
    datafile.close();
    Serial.println("closed file");
    Serial.println("check point");
  }
  Serial.println("check point2");

  // to disable SD Card: cs low -> spi disable -> vcc down
  digitalWrite(4, LOW);
  SPCR &= 0B10111111;  //disable SPI
  digitalWrite(20, LOW);

  // set interrupt pin mode using the build-in pull-up resistor
  pinMode(interrupt_pin, INPUT_PULLUP);
  
  dt = {second(unixTime), minute(unixTime), hour(unixTime), day(unixTime), month(unixTime), year(unixTime), (weekday(unixTime)-1)}; 
  // note for the weekday: time library starts with sunday=1 while rtc library starts at sunday=0
  alarmTimeUnix = unixTime; 
    
  struct alarmTime at = {minute(alarmTimeUnix), hour(alarmTimeUnix), day(alarmTimeUnix), (weekday(alarmTimeUnix)-1)};
  
  RTC8564.begin(&dt);

  // create arrays with occupied input pins from ADC1 and ADC2 
  ADC1_Inputs = (int*) malloc (sizeof(int)*maxNrInputs);
  ADC2_Inputs = (int*) malloc (sizeof(int)*maxNrInputs);

  for(int i = 0; i < 8; i++) 
  {
    if(in & (1 << i)) 
    {
      if(i < 4){
        ADC1_Inputs[length_ADC1_Inputs] = i;
        length_ADC1_Inputs++;
      }
      else{
        ADC2_Inputs[length_ADC2_Inputs] = (i-4);
        length_ADC2_Inputs++;
      }
    }
  }
  
}

void loop() {
  setTimer();

  if(!buffer_array)
  {
    buffer_array = (long int*) malloc(sizeof(long int) * arraySize); 
  }
  
  time_t time_value_was_taken = alarmTimeUnix;
  time_value_was_taken = (long int) time_value_was_taken;

  // offset is the space inside the array that cannot be filled with 1 complete cycle (NrInputs + timestamp)
  int offset = arraySize % (NrInputs+1);

  if(counter % (NrInputs+1) == 0)
  {
    buffer_array[counter] = time_value_was_taken;
    counter = counter +1;
  }


  
  if(length_ADC1_Inputs > 0)
  {
    ADS1.requestADC(ADC1_Inputs[0]);
  }
  if(length_ADC2_Inputs > 0)
  {
    ADS2.requestADC(ADC2_Inputs[0]);
  }

  int i = 0;
  int j = 0;
  while(true)
  {
    if(length_ADC1_Inputs != 0)
    {
      if(ADS1.isReady())
      { 
        value = ADS1.getValue(); 
        ADS1.requestADC(ADC1_Inputs[i+1]);
        /*
        // converting raw adc value to mV 
        value_volt = ADS1.toVoltage(value);
        value_volt = (long int) (value_volt *1000);
        */
        value = (long int) (value);
        buffer_array[counter] = value;
        counter++;
        i++;
      }
    }
    if(length_ADC2_Inputs != 0)
    {
      if(ADS2.isReady())
      {
        value = ADS2.getValue();
        value_volt = ADS1.toVoltage(value);
        ADS2.requestADC(ADC2_Inputs[j+1]);
        value = (long int) value;
        buffer_array[counter] = value;
        counter++;
        j++;
      }
    } 
    if(counter % (NrInputs+1) == 0)
    {
      break;
    }
    
  }

/*
  // code to check duration of the different states of the microcontroller
  Serial.print("Time end ADC: ");
  myTime = millis();
  Serial.println(myTime);
*/
  
  if(counter == (arraySize - offset)) // check if buffer is completely filled
  {
    writeToSDCard(buffer_array, offset);
    counter = 0;
  }

  clearAlarmFlag();

  // disable onboard ADC before sleep 
  ADCSRA = 0;
  sleepMode();
}

void writeToSDCard(long int* buffer_array, int offset)
{

  digitalWrite(20, HIGH);
  SPCR |= 0B01000000;  //enable SPI
  digitalWrite(4, HIGH);

  /*
  Serial.print("Time init SD Card: ");
  myTime = millis();
  Serial.println(myTime);
  */

  //SD Card needs to be reinitialized 
  if (!SD.begin(4)) 
  {
    Serial.println("Initialization failed!"); 
  }
  else
  {
    Serial.println("Initialization successful!");
  }
  
  datafile = SD.open("data.csv", FILE_WRITE);

  /*
  Serial.print("Time start writing SD Card: ");
  myTime = millis();
  Serial.println(myTime);
  */
  
  for(int i = 0; i < (arraySize-offset) ; i++) //arraysize = sizeof buffer_array/4 Byte
  {
    if(i % (NrInputs+1) == 0)
    {
      //time value was taken: change Unix time in date format -> datastring char array
      time_t dateMeasurement = buffer_array[i];
      char dataString[14];
      year_rtc = year(dateMeasurement);
      month_rtc = month(dateMeasurement);
      day_rtc = day(dateMeasurement);
      hour_rtc = hour(dateMeasurement);
      minute_rtc = minute(dateMeasurement);
      second_rtc = second(dateMeasurement);
      sprintf(dataString, "%4d%02d%02d%02d%02d%02d", year_rtc, month_rtc, day_rtc, hour_rtc, minute_rtc, second_rtc);
      datafile.print(dataString);
      datafile.print(",");
    }
  else
  {
    if((i-NrInputs)%(NrInputs+1) == 0){
      datafile.println(buffer_array[i]);
    }  
    else{
      datafile.print(buffer_array[i]);
      datafile.print(",");
    }
  }
  }
  
  datafile.close();

  /*
  Serial.print("Time end writing SD Card: ");
  myTime = millis();
  Serial.println(myTime);
  */
  
  //disable sd: cs low -> spi disable -> vcc down
  digitalWrite(4, LOW);
  SPCR &= 0B10111111;  //disable SPI
  digitalWrite(20, LOW);
  
}

void setTimer()
{
  // calculate new alarm time
  alarmTimeUnix = alarmTimeUnix + interval*60; // unix time ist in s -> interval (min) = interval * 60 (s)
  struct alarmTime at = {minute(alarmTimeUnix), hour(alarmTimeUnix), day(alarmTimeUnix), (weekday(alarmTimeUnix)-1)};

  RTC8564.setAlarm(RTC8564_AE_ALL, &at, 1);
}

void clearAlarmFlag()
{
  if(RTC8564.getAlarmFlag()){
    RTC8564.clearAlarmFlag(); // Alarm only triggers if Alarm Falg is set OFF
  }
  else{
    Serial.println("Alarm Flag OFF");
  }
}

  
void sleepMode()
{ 
  sleep_enable(); 
  set_sleep_mode(SLEEP_MODE_PWR_DOWN); 
  attachInterrupt(digitalPinToInterrupt(2), wakeUpMode, LOW); //pin 42 on uC correspondes to pin 2 in standard layout 
 
  sleep_cpu(); 
}

void wakeUpMode()
{
  sleep_disable();
  detachInterrupt(digitalPinToInterrupt(2)); //pin 42 uC correspondes to pin 2 standard layout  
  /*
  Serial.print("Time wake up: ");
  myTime = millis();
  Serial.println(myTime);
  Serial.flush();
  */
}

int checkConfigFile(File configfile)
{
  configfile = SD.open("setting.txt");
  
  if(!configfile)
  {
    Serial.println("configfile cannot be opened");
    return 1;
  }
  
  int size_buffer = configfile.size();
  char file_buffer[size_buffer];
  configfile.read(file_buffer, size_buffer);

  Serial.print(file_buffer);

  StaticJsonDocument<100> doc;
  
  // Deserialization of the JSON document
  DeserializationError error = deserializeJson(doc, file_buffer);
  if (error)
  {
    Serial.println(("Failed to read file, using default configuration"));
    Serial.println(error.c_str());
  }

  // Copy values from the JsonDocument to the Config
  check = doc["cNr"] | 1;
  if(check != 7)
  {
    return 1;
  }
 
  interval = doc["timer"] ;
  
  gain1 = doc["gain1"];
  ADS1.setGain(gain1);
  gain2 = doc["gain2"];
  ADS2.setGain(gain2);
  // 0x gain   +/- 6.144V
  // 1x gain   +/- 4.096V  1 bit = 2mV    
  // 2x gain   +/- 2.048V  1 bit = 1mV
  // 4x gain   +/- 1.024V  1 bit = 0.5mV
  // 8x gain   +/- 0.512V  1 bit = 0.25mV  
  // 16x gain  +/- 0.256V  1 bit = 0.125mV

  
  NrInputs = doc["NrInputs"];
  in = doc["in"] ;
  unixTime = doc["time"];
  arraySize = doc["size"];

  configfile.close();
  return 0;  
}



