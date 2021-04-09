#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Streaming.h>  
#include <EEPROM.h>

#include <Schedule.h>
#include <PolledTimeout.h>

#include <time.h>                       // time() ctime()
#include <sys/time.h>                   // struct timeval
#include <coredecls.h>                  // settimeofday_cb()

#include <sntp.h>                       // sntp_servermode_dhcp()
#include <TZ.h>

#include <ESP8266WebServer.h>

#define MYTZ TZ_Europe_Moscow


const char *ssid     = "KA-WiFi-ha";
const char *password = "185638955";
const char *hostName = "ha.controller.aquarium.water";

IPAddress staticIP(192,168,2,101);
IPAddress gateway(192,168,2,1);
//если маска 255,255,255,0 то NTP не работает
IPAddress subnet(0,0,0,0);

ESP8266WebServer server(80);

//константы интервалов в мсек
#define POOL_INT		1000

static timeval tv;
static timespec tp;
static time_t now;
static uint32_t now_ms, now_us;

//флаг установки времени с NTP. Пока время не приехало - работать таймеру запрещаем.
bool ntpTimeOk = false;

static esp8266::polledTimeout::periodicMs poolNow(POOL_INT);

// for testing purpose:
extern "C" int clock_gettime(clockid_t unused, struct timespec *tp);

// initial time (possibly given by an external RTC)
#define RTC_UTC_TEST 1510592825 // 1510592825 = Monday 13 November 2017 17:07:05 UTC

struct LED_MODE_TYPE {
	bool blink;
	uint16_t t1, t2; // свечения, пауза
};

enum LM_t {
	LM_WAIT,
	LM_WAIT_NTP,	
	LM_RUN,
	LM_LOW_PRESSURE,
	LM_PAUSED
};
LM_t LED_MODE;

LED_MODE_TYPE	 lm[] = { 	
						{1, 50, 10000}, //ждем цикл
						{1, 50, 100}, 	//ждем NTP
						{1, 50, 1000},  //цикл работает
						{1, 3000,3000},  //низкое давление
						{1, 50, 3000}	//пауза
					};					
					
// Переменные пуллинга					
unsigned long curMillis;
unsigned long btnTime = 0;
unsigned long prevMillisLED = 0;
unsigned long prevMillis1 = 0;
unsigned long prevMillis2 = 0;
unsigned long prevMillis3 = 0;
unsigned long prevMillis4 = 0;

/*WeMos D1 mini Pin Number 	Arduino IDE Pin Number
D0	16
D1	5
D2	4
D3	0
D4	2
D5	14
D6	12
D7	13
D8	15
TX	1
RX	3
*/

//клапана воды (входы)
#define V_RO		16	//D0
#define V_RAW		14	//D5
//клапана каналов (выходы)
#define V_C1		12	//D6
#define V_C2		13	//D7
//счетчик воды
#define METER		4
//датчик низкого давления 
#define PRESSURE	5

enum mode_type {			//режим работы
 MODE_WAIT,		//ждем таймера			
 MODE_START,	//старт			
 MODE_C1_RO,	//первый канал - осмос
 MODE_C1_RAW,	//первый канал - вода
 MODE_C2_RO,	//второй канал - осмос
 MODE_C2_RAW,	//второй канал - вода
 MODE_MAX		//остановка
};

mode_type MODE;

struct RELAY {
	uint8_t pin; 	//пин реле
	bool onState;	//состояние выхода когда реле включено (LOW/HIGH)
};

RELAY relay_ro	= {V_RO, 	LOW};
RELAY relay_raw = {V_RAW, 	LOW};
RELAY relay_c1 	= {V_C1, 	LOW};
RELAY relay_c2 	= {V_C2, 	LOW};

struct RELAY_STATE {
	RELAY relay; 	//реле
	bool state;		//состояние выхода когда реле включено (LOW/HIGH)
};
	
struct STATE {
	RELAY_STATE relays[4];			//реле и его состояние
	volatile uint32_t counter;	//значение счетчика
	volatile uint32_t ttl;		//время в состоянии
};

#define STATE_MAX_TIME_SEC 3600 //не больше часа

#define MAX_RO	10000
#define MAX_RAW	40000

