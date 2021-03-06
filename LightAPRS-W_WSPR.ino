// TODO values: BattMin, GpsMinVolt, WsprBattMin
// switch off Dev_Mode at Balloon start!
#include <Arduino.h>
#include <SoftwareSerial.h>
#include <avr/wdt.h>
#include <TinyGPS++.h>      //https://github.com/mikalhart/TinyGPSPlus
#include <LowPower.h>       //https://github.com/rocketscream/Low-Power
#include <si5351.h>         //https://github.com/etherkit/Si5351Arduino
#include <JTEncode.h>       //https://github.com/etherkit/JTEncode (JT65/JT9/JT4/FT8/WSPR/FSQ Encoder Library)
#include <TimeLib.h>        //https://github.com/PaulStoffregen/Time
#include <TimerThree.h>     //https://github.com/PaulStoffregen/TimerThree/

#define TCXO_PIN    0
#define BattPin     A2
#define GpsVccPin   18

#define ADC_REFERENCE REF_3V3

#define TCXO_ON          digitalWrite(TCXO_PIN, LOW)     //P-Mosfet
#define TCXO_OFF         digitalWrite(TCXO_PIN, HIGH)
#define GpsON            digitalWrite(GpsVccPin, LOW)    //P-Mosfet
#define GpsOFF           digitalWrite(GpsVccPin, HIGH)

// #define DEVMODE // THAT affects the timing!!! Development mode. Uncomment to enable for debugging.
// #define RAW_GPS // THAT affects the timing!!! RAW GPS datas. Uncomment to enable for debugging.

uint16_t  BeaconWait = 50;        // seconds sleep for next beacon (HF or VHF). This is optimized value, do not change this if possible.
uint16_t  BattWait = 60;          // seconds sleep if super capacitors/batteries are below BattMin (important if power source is solar panel)
float     BattMin = 0.1;          // min Volts to wake up.
float     GpsMinVolt = 0.2;       // min Volts for GPS to wake up. (important if power source is solar panel)
float     WsprBattMin = 0.2;      // min Volts for HF mradio module to transmit (TX) ~10 mW

//******************************  Sleep Settings *************************************

int       sleep_TCXO = 6;         // seconds to level off the TCXO after turning on
int       sleep_between = 8;      // break in seconds between normal WSPR and Tele-WSPR transmission to begin at second 0 again
int       fine_sleep = 600;       // fine-tuning in milliseconds
int       timeout_GPS_reset = 20; // switch GPS off for x seconds to reset it

//******************************  HF CONFIG *************************************

char hf_callsign[7] = "NONAME";   //DO NOT FORGET TO CHANGE YOUR CALLSIGN
char tele_prefix[4] = "AB0";      //DO NOT FORGET TO CHANGE THE TELEMETRIE PREFIX CALLSIGN

//#define WSPR_DEFAULT_FREQ       10140200UL //30m band
#define WSPR_DEFAULT_FREQ       14097130UL //20m band
//#define WSPR_DEFAULT_FREQ       18106100UL //17M band
//#define WSPR_DEFAULT_FREQ       21096100UL //15m band
//#define WSPR_DEFAULT_FREQ       24926100UL //12M band
//#define WSPR_DEFAULT_FREQ       28126100UL //10m band
//for all bands -> http://wsprnet.org/drupal/node/7352


// Supported modes, default HF mode is WSPR
enum mode {MODE_JT9, MODE_JT65, MODE_JT4, MODE_WSPR, MODE_FSQ_2, MODE_FSQ_3,
           MODE_FSQ_4_5, MODE_FSQ_6, MODE_FT8
          };

enum mode cur_mode = MODE_WSPR; //default HF mode

//supported other modes freq for 20m
#define JT9_DEFAULT_FREQ        14078700UL
#define FT8_DEFAULT_FREQ        14075500UL
#define JT65_DEFAULT_FREQ       14078300UL
#define JT4_DEFAULT_FREQ        14078500UL
#define FSQ_DEFAULT_FREQ        7105350UL     // Base freq is 1350 Hz higher than dial freq in USB

//*******************************************************************************

boolean  aliveStatus = true; //for tx status message on first wake-up just once.

//******************************  HF SETTINGS   *********************************

#define JT9_TONE_SPACING        174          // ~1.74 Hz
#define JT65_TONE_SPACING       269          // ~2.69 Hz
#define JT4_TONE_SPACING        437          // ~4.37 Hz
#define WSPR_TONE_SPACING       146          // ~1.46 Hz
#define FSQ_TONE_SPACING        879          // ~8.79 Hz
#define FT8_TONE_SPACING        625          // ~6.25 Hz

#define JT9_DELAY               576          // Delay value for JT9-1
#define JT65_DELAY              371          // Delay in ms for JT65A
#define JT4_DELAY               229          // Delay value for JT4A
#define WSPR_DELAY              683          // Delay value for WSPR
#define FSQ_2_DELAY             500          // Delay value for 2 baud FSQ
#define FSQ_3_DELAY             333          // Delay value for 3 baud FSQ
#define FSQ_4_5_DELAY           222          // Delay value for 4.5 baud FSQ
#define FSQ_6_DELAY             167          // Delay value for 6 baud FSQ
#define FT8_DELAY               159          // Delay value for FT8

#define CORRECTION              680         // Change this for your ref osc
// #define CORRECTION              -5700         // Change this for your ref osc

