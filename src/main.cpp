#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>

// Визначення піна для зуммера (наприклад, D1 / GPIO 5)
#define BUZZER_PIN 5

// Облікові дані для доступу до захищеної вебпанелі (Basic Auth)
const char *www_user = "";
const char *www_pass = "";

ESP8266WebServer server(80);
DNSServer dnsServer;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 2 * 3600, 60000); // UTC+2, оновлення кожні 60с

// IP-адреса та шлюз плати у режимі Точки Доступу (AP / Captive Portal)
IPAddress apIP(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

bool isConfigMode = false;
bool buzzerState = false; // Поточний стан зуммера

unsigned long last_time_connecting = 0;
unsigned long startAttemptTime = 0;
unsigned long last_time_sec = 0;

enum States
{
  CONNECTING,
  SUCCESSFULLY,
  FAILED
};

States cur_state = CONNECTING;

// Сторінка налаштування Wi-Fi (коли роутер недоступний)
void handleRootConfig()
{
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<title>Wi-Fi Configurator</title></head><body>"
                "<h2>ESP8266 Wi-Fi Setup</h2>"
                "<form action='/save' method='POST'>"
                "SSID: <input type='text' name='ssid'><br><br>"
                "Password: <input type='password' name='pass'><br><br>"
                "<input type='submit' value='Save & Connect'>"
                "</form></body></html>";
  server.send(200, "text/html", html);
}

// Збереження даних конфігуратора
void handleSave()
{
  server.send(200, "text/html", "<h3>Settings saved! Rebooting...</h3>");
  delay(2000);
  ESP.restart();
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  // Налаштування піна зуммера як виходу
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  // Спроба підключення до збереженої мережі Wi-Fi
  WiFi.begin("", "");
  startAttemptTime = millis();
}

void loop()
{
  unsigned long cur_time = millis();

  switch (cur_state)
  {
  case CONNECTING:
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("\nConnected successfully!");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());

      // Запускаємо NTP-клієнт та mDNS
      timeClient.begin();
      if (MDNS.begin("esp8266"))
      {
        Serial.println("MDNS responder started: http://esp8266.local");
      }
      MDNS.addService("http", "tcp", 80);

      // Головна захищена вебсторінка хаба з керуванням зуммером
      server.on("/", []()
                {
        // Перевірка Basic Auth
        if (!server.authenticate(www_user, www_pass))
        {
          return server.requestAuthentication();
        }

        // Обробка натискання кнопки перемикання зуммера на сторінці
        if (server.hasArg("toggle"))
        {
          buzzerState = !buzzerState;
          if (buzzerState) {
            tone(BUZZER_PIN, 100); // Увімкнути звук на частоті 1000 Гц
          } else {
            noTone(BUZZER_PIN); // Вимкнути звук
          }
        }

        String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                      "<title>Automation Hub - Buzzer</title></head><body>"
                      "<h2>Local Automation Hub (Protected)</h2>"
                      "<p>Status: <span style='color:green;'><b>Online</b></span></p>"
                      "<p>Current NTP Time: <b>" + timeClient.getFormattedTime() + "</b></p>"
                      "<p>Buzzer Status: <b>" + (buzzerState ? "<span style='color:red;'>ON (Beep)</span>" : "OFF") + "</b></p>"
                      "<form action='/' method='POST'>"
                      "<input type='hidden' name='toggle' value='1'>"
                      "<input type='submit' value='Toggle Buzzer' style='padding: 10px 20px; font-size: 16px;'>"
                      "</form>"
                      "</body></html>";
                      
        server.send(200, "text/html", html); });

      server.begin();
      cur_state = SUCCESSFULLY;
    }
    else if (cur_time - startAttemptTime >= 10000) // Таймаут 10 секунд
    {
      Serial.println("\nFailed to connect. Starting AP Config Portal...");
      isConfigMode = true;

      WiFi.mode(WIFI_AP);
      WiFi.softAPConfig(apIP, apIP, subnet);
      WiFi.softAP("ESP8266_Config_Hub");

      dnsServer.start(53, "*", apIP);

      server.on("/", handleRootConfig);
      server.on("/save", HTTP_POST, handleSave);
      server.onNotFound(handleRootConfig);

      server.begin();
      cur_state = FAILED;
    }
    else
    {
      // Неблокуюча індикація крапками
      if (cur_time - last_time_connecting >= 500)
      {
        last_time_connecting = cur_time;
        Serial.print(".");
      }
    }
    break;
  }

  case SUCCESSFULLY:
  {
    server.handleClient();

    // Перевірка втрати зв'язку з Wi-Fi під час роботи
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi connection lost! Reconnecting...");
      WiFi.reconnect();
      startAttemptTime = millis();
      cur_state = CONNECTING;
      break;
    }

    // Фонова перевірка часу та таймерів раз на 1 секунду
    if (cur_time - last_time_sec >= 1000)
    {
      last_time_sec = cur_time;
      timeClient.update();

      int currentHour = timeClient.getHours();
      int currentMinute = timeClient.getMinutes();
      int currentSecond = timeClient.getSeconds();

      // Приклад NTP-розкладу (наприклад, ранковий сигнал о 08:00:00)
      if (currentHour == 20 && currentMinute == 40 && currentSecond == 0)
      {
        buzzerState = true;
        tone(BUZZER_PIN, 100);
        Serial.println("[AUTOMATION] Triggered scheduled event: Morning routine!");
      }

      // Автоматичне вимкнення через 5 секунд (о 08:00:05)
      if (currentHour == 20 && currentMinute == 40 && currentSecond == 40)
      {
        buzzerState = false;
        noTone(BUZZER_PIN);
        Serial.println("[AUTOMATION] Buzzer turned OFF automatically.");
      }
    }
    break;
  }

  case FAILED:
  {
    // Обслуговування Captive Portal (точки доступу)
    dnsServer.processNextRequest();
    server.handleClient();
    break;
  }

  default:
    break;
  }
}