volatile STATE state[MODE_MAX+1] = {
					{{{relay_ro, LOW},  {relay_raw, LOW},  {relay_c1, LOW},  {relay_c2, LOW}},  0, 0},  //ожидание
					{{{relay_ro, LOW},  {relay_raw, LOW},  {relay_c1, LOW},  {relay_c2, LOW}},  0, 0},  //запуск
					{{{relay_ro, HIGH}, {relay_raw, LOW},  {relay_c1, HIGH}, {relay_c2, LOW}},  0, 0}, //осмос, канал 1
					{{{relay_ro, LOW},  {relay_raw, HIGH}, {relay_c1, HIGH}, {relay_c2, LOW}},  0, 0}, //вода, канал 1
					{{{relay_ro, HIGH}, {relay_raw, LOW},  {relay_c1, LOW},  {relay_c2, HIGH}}, 0, 0}, //осмос, канал 2
					{{{relay_ro, LOW},  {relay_raw, HIGH}, {relay_c1, LOW},  {relay_c2, HIGH}}, 0, 0}, //вода, канал 2
					{{{relay_ro, LOW},  {relay_raw, LOW},  {relay_c1, LOW},  {relay_c2, LOW}},  0, 0}  //стоп
				};

void printStatus(void) {	
	printTime();
	Serial << F(" MODE = ");
	switch(MODE) {
		case MODE_WAIT: 	Serial << F("MODE_WAIT") << endl; break;
		case MODE_START: 	Serial << F("MODE_START") << endl; break;
		case MODE_C1_RO: 	Serial << F("MODE_C1_RO") << endl; break;
		case MODE_C1_RAW: 	Serial << F("MODE_C1_RAW") << endl; break;
		case MODE_C2_RO: 	Serial << F("MODE_C2_RO") << endl; break;
		case MODE_C2_RAW: 	Serial << F("MODE_C2_RAW") << endl; break;
		case MODE_MAX: 		Serial << F("MODE_MAX") << endl; break;
	};
} 

void printTime(void) { 
	now = time(nullptr);
	tm* localTimeValue = localtime(&now);
	
	char str[9];
	
	if(localTimeValue != nullptr) {
		snprintf(str, sizeof(str), "%02d:%02d:%02d", localTimeValue->tm_hour, localTimeValue->tm_min, localTimeValue->tm_sec);	
		Serial << str << endl;		
	} else
		Serial << F("Can't aquire local time") << endl;
	
}

struct settings_t {
	uint8_t hour;
	//кол-во миллилитров 
	uint16_t ro1;
	uint16_t raw1;
	uint16_t ro2;
	uint16_t raw2;
} settings;

void saveSettings() {
	EEPROM.begin(512); 
    delay(10);
	EEPROM.put(0, settings);
    EEPROM.commit(); 
}

void readSettings() {
     EEPROM.begin(512); 
     delay(10); 
     EEPROM.get(0, settings);
		
}	 
	 
void processLED() { 
	if (lm[LED_MODE].blink) {
		//Если цикл режима светодиода кончился - ставим цикл в начало
		if(curMillis - prevMillisLED > lm[LED_MODE].t1+lm[LED_MODE].t2)
			prevMillisLED = curMillis;
			
		if((prevMillisLED < curMillis) && (curMillis <= (prevMillisLED+lm[LED_MODE].t1))) { //фаза удержания яркости
			digitalWrite(LED_BUILTIN, LOW);
		} else if(prevMillisLED+lm[LED_MODE].t1 < curMillis && curMillis <= prevMillisLED+lm[LED_MODE].t1+lm[LED_MODE].t2) { //фаза выключения
			digitalWrite(LED_BUILTIN, HIGH);
		}		
	} else
		digitalWrite(LED_BUILTIN, LOW);		 
}

void time_is_set_scheduled() {
  ntpTimeOk = true;  
  LED_MODE = LED_MODE==LM_WAIT_NTP ? LM_WAIT : LED_MODE; 
  Serial << "Update time from NTP" << endl;
}