// Global variables
unsigned long hf_freq;
char hf_call[7] = "NOCALL";                  //You don't have to change this.
char hf_message[13] = "NOCALL AA00";         //for non WSPR modes, you don't have to change this, updated by hf_call and GPS location
char hf_loc_2_last_digits[] = "AA";
char hf_loc_4_digits[] = "AA00";             //for WSPR, updated by GPS location. You don't have to change this.
char hf_loc_6_digits[] = "AA00AA";           //for WSPR, updated by GPS location. You don't have to change this.
uint8_t dbm = -10;
int val_alt = 0;
char* val_char_alt;
uint8_t tx_buffer[255];
uint8_t symbol_count;
uint16_t tone_delay, tone_spacing;
volatile bool proceed = false;

//******************************  GPS SETTINGS   *********************************

// int16_t GpsResetTime = 300;                    // timeout for reset if GPS is not fixed
int16_t GpsResetTime = 600;                    // timeout for reset if GPS is not fixed
boolean GpsFirstFix = false;                   // do not change this
boolean ublox_high_alt_mode_enabled = false;   // do not change this
int16_t GpsInvalidTime = 0;                    // do not change this
int16_t gps_sats_value = 0;                    // do not change this
int16_t gps_seconds = 0;                       // do not change this

//********************************************************************************

Si5351 si5351(0x60);
TinyGPSPlus gps;
JTEncode jtencode;

void setup() {
  wdt_enable(WDTO_8S);
  analogReference(INTERNAL2V56);
  pinMode(BattPin, INPUT);
  pinMode(TCXO_PIN, OUTPUT);
  pinMode(GpsVccPin, OUTPUT);

  TCXO_OFF;
  GpsON;

  Serial.begin(57600); // Arduino serial
  Serial1.begin(9600); // GPS serial (RS41)
// Serial1.begin(38400); // GPS serial (NEO-6M)
  
//  delay(100);
//  setGPS_continuous_mode();
//  delay(100);
}

void loop() {
  wdt_reset();

  if (readBatt() > BattMin) {
    if (aliveStatus) {
	
#if defined(DEVMODE)
      Serial.println(F("Loop starting....."));
#endif

      aliveStatus = false;

      while (readBatt() < BattMin) {
        sleepSeconds(BattWait);
      }

    }

    updateGpsData(1000);
	  gps_sats_value = gps.satellites.value();
	  gps_seconds = second();
    gpsDebug();

    if (gps.location.isValid() && gps.location.age() < 1000) {
      GpsInvalidTime = 0;
    } else {
      GpsInvalidTime++;
      if (GpsInvalidTime > GpsResetTime) {
        GpsOFF;
        ublox_high_alt_mode_enabled = false; //gps sleep mode resets high altitude mode.
        wdt_reset();
        sleepSeconds(timeout_GPS_reset);
        GpsON;
//        delay(100);
//        setGPS_continuous_mode();
//        delay(100);
        GpsInvalidTime=0; 
      }
    }

    if ((gps.location.age() < 1000 || gps.location.isUpdated()) && gps.location.isValid()) {
      if (gps.satellites.isValid() && gps.satellites.value() >= 3) {
        GpsFirstFix = true;
#if defined(DEVMODE)
        Serial.println(F("3D FIX confirmed"));
#endif

        if (readBatt() > WsprBattMin && gps_sats_value >= 3 && timeStatus() == timeSet && ((minute() == 01) || (minute() == 11) || (minute() == 21) || (minute() == 31) || (minute() == 41) || (minute() == 51) ) && gps_seconds == 55 ) {

		      // first WSPR Transmission
		      cur_mode = MODE_WSPR;
          GridLocator_4_digits(hf_loc_4_digits);
          sprintf(hf_call, "%s", hf_callsign);

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode WSPR Preparing"));
          Serial.println(hf_loc_4_digits);
          Serial.println(hf_call);
#endif
          wdt_reset();
          
		      GpsOFF;
//          setGPS_powersave();
          delay(50);
          TCXO_ON;
          sleepSeconds(sleep_TCXO);

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode WSPR Sending"));
#endif

          encode();

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode WSPR Sent"));
#endif

          sleepSeconds(sleep_between);
          delay(fine_sleep);

          // WSPR Telemetrie Data Transmission
		      cur_mode = MODE_WSPR;
		      GridLocator_2_last_digits(hf_loc_2_last_digits);
		      GridLocator_4_digits(hf_loc_4_digits);
          sprintf(hf_call, "%s%s%s", tele_prefix, val_char_alt, hf_loc_2_last_digits);

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode TELE-WSPR Preparing"));
          Serial.println(hf_loc_4_digits);
          Serial.println(hf_call);
#endif
          wdt_reset();

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode TELE-WSPR Sending"));
#endif
          encode();

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode TELE-WSPR Sent"));
#endif

          sleepSeconds(sleep_between);
          delay(fine_sleep);
		  
		      // second WSPR Transmission 
		      cur_mode = MODE_WSPR;
          GridLocator_4_digits(hf_loc_4_digits);
          sprintf(hf_call, "%s", hf_callsign);

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode second WSPR Preparing"));
          Serial.println(hf_loc_4_digits);
          Serial.println(hf_call);
#endif
          wdt_reset();

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode second WSPR Sending"));
#endif

          encode();

#if defined(DEVMODE)
          Serial.println(F("Digital HF Mode second WSPR Sent"));
#endif		  

          TCXO_OFF;
		      GpsON;
//		      setGPS_continuous_mode();
		      delay(50);
          ublox_high_alt_mode_enabled = false;

        }
		    else {
          updatePosition();
          Serial.flush();
          //If two minutes time slot is close, sleep less than default.
          if (timeStatus() == timeSet && ((minute() % 10 == 2) || (minute() % 10 == 6))) {
//            sleepSeconds(60 - second());
          } else {
//            sleepSeconds(BeaconWait);
          }

        }

      } else {
#if defined(DEVMODE)
        Serial.println(F("Not enough satellites"));
#endif
      }
    }
  } else {
    sleepSeconds(BattWait);
  }

}