void setup() {
 
  pinMode(LED_BUILTIN, OUTPUT);
    
  pinMode(V_RO, OUTPUT);
  pinMode(V_RAW, OUTPUT);
  pinMode(V_C1, OUTPUT);
  pinMode(V_C2, OUTPUT);
  pinMode(PRESSURE, INPUT);
  digitalWrite(PRESSURE, HIGH);
	
	//Повыключать все
	for (int i = 0; i < 4; i++) {			
		digitalWrite(state[MODE].relays[i].relay.pin, !state[MODE].relays[i].relay.onState);			
	}
	
  Serial.begin(115200);
  
  LED_MODE = LM_WAIT_NTP; 
  
  time_t rtc = RTC_UTC_TEST;
  timeval tv = { rtc, 0 };
  settimeofday(&tv, nullptr);

  settimeofday_cb(time_is_set_scheduled);
  configTime(MYTZ, "pool.ntp.org");
  
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);  
  WiFi.setAutoReconnect(true);
  WiFi.hostname(hostName);
  WiFi.config(staticIP, gateway, subnet);
  WiFi.begin(ssid, password);  

  readSettings();

  server.on("/", handle_OnConnect);
  server.on("/settings", handle_Settings);
  server.on("/save", handle_Save);
  
  server.on("/start", handle_start);
  server.on("/stop", handle_stop);  
  server.onNotFound(handle_NotFound);
  
  server.begin(); 
  
   pinMode(METER, INPUT);
   attachInterrupt(digitalPinToInterrupt(METER), getFlow, RISING);
   sei();
   
   //ставим всем режимам по 1 сек времени и запускаем - это диагностика
   for ( int i = MODE_C1_RO; i <= MODE_C2_RAW; i++ )
		state[i].ttl = 2;
		
   MODE = MODE_START;
}

//даташит - 4380. в реальности хрен пойми сколько, но точно меньше
//импульсов счетчика на 1мл воды 0.3323
//V=0.3223*COUNT+10.388
//count = V-10.388 / 0.3223

uint32_t mlToCount(uint32_t ml) {
	return round((ml * 10000 - 10388)/ 2918);
}

//обработчик счетчика 
ICACHE_RAM_ATTR void getFlow ()
{
	if(MODE > MODE_START)
		if(state[MODE].counter>0) {		
			state[MODE].counter--;
		}
}

void loop() {
	
	curMillis = millis();
	
	//проверим на переполнение
	if(prevMillis1 > curMillis || prevMillis2 > curMillis || prevMillis3 > curMillis || prevMillis4 > curMillis || prevMillisLED > curMillis){
		prevMillis1 = 0;
		prevMillis2 = 0;
		prevMillis3 = 0;
		prevMillis4 = 0;
		prevMillisLED = 0;
	}
	
	processLED();
	server.handleClient();
	
	if (poolNow) {
		pool();
	}	
}

uint16_t deadTime = 10;
bool manualStart = false;

//Периодичность контроля давления
#define CHECK_PRESSURE_INTERVAL_SEC 30

//Время паузы / работы
#define PAUSE_TIME_SEC 10
//300
#define WORK_TIME_SEC 10
//60

uint16_t checkPressureTime = CHECK_PRESSURE_INTERVAL_SEC;
uint16_t pauseTimer = CHECK_PRESSURE_INTERVAL_SEC;

bool lowPressure = false;

void pool() {
		
	if(MODE == MODE_WAIT) {	
		if(deadTime>0) 
			deadTime--;	
		now = time(nullptr);
		tm* localTimeValue = localtime(&now);
		if( (isStartHour(localTimeValue) || manualStart) 
			  && deadTime==0
			  && ntpTimeOk ) {
			//Задержка запуска = 0
			state[MODE_START].ttl = 0;
			state[MODE_START].counter = 0;
			
			state[MODE_C1_RO].ttl = STATE_MAX_TIME_SEC;
			state[MODE_C1_RO].counter = mlToCount(settings.ro1);
			state[MODE_C1_RAW].ttl = STATE_MAX_TIME_SEC;
			state[MODE_C1_RAW].counter = mlToCount(settings.raw1);
			state[MODE_C2_RO].ttl = STATE_MAX_TIME_SEC;
			state[MODE_C2_RO].counter = mlToCount(settings.ro2);
			state[MODE_C2_RAW].ttl = STATE_MAX_TIME_SEC;
			state[MODE_C2_RAW].counter = mlToCount(settings.raw2);		
			
			//Защитный интервал от повторных запусков
			deadTime = STATE_MAX_TIME_SEC;
			LED_MODE = LM_RUN;
			MODE = MODE_START;		
			manualStart = false;
			Serial << endl << F("Start") << endl;
		}
	}
	
	//контроль давления каждые 10 сек
	if(isWork()) {
		if(checkPressureTime<=0) {		
			checkPressureTime = CHECK_PRESSURE_INTERVAL_SEC;
			lowPressure = digitalRead(PRESSURE);
		}			
		
		checkPressureTime--;
		
		if(lowPressure) {
			//Выключить выходные клапана
			Serial << endl << F("out valve off - low pressure. Next check in ") << checkPressureTime << F("sec.") << endl;
			LED_MODE = LM_LOW_PRESSURE;
			relayOf(relay_c1);
			relayOf(relay_c2);
		} else {
			if(pauseTimer<=0) 			
				pauseTimer = PAUSE_TIME_SEC + WORK_TIME_SEC;
			
			pauseTimer--;
			
			//Пауза, выключить выход
			if(pauseTimer>WORK_TIME_SEC) {
				Serial << endl << F("out valve off - paused to ") << pauseTimer - WORK_TIME_SEC << F("sec.") << endl;
				LED_MODE = LM_PAUSED;
				relayOf(relay_c1);
				relayOf(relay_c2);
				}
			else {
				Serial << endl << F("out valve ON - worked to ") << pauseTimer << F("sec.") << endl;
				LED_MODE = LM_RUN;
				setRelay();
				state[MODE].ttl--;
			}
		}			
	}	
	
	processState();
}

boolean isWork(){
	return MODE == MODE_C1_RAW || MODE == MODE_C1_RO || MODE == MODE_C2_RAW || MODE == MODE_C2_RO;
}

boolean isStartHour(tm* localTimeValue){
	return localTimeValue->tm_hour == settings.hour && localTimeValue->tm_min == 0;
}

void processState(void) {
	if(MODE > MODE_WAIT ) {	
		printStatus();
		//время текущего состояния вышло
		if(state[MODE].ttl==0 || state[MODE].counter==0) {
			Serial << "state[MODE].ttl==0" << endl;
			if(MODE <= MODE_MAX) {
				// переходим на след состояние
				incMode();
				Serial << "incMode" << endl;
			} else {
				// переходим в режим ожидания
				MODE = MODE_WAIT;
				LED_MODE = LM_WAIT;
				Serial << "MODE = MODE_WAIT" << endl;
			}
			//включаем новый режим
			Serial << "switch new mode" << endl;
			setRelay();
		} else {
		Serial << "state[MODE].ttl > 0" << endl;
		}		
	}
	
	if(MODE==MODE_WAIT || MODE==MODE_MAX)
		LED_MODE = LM_WAIT;
}

void setRelay() {
	for (int i = 0; i < 4; i++) {
		bool newPinState = state[MODE].relays[i].relay.onState ? state[MODE].relays[i].state : !state[MODE].relays[i].state;
		Serial << F("digitalWrite: ") << state[MODE].relays[i].relay.pin << F("->") << newPinState << endl;
		digitalWrite(state[MODE].relays[i].relay.pin, newPinState);
	}
}

void relayOf(RELAY relay) {
	bool newPinState = !relay.onState;
	Serial << F("digitalWrite: ") << relay.pin << F("->") << newPinState << endl;
	digitalWrite(relay.pin, newPinState);
}

void handle_OnConnect() { 
  Serial << endl << F("handle_OnConnect");
  server.send(200, "text/html", MainPage()); 
}

void handle_NotFound() {
  server.send(404, "text/plain", "Not found");
}