void sleepSeconds(int sec) {

#if defined(DEVMODE)
  Serial.flush();
#endif
  wdt_disable();
  for (int i = 0; i < sec; i++) {

    LowPower.powerDown(SLEEP_1S, ADC_OFF, BOD_ON);
  }
  wdt_enable(WDTO_8S);
  wdt_reset();
}

void updatePosition() {
  // Convert and set latitude NMEA string Degree Minute Hundreths of minutes ddmm.hh[S,N].
  char latStr[10];
  int temp = 0;

  double d_lat = gps.location.lat();
  double dm_lat = 0.0;

  if (d_lat < 0.0) {
    temp = -(int)d_lat;
    dm_lat = temp * 100.0 - (d_lat + temp) * 60.0;
  } else {
    temp = (int)d_lat;
    dm_lat = temp * 100 + (d_lat - temp) * 60.0;
  }

  dtostrf(dm_lat, 7, 2, latStr);

  if (dm_lat < 1000) {
    latStr[0] = '0';
  }

  if (d_lat >= 0.0) {
    latStr[7] = 'N';
  } else {
    latStr[7] = 'S';
  }


  // Convert and set longitude NMEA string Degree Minute Hundreths of minutes ddmm.hh[E,W].
  char lonStr[10];
  double d_lon = gps.location.lng();
  double dm_lon = 0.0;

  if (d_lon < 0.0) {
    temp = -(int)d_lon;
    dm_lon = temp * 100.0 - (d_lon + temp) * 60.0;
  } else {
    temp = (int)d_lon;
    dm_lon = temp * 100 + (d_lon - temp) * 60.0;
  }

  dtostrf(dm_lon, 8, 2, lonStr);

  if (dm_lon < 10000) {
    lonStr[0] = '0';
  }
  if (dm_lon < 1000) {
    lonStr[1] = '0';
  }

  if (d_lon >= 0.0) {
    lonStr[8] = 'E';
  } else {
    lonStr[8] = 'W';
  }


}


static void updateGpsData(int ms)
{

  if(!ublox_high_alt_mode_enabled){
      //enable ublox high altitude mode
      delay(100);
      setGPS_DynamicModel6();
      #if defined(DEVMODE)
        Serial.println(F("ublox DynamicModel6 enabled..."));
      #endif      
      ublox_high_alt_mode_enabled = true;
   }

  while (!Serial1) {
    delayMicroseconds(1); // wait for serial port to connect.
  }
  unsigned long start = millis();
  unsigned long bekle = 0;
  do
  {
    while (Serial1.available() > 0) {
      char c;
      c = Serial1.read();
      
#if defined(RAW_GPS)
      Serial.print(c);
#endif
      
      gps.encode(c);
      bekle = millis();
    }
    if (gps.time.isValid())
    {
      setTime(gps.time.hour(), gps.time.minute(), gps.time.second(), NULL, NULL, NULL);

    }
    if (bekle != 0 && bekle + 10 < millis())break;
  } while (millis() - start < ms);

  set_tx_dbm();
  divide_alt();

}

float readBatt() {
  float R1 = 560000.0; // 560K
  float R2 = 100000.0; // 100K
  float value = 0.0;
  do {
    value = analogRead(BattPin);
    delay(5);// ilk a????l????ta hatal?? ??l????m yapmamas?? i??in
    value = analogRead(BattPin);
    //value = value - 8; // diren?? tolerans d??zeltmesi
    value = (value * 2.56) / 1024.0;
    value = value / (R2 / (R1 + R2));
  } while (value > 16.0);
  return value ;
}

void Timer3Tick(void)
{
  proceed = true;
}