String MainPage() {
  String ptr = "<!DOCTYPE html> <html>\n";
  ptr +="<head><meta http-equiv=\"Refresh\" content=\"5\" charset=\"UTF-8\" name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n";
  ptr +="<title>Контроллер подмены воды в аквариуме</title>\n";
  ptr +="<style>html  lang=\"ru-RU\" { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}\n";
  ptr +="body{margin-top: 50px;} h1 {color: #444444;margin: 50px auto 30px;} h3 {color: #444444;margin-bottom: 50px;}\n";
  ptr +=".button {display: block;width: 120px;background-color: #1abc9c;border: none;color: white;padding: 13px 30px;text-decoration: none;font-size: 25px;margin: 0px auto 35px; text-align: center; cursor: pointer;border-radius: 4px;}\n";
  ptr +=".button-start {background-color: #1abc9c;}\n";
  ptr +=".button-start:active {background-color: #16a085;}\n";
  ptr +=".button-stop {background-color: #34495e;}\n";
  ptr +=".button-stop:active {background-color: #2c3e50;}\n";
  ptr +=".button-settings {background-color: #34495e;}\n";
  ptr +=".button-settings:active {background-color: #2c3e50;}\n";
  ptr +="p {font-size: 14px;color: #888;margin-bottom: 10px;}\n";
  ptr +="</style>\n";
  ptr +="</head>\n";
  ptr +="<body>\n";
  ptr +="<h1>Контроллер подмены</h1>\n";
  
	ptr += "<p>ESP8266 Wemos D1 mini based</p>\n";
	ptr += "<p>SSID: " + WiFi.SSID() + "</p>\n";
  	ptr += "<p>IP: " + WiFi.localIP().toString() + "</p>\n";
  	ptr += "<p>MAC: " + WiFi.macAddress() + "</p>\n";
	
	now = time(nullptr);
	tm* localTimeValue = localtime(&now);
	
	char str[9];
	
	if(localTimeValue != nullptr) {
		snprintf(str, sizeof(str), "%02d:%02d:%02d", localTimeValue->tm_hour, localTimeValue->tm_min, localTimeValue->tm_sec);
	
		Serial << str << endl;
	
		ptr += "<p>Время: " + String(str) + "</p>\n";
	} else
		Serial << "Can't aquire local time" << endl;  
		
	if(MODE >= MODE_START && MODE < MODE_MAX) {
		
		char tmpc[16];
		
		if(lowPressure) {
			snprintf(tmpc, sizeof(tmpc), "%d", checkPressureTime);
			ptr += "<p>Подмена приостановлена - низкое давление. Проверка через " + String(tmpc) + " сек. </p>\n";
		}
		else if(pauseTimer>WORK_TIME_SEC) 
			ptr += "<p>Подмена запущена. Пауза.</p>\n";
		else
			ptr += "<p>Подмена запущена. </p>\n";
		
		ptr += "<div>";
			ptr += "<p>Канал 1 </p>\n";
			snprintf(tmpc, sizeof(tmpc), "%d", getPercent(state[MODE_C1_RO].counter, mlToCount(settings.ro1)));
			ptr += "<td>  Осмос " + String(tmpc) + "%</td>\n";
			snprintf(tmpc, sizeof(tmpc), "%d", getPercent(state[MODE_C1_RAW].counter, mlToCount(settings.raw1)));
			ptr += "<td>, вода " + String(tmpc) + "%</td>\n";
		ptr += "<div>";
		
		ptr += "<div>";
			ptr += "<p>Канал 2 </p>\n";
			snprintf(tmpc, sizeof(tmpc), "%d", getPercent(state[MODE_C2_RO].counter, mlToCount(settings.ro2)));
			ptr += "<td>  Осмос " + String(tmpc) + "%</td>\n";
			snprintf(tmpc, sizeof(tmpc), "%d", getPercent(state[MODE_C2_RAW].counter, mlToCount(settings.raw2)));
			ptr += "<td>, вода " + String(tmpc) + "%</td>\n";
		ptr += "<div>";
		
		ptr += "<div>";			
			int32_t cnt = (int32_t)state[MODE_C1_RO].counter + (int32_t)state[MODE_C2_RO].counter + (int32_t)state[MODE_C1_RAW].counter + (int32_t)state[MODE_C2_RAW].counter;
			int32_t set = mlToCount(settings.ro1) + mlToCount(settings.ro2) + mlToCount(settings.raw1) + mlToCount(settings.raw2);
			snprintf(tmpc, sizeof(tmpc), "%d", getPercent(cnt, set));
			ptr += "<p>Общий прогресс " + String(tmpc) + "%</p>\n";			
		ptr += "<div>";
	}
	
	if(MODE == MODE_WAIT && deadTime > 0) {
		char tmpc[16];
		int minute;
		
		if(deadTime < 60)
			minute = 1;
		else
			minute = deadTime/60;
		
		snprintf(tmpc, sizeof(tmpc), "%d", minute);
		ptr += "<p>Подмена закончена. Следующий запуск не ранее чем через " + String(tmpc) + " минут. </p>\n";
	}
	
	ptr += "<a class=\"button button-start\" id=\"start_button\">Пуск</a>";
	ptr += "<a class=\"button button-stop\" id=\"stop_button\">Стоп</a>";
	ptr += "<a class=\"button button-settings\" id=\"settings\" href=\"/settings\">Настройки</a>";
	
	ptr +="\
	<script src=\"https://ajax.googleapis.com/ajax/libs/jquery/1.11.3/jquery.min.js\"></script>\    
    <script>\
      $('#start_button').click(function(e){\
        e.preventDefault();\
        $.get('/start', function(data){\
		console.log(data);\		
        });\
      });\   
	  $('#stop_button').click(function(e){\
        e.preventDefault();\
        $.get('/stop', function(data){\
		console.log(data);\		
        });\
      });\   	  
    </script>";

  ptr +="</body>\n";
  
  ptr +="</html>\n";
  return ptr;
}