void encode()
{
  wdt_reset();
  gps_sats_value = 0;
  gps_seconds = 0;
  si5351.init(SI5351_CRYSTAL_LOAD_0PF, 0, CORRECTION);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA); // Set for max power if desired
  si5351.output_enable(SI5351_CLK0, 0);
  uint8_t i;

  switch (cur_mode)
  {
    case MODE_JT9:
      hf_freq = JT9_DEFAULT_FREQ;
      symbol_count = JT9_SYMBOL_COUNT; // From the library defines
      tone_spacing = JT9_TONE_SPACING;
      tone_delay = JT9_DELAY;
      break;
    case MODE_JT65:
      hf_freq = JT65_DEFAULT_FREQ;
      symbol_count = JT65_SYMBOL_COUNT; // From the library defines
      tone_spacing = JT65_TONE_SPACING;
      tone_delay = JT65_DELAY;
      break;
    case MODE_JT4:
      hf_freq = JT4_DEFAULT_FREQ;
      symbol_count = JT4_SYMBOL_COUNT; // From the library defines
      tone_spacing = JT4_TONE_SPACING;
      tone_delay = JT4_DELAY;
      break;
    case MODE_WSPR:
      hf_freq = WSPR_DEFAULT_FREQ;
      symbol_count = WSPR_SYMBOL_COUNT; // From the library defines
      tone_spacing = WSPR_TONE_SPACING;
      tone_delay = WSPR_DELAY;
      break;
    case MODE_FT8:
      hf_freq = FT8_DEFAULT_FREQ;
      symbol_count = FT8_SYMBOL_COUNT; // From the library defines
      tone_spacing = FT8_TONE_SPACING;
      tone_delay = FT8_DELAY;
      break;
    case MODE_FSQ_2:
      hf_freq = FSQ_DEFAULT_FREQ;
      tone_spacing = FSQ_TONE_SPACING;
      tone_delay = FSQ_2_DELAY;
      break;
    case MODE_FSQ_3:
      hf_freq = FSQ_DEFAULT_FREQ;
      tone_spacing = FSQ_TONE_SPACING;
      tone_delay = FSQ_3_DELAY;
      break;
    case MODE_FSQ_4_5:
      hf_freq = FSQ_DEFAULT_FREQ;
      tone_spacing = FSQ_TONE_SPACING;
      tone_delay = FSQ_4_5_DELAY;
      break;
    case MODE_FSQ_6:
      hf_freq = FSQ_DEFAULT_FREQ;
      tone_spacing = FSQ_TONE_SPACING;
      tone_delay = FSQ_6_DELAY;
      break;
  }
  set_tx_buffer();

  // Now transmit the channel symbols
  if (cur_mode == MODE_FSQ_2 || cur_mode == MODE_FSQ_3 || cur_mode == MODE_FSQ_4_5 || cur_mode == MODE_FSQ_6)
  {
    uint8_t j = 0;
    while (tx_buffer[j++] != 0xff);
    symbol_count = j - 1;
  }

  // Reset the tone to the base frequency and turn on the output
  si5351.output_enable(SI5351_CLK0, 1);

  uint32_t timer_period = tone_delay;
  timer_period *= 1000;
  Timer3.initialize(timer_period);
  Timer3.attachInterrupt(Timer3Tick);
  Timer3.restart();

  for (i = 0; i < symbol_count; i++)
  {
    si5351.set_freq((hf_freq * 100) + (tx_buffer[i] * tone_spacing), SI5351_CLK0);
    proceed = false;
    while (!proceed);
    wdt_reset();
  }
  Timer3.stop();
  // Turn off the output
  si5351.output_enable(SI5351_CLK0, 0);
}

void set_tx_buffer()
{
  // Clear out the transmit buffer
  memset(tx_buffer, 0, 255);

  // Set the proper frequency and timer CTC depending on mode
  switch (cur_mode)
  {
    case MODE_JT9:
      jtencode.jt9_encode(hf_message, tx_buffer);
      break;
    case MODE_JT65:
      jtencode.jt65_encode(hf_message, tx_buffer);
      break;
    case MODE_JT4:
      jtencode.jt4_encode(hf_message, tx_buffer);
      break;
    case MODE_WSPR:
      jtencode.wspr_encode(hf_call, hf_loc_4_digits, dbm, tx_buffer);
      break;
    case MODE_FT8:
      jtencode.ft8_encode(hf_message, tx_buffer);
      break;
    case MODE_FSQ_2:
    case MODE_FSQ_3:
    case MODE_FSQ_4_5:
    case MODE_FSQ_6:
      jtencode.fsq_dir_encode(hf_call, "n0call", ' ', "hello world", tx_buffer);
      break;
  }
}

int set_tx_dbm()
{
  int altitude_dbm = gps.altitude.meters();

  if (altitude_dbm >= 0 && altitude_dbm <= 999) {
    dbm = 0;
  }
  else if (altitude_dbm >= 1000 && altitude_dbm <= 1999) {
    dbm = 3;
  }
  else if (altitude_dbm >= 2000 && altitude_dbm <= 2999) {
    dbm = 7;
  }
  else if (altitude_dbm >= 3000 && altitude_dbm <= 3999) {
    dbm = 10;
  }
  else if (altitude_dbm >= 4000 && altitude_dbm <= 4999) {
    dbm = 13;
  }
  else if (altitude_dbm >= 5000 && altitude_dbm <= 5999) {
    dbm = 17;
  }
  else if (altitude_dbm >= 6000 && altitude_dbm <= 6999) {
    dbm = 20;
  }
  else if (altitude_dbm >= 7000 && altitude_dbm <= 7999) {
    dbm = 23;
  }
  else if (altitude_dbm >= 8000 && altitude_dbm <= 8999) {
    dbm = 27;
  }
  else if (altitude_dbm >= 9000 && altitude_dbm <= 9999) {
    dbm = 30;
  }
  else if (altitude_dbm >= 10000 && altitude_dbm <= 10999) {
    dbm = 33;
  }
  else if (altitude_dbm >= 11000 && altitude_dbm <= 11999) {
    dbm = 37;
  }
  else if (altitude_dbm >= 12000 && altitude_dbm <= 12999) {
    dbm = 40;
  }
  else if (altitude_dbm >= 13000 && altitude_dbm <= 13999) {
    dbm = 43;
  }
  else if (altitude_dbm >= 14000 && altitude_dbm <= 14999) {
    dbm = 47;
  }
  else if (altitude_dbm >= 15000 && altitude_dbm <= 15999) {
    dbm = 50;
  }
  else {
    dbm = -20;
  }
  return dbm;
}

void GridLocator_4_digits(char *dst) {
  char o1, o2;
  char a1, a2;
  long remainder;
  
  // longitude
  remainder = (gps.location.lng() * 100000) + 18000000L;
  o1 = (remainder / 2000000);
  o2 = ((remainder % 2000000)) / 200000;
  
  // latitude
  remainder = (gps.location.lat() * 100000) +  9000000L;
  a1 = (remainder / 1000000);
  a2 = ((remainder % 1000000)) / 100000;

  dst[0] = o1 + 'A';
  dst[1] = a1 + 'A';
  dst[2] = o2 + '0';
  dst[3] = a2 + '0';
  dst[4] = (char)0;

}


void GridLocator_6_digits(char *dst) {
  char o1, o2, o3;
  char a1, a2, a3;
  long remainder;
  
  // longitude
  remainder = (gps.location.lng() * 100000) + 18000000L;
  o1 = (remainder / 2000000);
  o2 = ((remainder % 2000000)) / 200000;
  o3 = ((remainder % 200000)) / 8333;
  
  // latitude
  remainder = (gps.location.lat() * 100000) +  9000000L;
  a1 = (remainder / 1000000);
  a2 = ((remainder % 1000000)) / 100000;
  a3 = ((remainder % 100000)) / 4166;

  dst[0] = o1 + 'A';
  dst[1] = a1 + 'A';
  dst[2] = o2 + '0';
  dst[3] = a2 + '0';
  dst[4] = o3 + 'A';
  dst[5] = a3 + 'A';
  dst[6] = (char)0;

}


void GridLocator_2_last_digits(char *dst) {
  char o1;
  char a1;
  long remainder;
  
  // longitude
  remainder = (gps.location.lng() * 100000) + 18000000L;
  o1 = ((remainder % 200000)) / 8333;
  
  // latitude
  remainder = (gps.location.lat() * 100000) +  9000000L;
  a1 = ((remainder % 100000)) / 4166;

  dst[0] = o1 + 'A';
  dst[1] = a1 + 'A';
  dst[2] = (char)0;

}

int divide_alt() {
  
  int altitude_divide = gps.altitude.meters();
//  int altitude_divide = 13299.50;
  int string_alt = altitude_divide;
  String string_val = String(string_alt);
  String val_string;
  
  if (altitude_divide >= 0 && altitude_divide <= 999) {
    val_alt = altitude_divide;
  }
  else if (altitude_divide >= 1000 && altitude_divide <= 9999) {
    val_string = string_val.substring(1,4);
    val_alt = val_string.toInt();
  }
  else if (altitude_divide >= 10000 && altitude_divide <= 15999) {
    val_string = string_val.substring(2,5);
    val_alt = val_string.toInt();
  }
  else {
    val_alt = 0;
  }

  val_alt = val_alt / 38.41;
  
  if (val_alt == 1) { val_char_alt = "A"; }
  else if (val_alt == 2)  { val_char_alt = "B"; }
  else if (val_alt == 3)  { val_char_alt = "C"; }
  else if (val_alt == 4)  { val_char_alt = "D"; }
  else if (val_alt == 5)  { val_char_alt = "E"; }  
  else if (val_alt == 6)  { val_char_alt = "F"; }
  else if (val_alt == 7)  { val_char_alt = "G"; }
  else if (val_alt == 8)  { val_char_alt = "H"; }
  else if (val_alt == 9)  { val_char_alt = "I"; }
  else if (val_alt == 10) { val_char_alt = "J"; }
  else if (val_alt == 11) { val_char_alt = "K"; }  
  else if (val_alt == 12) { val_char_alt = "L"; }
  else if (val_alt == 13) { val_char_alt = "M"; }
  else if (val_alt == 14) { val_char_alt = "N"; }
  else if (val_alt == 15) { val_char_alt = "O"; }
  else if (val_alt == 16) { val_char_alt = "P"; }
  else if (val_alt == 17) { val_char_alt = "Q"; }  
  else if (val_alt == 18) { val_char_alt = "R"; }
  else if (val_alt == 19) { val_char_alt = "S"; }
  else if (val_alt == 20) { val_char_alt = "T"; }
  else if (val_alt == 21) { val_char_alt = "U"; }
  else if (val_alt == 22) { val_char_alt = "V"; }
  else if (val_alt == 23) { val_char_alt = "W"; }
  else if (val_alt == 24) { val_char_alt = "X"; }
  else if (val_alt == 25) { val_char_alt = "Y"; }
  else if (val_alt == 26) { val_char_alt = "Z"; }
  else { val_char_alt = "A"; }

}