uint8_t getPercent(uint32_t value1, uint32_t value2) {
	return round(100 - (10000*value1)/(value2*100));
}

void handle_Settings(){
	Serial << endl << F("handle_Settings");
	server.send(200, "text/html", ConfigPage()); 
}

String ConfigPage() {

	char tmpc[16];

  String ptr = "<!DOCTYPE html> <html>\n";
  ptr +="<head><meta meta charset=\"UTF-8\" name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n";
  ptr +="<title>Настройка параметров контроллера подмены воды в аквариуме</title>\n";
  ptr +="<style>html  lang=\"ru-RU\" { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}\n";
  ptr +="body{margin-top: 50px;} h1 {color: #444444;margin: 50px auto 30px;} h3 {color: #444444;margin-bottom: 50px;}\n";
  ptr +=".button {display: block;width: 120px;background-color: #1abc9c;border: none;color: white;padding: 13px 30px;text-decoration: none;font-size: 20px;margin: 0px auto 35px; text-align: center; cursor: pointer;border-radius: 4px;}\n";
  ptr +=".button-save {background-color: #1abc9c;}\n";
  ptr +=".button-save:active {background-color: #16a085;}\n";
  ptr +=".button-cancel {background-color: #34495e;}\n";
  ptr +=".button-cancel:active {background-color: #2c3e50;}\n";
  ptr +="p {font-size: 14px;color: #888;margin-bottom: 10px;}\n";
  ptr +="</style>\n";
  ptr +="</head>\n";
  ptr +="<body>\n";
  ptr +="<h1>Настройки</h1>\n";

	ptr += "<form action=\"/cTS\">";
	
	ptr += "<div>";
	snprintf(tmpc, sizeof(tmpc), "%02d", settings.hour);
	ptr += "<td>Время запуска </td><td><input type=\"number\" min=\"0\" max=\"23\" step=\"1\" name=\"hour\" id=\"hour\" value=\"" + String(tmpc) + "\"> часов</td>";
	ptr += "</div>";
	
	ptr += "<div>";
		ptr += "<td><h2>Объемы подмены (миллилитры)</h2></td>\n";
	ptr += "<div>";
	
	ptr += "<div>";
		ptr += "<td><h3>Канал 1</h3></td>\n";
	ptr += "</div>";
	
	ptr += "<div>";		
		snprintf(tmpc, sizeof(tmpc), "%d", settings.ro1);	
		ptr += "<td>Осмос </td><td><input type=\"number\" step=\"100\" min=\"0\" max=\"5000\" name=\"ro1\" id=\"ro1\" value=\"" + String(tmpc) + "\"></td>";	
		snprintf(tmpc, sizeof(tmpc), "%d", settings.raw1);
		ptr += "<td> Обычная вода </td><td><input type=\"number\" step=\"100\" min=\"0\" max=\"40000\" name=\"raw1\" id=\"raw1\"value=\"" + String(tmpc) + "\"></td>";	
	ptr += "<div>";
	
	ptr += "<div>";
		ptr += "<td><h3>Канал 2</h3></td>\n";
	ptr += "</div>";

	ptr += "<div>";
		snprintf(tmpc, sizeof(tmpc), "%d", settings.ro2);		
		ptr += "<td>Осмос </td><td><input type=\"number\" step=\"100\" min=\"0\" max=\"5000\" name=\"ro2\" id=\"ro2\" value=\"" + String(tmpc) + "\"></td>";	
		snprintf(tmpc, sizeof(tmpc), "%d", settings.raw2);
		ptr += "<td> Обычная вода </td><td><input type=\"number\" step=\"100\" min=\"0\" max=\"40000\" name=\"raw2\" id=\"raw2\" value=\"" + String(tmpc) + "\"></td>";	
	ptr += "</div>";
	
	ptr += "</form>";
	ptr += "<a class=\"button button-save\" id=\"save_button\">Сохранить</a>";
	ptr += "<a class=\"button button-cancel\" id=\"cancel_button\" href=\"/\">Отмена</a>";
	
	ptr +="\
	<script src=\"https://ajax.googleapis.com/ajax/libs/jquery/1.11.3/jquery.min.js\"></script>\    
    <script>\
      var hour;\
      var ro1;\
      var raw1;\
	  var ro2;\
	  var raw2;\
      $('#save_button').click(function(e){\
        e.preventDefault();\
        hour = $('#hour').val();\
        ro1 = $('#ro1').val();\
        raw1 = $('#raw1').val();\
		ro2 = $('#ro2').val();\
        raw2 = $('#raw2').val();\
        $.get('/save?hour=' + hour + '&ro1=' + ro1 + '&raw1=' + raw1 + '&ro2=' + ro2 + '&raw2=' + raw2, function(data){\
		console.log(data);\
		window.location.href = '/';\
        });\
      });\      
    </script>";	 
	
  ptr +="</body>\n";
  ptr +="</html>\n";
  return ptr;
}