void gpsDebug() {
#if defined(DEVMODE)
  Serial.println();
  Serial.println(F("Sats HDOP Latitude   Longitude   Fix  Date       Time     Date Alt    Course Speed Card Chars Sentences Checksum"));
  Serial.println(F("          (deg)      (deg)       Age                      Age  (m)    --- from GPS ----  RX    RX        Fail"));
  Serial.println(F("-----------------------------------------------------------------------------------------------------------------"));

  GridLocator_6_digits(hf_loc_6_digits);

//  printInt(gps.satellites.value(), gps.satellites.isValid(), 5);
  printInt(gps_sats_value, gps.satellites.isValid(), 5);
  printInt(gps.hdop.value(), gps.hdop.isValid(), 5);
  printFloat(gps.location.lat(), gps.location.isValid(), 11, 6);
  printFloat(gps.location.lng(), gps.location.isValid(), 12, 6);
  printInt(gps.location.age(), gps.location.isValid(), 5);
  printDateTime(gps.date, gps.time);
  printFloat(gps.altitude.meters(), gps.altitude.isValid(), 7, 2);
  printFloat(gps.course.deg(), gps.course.isValid(), 7, 2);
  printFloat(gps.speed.kmph(), gps.speed.isValid(), 6, 2);
  printStr(gps.course.isValid() ? TinyGPSPlus::cardinal(gps.course.value()) : "*** ", 6);

  printInt(gps.charsProcessed(), true, 6);
  printInt(gps.sentencesWithFix(), true, 10);
  printInt(gps.failedChecksum(), true, 9);
  Serial.println();
  Serial.println(readBatt());
  Serial.println(hf_loc_6_digits);
  Serial.println(GpsInvalidTime);

#endif
}

static void printFloat(float val, bool valid, int len, int prec)
{
#if defined(DEVMODE)
  if (!valid)
  {
    while (len-- > 1)
      Serial.print('*');
    Serial.print(' ');
  }
  else
  {
    Serial.print(val, prec);
    int vi = abs((int)val);
    int flen = prec + (val < 0.0 ? 2 : 1);
    flen += vi >= 1000 ? 4 : vi >= 100 ? 3 : vi >= 10 ? 2 : 1;
    for (int i = flen; i < len; ++i)
      Serial.print(' ');
  }
#endif
}

static void printInt(unsigned long val, bool valid, int len)
{
#if defined(DEVMODE)
  char sz[32] = "*****************";
  if (valid)
    sprintf(sz, "%ld", val);
  sz[len] = 0;
  for (int i = strlen(sz); i < len; ++i)
    sz[i] = ' ';
  if (len > 0)
    sz[len - 1] = ' ';
  Serial.print(sz);
#endif
}

static void printDateTime(TinyGPSDate &d, TinyGPSTime &t)
{
#if defined(DEVMODE)
  if (!d.isValid())
  {
    Serial.print(F("********** "));
  }
  else
  {
    char sz[32];
    sprintf(sz, "%02d/%02d/%02d ", d.month(), d.day(), d.year());
    Serial.print(sz);
  }

  if (!t.isValid())
  {
    Serial.print(F("******** "));
  }
  else
  {
    char sz[32];
    sprintf(sz, "%02d:%02d:%02d ", t.hour(), t.minute(), t.second());
    Serial.print(sz);
  }

  printInt(d.age(), d.isValid(), 5);
#endif
}

static void printStr(const char *str, int len)
{
#if defined(DEVMODE)
  int slen = strlen(str);
  for (int i = 0; i < len; ++i)
    Serial.print(i < slen ? str[i] : ' ');
#endif
}

//following GPS code from : https://github.com/HABduino/HABduino/blob/master/Software/habduino_v4/habduino_v4.ino
void setGPS_DynamicModel6() {
  int gps_set_sucess=0;

  // RS41 GPS Module set Airborne Mode < 1g / Fix Mode Auto / min SV Elevation 1 deg
  uint8_t setdm6[] = {
    0xB5, 0x62, 0x06, 0x24, 0x24, 0x00, 0xFF, 0xFF, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
    0x01, 0x0A, 0xFA, 0x00, 0xFA, 0x00, 0x64, 0x00, 0x2C, 0x01, 0x00, 0x0A, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5D, 0xE3 };
 
  while(!gps_set_sucess)
  {
    sendUBX(setdm6, sizeof(setdm6)/sizeof(uint8_t));
    gps_set_sucess=getUBX_ACK(setdm6);
  }
}

void sendUBX(uint8_t *MSG, uint8_t len) {
  Serial1.flush();
  Serial1.write(0xFF);
  delay(500);
  for(int i=0; i<len; i++) {
    Serial1.write(MSG[i]);
  }
}
boolean getUBX_ACK(uint8_t *MSG) {
  uint8_t b;
  uint8_t ackByteID = 0;
  uint8_t ackPacket[10];
  unsigned long startTime = millis();
 
// Construct the expected ACK packet
  ackPacket[0] = 0xB5; // header
  ackPacket[1] = 0x62; // header
  ackPacket[2] = 0x05; // class
  ackPacket[3] = 0x01; // id
  ackPacket[4] = 0x02; // length
  ackPacket[5] = 0x00;
  ackPacket[6] = MSG[2]; // ACK class
  ackPacket[7] = MSG[3]; // ACK id
  ackPacket[8] = 0; // CK_A
  ackPacket[9] = 0; // CK_B
 
// Calculate the checksums
  for (uint8_t ubxi=2; ubxi<8; ubxi++) {
    ackPacket[8] = ackPacket[8] + ackPacket[ubxi];
    ackPacket[9] = ackPacket[9] + ackPacket[8];
  }
 
while (1) {
 
  // Test for success
  if (ackByteID > 9) {
    // All packets in order!
    #if defined(DEVMODE)
      Serial.println("setdm6 okay");
    #endif
    return true;
  }
 
  // Timeout if no valid response in 3 seconds
  if (millis() - startTime > 3000) {
    #if defined(DEVMODE)
      Serial.println("setdm6 not okay");
    #endif
    return false;
  }
 
  // Make sure data is available to read
  if (Serial1.available()) {
    b = Serial1.read();
 
// Check that bytes arrive in sequence as per expected ACK packet
    if (b == ackPacket[ackByteID]) {
      ackByteID++;
    }
    else {
      ackByteID = 0; // Reset and look again, invalid order
    }
  }
  }
}

void setGPS_powersave() {
  int gps_set_powersave_sucess=0;

  uint8_t setpowersave[] = {
    0xB5, 0x62, 0x06, 0x11, 0x02, 0x00, 0x08, 0x01, 0x22, 0x92 };
 
  while(!gps_set_powersave_sucess)
  {
    send_powersave_UBX(setpowersave, sizeof(setpowersave)/sizeof(uint8_t));
    gps_set_powersave_sucess=get_powersave_UBX_ACK(setpowersave);
  }
}

void send_powersave_UBX(uint8_t *MSG, uint8_t len) {
  Serial1.flush();
  Serial1.write(0xFF);
  delay(500);
  for(int i=0; i<len; i++) {
    Serial1.write(MSG[i]);
  }
}
boolean get_powersave_UBX_ACK(uint8_t *MSG) {
  uint8_t b;
  uint8_t ackByteID = 0;
  uint8_t ackPacket[10];
  unsigned long startTime = millis();
 
// Construct the expected ACK packet
  ackPacket[0] = 0xB5; // header
  ackPacket[1] = 0x62; // header
  ackPacket[2] = 0x05; // class
  ackPacket[3] = 0x01; // id
  ackPacket[4] = 0x02; // length
  ackPacket[5] = 0x00;
  ackPacket[6] = MSG[2]; // ACK class
  ackPacket[7] = MSG[3]; // ACK id
  ackPacket[8] = 0; // CK_A
  ackPacket[9] = 0; // CK_B
 
// Calculate the checksums
  for (uint8_t ubxi=2; ubxi<8; ubxi++) {
    ackPacket[8] = ackPacket[8] + ackPacket[ubxi];
    ackPacket[9] = ackPacket[9] + ackPacket[8];
  }
 
while (1) {
 
  // Test for success
  if (ackByteID > 9) {
    // All packets in order!
    #if defined(DEVMODE)
      Serial.println("set powersave okay");
    #endif
    return true;
  }
 
  // Timeout if no valid response in 3 seconds
  if (millis() - startTime > 3000) {
    #if defined(DEVMODE)
      Serial.println("set powersave not okay");
    #endif
    return false;
  }
 
  // Make sure data is available to read
  if (Serial1.available()) {
    b = Serial1.read();
 
// Check that bytes arrive in sequence as per expected ACK packet
    if (b == ackPacket[ackByteID]) {
      ackByteID++;
    }
    else {
      ackByteID = 0; // Reset and look again, invalid order
    }
  }
  }
}

void setGPS_continuous_mode() {
  int gps_set_continuous_sucess=0;

  uint8_t setcontinuous[] = {
    0xB5, 0x62, 0x06, 0x11, 0x02, 0x00, 0x08, 0x00, 0x21, 0x91 };
 
  while(!gps_set_continuous_sucess)
  {
    send_continuous_UBX(setcontinuous, sizeof(setcontinuous)/sizeof(uint8_t));
    gps_set_continuous_sucess=get_continuous_UBX_ACK(setcontinuous);
  }
}

void send_continuous_UBX(uint8_t *MSG, uint8_t len) {
  Serial1.flush();
  Serial1.write(0xFF);
  delay(500);
  for(int i=0; i<len; i++) {
    Serial1.write(MSG[i]);
  }
}
boolean get_continuous_UBX_ACK(uint8_t *MSG) {
  uint8_t b;
  uint8_t ackByteID = 0;
  uint8_t ackPacket[10];
  unsigned long startTime = millis();
 
// Construct the expected ACK packet
  ackPacket[0] = 0xB5; // header
  ackPacket[1] = 0x62; // header
  ackPacket[2] = 0x05; // class
  ackPacket[3] = 0x01; // id
  ackPacket[4] = 0x02; // length
  ackPacket[5] = 0x00;
  ackPacket[6] = MSG[2]; // ACK class
  ackPacket[7] = MSG[3]; // ACK id
  ackPacket[8] = 0; // CK_A
  ackPacket[9] = 0; // CK_B
 
// Calculate the checksums
  for (uint8_t ubxi=2; ubxi<8; ubxi++) {
    ackPacket[8] = ackPacket[8] + ackPacket[ubxi];
    ackPacket[9] = ackPacket[9] + ackPacket[8];
  }
 
while (1) {
 
  // Test for success
  if (ackByteID > 9) {
    // All packets in order!
    #if defined(DEVMODE)
      Serial.println("set continuous okay");
    #endif
    return true;
  }
 
  // Timeout if no valid response in 3 seconds
  if (millis() - startTime > 3000) {
    #if defined(DEVMODE)
      Serial.println("set continuous not okay");
    #endif
    return false;
  }
 
  // Make sure data is available to read
  if (Serial1.available()) {
    b = Serial1.read();
 
// Check that bytes arrive in sequence as per expected ACK packet
    if (b == ackPacket[ackByteID]) {
      ackByteID++;
    }
    else {
      ackByteID = 0; // Reset and look again, invalid order
    }
  }
  }
}