uint16_t validate(uint16_t value, uint16_t minValue, uint16_t maxValue){
	if(value < minValue)
		return minValue;
	if(value > maxValue)
		return maxValue;
	return value;
} 

void handle_Save(){
	Serial << endl << F("handle_Save") << endl;
	
	String tmps;
	uint8_t valueCount = 0;
	
	tmps = server.arg("hour");
	if (tmps != ""){
		Serial << "hour: " << tmps << endl;
		settings.hour = validate(tmps.toInt(), 1, 23);
		valueCount++;
	}
	
	tmps = server.arg("ro1");
	if (tmps != ""){
		Serial << "ro1: " << tmps << endl;
		settings.ro1 = validate(tmps.toInt(), 0, MAX_RO);
		valueCount++;
	}
	
	tmps = server.arg("raw1");
	if (tmps != ""){
		Serial << "raw1: " << tmps << endl;
		settings.raw1 = validate(tmps.toInt(), 0, MAX_RAW);
		valueCount++;
	}
	
	tmps = server.arg("ro2");
	if (tmps != ""){
		Serial << "ro2: " << tmps << endl;
		settings.ro2 = validate(tmps.toInt(), 0, MAX_RO);
		valueCount++;
	}
	
	tmps = server.arg("raw2");
	if (tmps != ""){
		Serial << "raw2: " << tmps << endl;
		settings.raw2 = validate(tmps.toInt(), 0, MAX_RAW);
		valueCount++;
	}
	
	if(valueCount==5) {
		Serial << "Write settings" << endl;
		saveSettings();
	}	
	
	server.send(200, "text/html", MainPage()); 
}

void handle_start() {
	manualStart = true;
	server.send(200, "text/html", MainPage());
}

void handle_stop() {
	manualStart = false;
	//Повыключать все
	for (int i = 0; i < 4; i++) {			
		digitalWrite(state[MODE].relays[i].relay.pin, !state[MODE].relays[i].relay.onState);			
	}
	MODE = MODE_WAIT;
	
	server.send(200, "text/html", MainPage());
}

void incMode() {
	switch(MODE){
		case MODE_WAIT: 	MODE = MODE_START; break;
		case MODE_START:	MODE = MODE_C1_RO; break;
		case MODE_C1_RO:	MODE = MODE_C1_RAW; break;
		case MODE_C1_RAW:	MODE = MODE_C2_RO; break;
		case MODE_C2_RO:	MODE = MODE_C2_RAW; break;
		case MODE_C2_RAW:	MODE = MODE_MAX; break;
		case MODE_MAX:		MODE = MODE_WAIT; break;
	}
}