/*
void setGPS_coldstart() {
  int gps_coldstart_sucess=0;

  uint8_t setcoldstart[] = {
	0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0xFF, 0xB9, 0x00, 0x00, 0xC6, 0x8B };
 
  while(!gps_coldstart_sucess)
  {
    send_coldstart_UBX(setcoldstart, sizeof(setcoldstart)/sizeof(uint8_t));
    gps_coldstart_sucess=get_coldstart_UBX_ACK(setcoldstart);
  }
}

void send_coldstart_UBX(uint8_t *MSG, uint8_t len) {
  Serial1.flush();
  Serial1.write(0xFF);
  delay(500);
  for(int i=0; i<len; i++) {
    Serial1.write(MSG[i]);
  }
}
boolean get_coldstart_UBX_ACK(uint8_t *MSG) {
  uint8_t b;
  uint8_t ackByteID = 0;
  uint8_t ackPacket[10];
  unsigned long startTime = millis();
 
// Construct the expected ACK packet
  ackPacket[0] = 0xB5; // header
  ackPacket[1] = 0x62; // header
  ackPacket[2] = 0x05; // class
  ackPacket[3] = 0x01; // id
  ackPacket[4] = 0x02; // length
  ackPacket[5] = 0x00;
  ackPacket[6] = MSG[2]; // ACK class
  ackPacket[7] = MSG[3]; // ACK id
  ackPacket[8] = 0; // CK_A
  ackPacket[9] = 0; // CK_B
 
// Calculate the checksums
  for (uint8_t ubxi=2; ubxi<8; ubxi++) {
    ackPacket[8] = ackPacket[8] + ackPacket[ubxi];
    ackPacket[9] = ackPacket[9] + ackPacket[8];
  }
 
while (1) {
 
  // Test for success
  if (ackByteID > 9) {
    // All packets in order!
    #if defined(DEVMODE)
      Serial.println("set coldstart okay");
    #endif
    return true;
  }
 
  // Timeout if no valid response in 3 seconds
  if (millis() - startTime > 3000) {
    #if defined(DEVMODE)
      Serial.println("set coldstart not okay");
    #endif
    return false;
  }
 
  // Make sure data is available to read
  if (Serial1.available()) {
    b = Serial1.read();
 
// Check that bytes arrive in sequence as per expected ACK packet
    if (b == ackPacket[ackByteID]) {
      ackByteID++;
    }
    else {
      ackByteID = 0; // Reset and look again, invalid order
    }
  }
  }
}


void setGPS_jamming() {
  int gps_set_jam_sucess=0;

  uint8_t setjam[] = {
    0xB5, 0x62, 0x06, 0x39, 0x08, 0x00, 0xF3, 0xAC, 0x62, 0xAD, 0x1E, 0x53, 0x00, 0x00, 0x66, 0x75 };
 
  while(!gps_set_jam_sucess)
  {
    send_jam_UBX(setjam, sizeof(setjam)/sizeof(uint8_t));
    gps_set_jam_sucess=get_jam_UBX_ACK(setjam);
  }
}

void send_jam_UBX(uint8_t *MSG, uint8_t len) {
  Serial1.flush();
  Serial1.write(0xFF);
  delay(500);
  for(int i=0; i<len; i++) {
    Serial1.write(MSG[i]);
  }
}
boolean get_jam_UBX_ACK(uint8_t *MSG) {
  uint8_t b;
  uint8_t ackByteID = 0;
  uint8_t ackPacket[10];
  unsigned long startTime = millis();
 
// Construct the expected ACK packet
  ackPacket[0] = 0xB5; // header
  ackPacket[1] = 0x62; // header
  ackPacket[2] = 0x05; // class
  ackPacket[3] = 0x01; // id
  ackPacket[4] = 0x02; // length
  ackPacket[5] = 0x00;
  ackPacket[6] = MSG[2]; // ACK class
  ackPacket[7] = MSG[3]; // ACK id
  ackPacket[8] = 0; // CK_A
  ackPacket[9] = 0; // CK_B
 
// Calculate the checksums
  for (uint8_t ubxi=2; ubxi<8; ubxi++) {
    ackPacket[8] = ackPacket[8] + ackPacket[ubxi];
    ackPacket[9] = ackPacket[9] + ackPacket[8];
  }
 
while (1) {
 
  // Test for success
  if (ackByteID > 9) {
    // All packets in order!
    #if defined(DEVMODE)
      Serial.println("set jamming okay");
    #endif
    return true;
  }
 
  // Timeout if no valid response in 3 seconds
  if (millis() - startTime > 3000) {
    #if defined(DEVMODE)
      Serial.println("set jamming not okay");
    #endif
    return false;
  }
 
  // Make sure data is available to read
  if (Serial1.available()) {
    b = Serial1.read();
 
// Check that bytes arrive in sequence as per expected ACK packet
    if (b == ackPacket[ackByteID]) {
      ackByteID++;
    }
    else {
      ackByteID = 0; // Reset and look again, invalid order
    }
  }
  }
}

*/
