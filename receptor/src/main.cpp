#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// Credenciales (WiFi y Telegram) definidas en include/secrets.h (ignorado por Git)
#include "secrets.h"

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Definición de pines LoRa E220 en la Estación Terrestre
#define LORA_RX 16
#define LORA_TX 17
#define LORA_AUX 15

HardwareSerial loraSerial(2);
WebServer server(80);

// Variables globales para la telemetría recibida
String lastJsonRaw = "{}";
unsigned int lastId = 0;
float lastAlt = 0.0;
float lastPres = 0.0;
float lastTemp = 0.0;
int lastHum = 0;
int lastPm1 = 0, lastPm25 = 0, lastPm10 = 0;
float accX = 0.0, accY = 0.0, accZ = 0.0;
unsigned long ultimaRecepcion = 0; // millis() del último paquete LoRa recibido

// Control de tiempos para Telegram
unsigned long ultimoSondeo = 0;
unsigned long ultimaAlerta[2] = {0, 0};
const unsigned long INTERVALO_SONDEO = 2000;            // revisar mensajes de Telegram cada 2 s
const unsigned long ENFRIAMIENTO = 5UL * 60UL * 1000UL; // 5 min entre avisos iguales
const unsigned long SIN_SENAL_MS = 60000;               // 1 minuto sin paquetes para disparar alerta

// Umbrales de ejemplo para alertas en Telegram
const float PM25_ALTO = 45.0;

// Declaración de funciones
void handleRoot();
void handleData();
void enviarDatosTelegram(const String &chat_id);
void alertaTelegram(int i, const String &texto);
void revisarAlertas();
void atenderMensajes(int n);

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==============================================");
  Serial.println("🛰️ ESTACIÓN TERRESTRE: WEB, GRÁFICAS Y TELEGRAM");
  Serial.println("==============================================");

  // Configurar búfer UART amplio para evitar pérdida de paquetes LoRa durante peticiones web/Telegram
  loraSerial.setRxBufferSize(256);
  loraSerial.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
  pinMode(LORA_AUX, INPUT);

  // Conectar a Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Configuración de certificado seguro para Telegram (puedes cambiar a client.setInsecure() si te da problemas de certificado SSL)
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  Serial.print("Conectando a Wi-Fi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n¡Conectado a Wi-Fi con éxito!");
  Serial.print("🌐 Panel Web en Vivo: http://");
  Serial.println(WiFi.localIP());

  // Mensaje inicial a Telegram indicando que la estación encendió
  bot.sendMessage(CHAT_ID, "🛰️ AeroSense Terrestre en línea y escuchando al globo", "");

  // Rutas del Servidor Web
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop()
{
  // 1. Manejar peticiones HTTP del navegador (Dashboard con gráficas)
  server.handleClient();

  // 2. Verificar si hay datos nuevos viniendo del LoRa E220
  if (loraSerial.available() > 0)
  {
    String incomingMessage = loraSerial.readStringUntil('\n');
    incomingMessage.trim();

    if (incomingMessage.startsWith("{") && incomingMessage.endsWith("}"))
    {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, incomingMessage);

      if (!error)
      {
        lastJsonRaw = incomingMessage;
        lastId = doc["id"];
        lastAlt = doc["alt"];
        lastPres = doc["pres"];
        lastTemp = doc["temp"];
        lastHum = doc["hum"];

        JsonObject pm = doc["pm"];
        lastPm1 = pm["pm1"];
        lastPm25 = pm["pm25"];
        lastPm10 = pm["pm10"];

        JsonObject acc = doc["acc"];
        accX = acc["x"];
        accY = acc["y"];
        accZ = acc["z"];

        ultimaRecepcion = millis(); // Actualizar marca de tiempo de última señal

        Serial.println("-> Paquete LoRa procesado correctamente:");
        Serial.println(incomingMessage);
      }
    }
  }

  // 3. Revisar comandos y enviar alertas de Telegram de forma periódica
  if (millis() - ultimoSondeo > INTERVALO_SONDEO)
  {
    int n = bot.getUpdates(bot.last_message_received + 1);
    while (n)
    {
      atenderMensajes(n);
      n = bot.getUpdates(bot.last_message_received + 1);
    }
    revisarAlertas();
    ultimoSondeo = millis();
  }
}

// --- FUNCIONES DE TELEGRAM ---

void enviarDatosTelegram(const String &chat_id)
{
  String m = "📊 *Datos más recientes (Globo amarrado)*\n";
  m += "📦 ID Paquete: " + String(lastId) + "\n";
  m += "🏔️ Altitud: " + String(lastAlt, 1) + " m\n";
  m += "🌡️ Temperatura: " + String(lastTemp, 1) + " °C\n";
  m += "💧 Humedad: " + String(lastHum) + " %\n";
  m += "🌫️ PM1.0: " + String(lastPm1) + " µg/m3\n";
  m += "🌫️ PM2.5: " + String(lastPm25) + " µg/m3\n";
  m += "🌫️ PM10: " + String(lastPm10) + " µg/m3\n";
  m += "🔵 Presión: " + String(lastPres, 1) + " hPa";
  bot.sendMessage(chat_id, m, "Markdown");
}

void alertaTelegram(int i, const String &texto)
{
  if (ultimaAlerta[i] == 0 || millis() - ultimaAlerta[i] > ENFRIAMIENTO)
  {
    bot.sendMessage(CHAT_ID, "⚠️ *ALERTA AEROSENSE:* " + texto, "Markdown");
    ultimaAlerta[i] = millis();
  }
}

void revisarAlertas()
{
  // Alerta si lleva más de 1 minuto sin recibir paquetes del globo
  if (ultimaRecepcion > 0 && (millis() - ultimaRecepcion > SIN_SENAL_MS))
  {
    alertaTelegram(0, "¡Sin señal LoRa del globo desde hace más de 1 minuto!");
  }
  // Alerta si los niveles de PM2.5 rebasan el umbral
  if (lastPm25 > PM25_ALTO)
  {
    alertaTelegram(1, "Nivel alto de partículas PM2.5 detectado: " + String(lastPm25) + " µg/m3");
  }
}

void atenderMensajes(int n)
{
  for (int i = 0; i < n; i++)
  {
    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID)
    { // Filtrar por chat autorizado
      bot.sendMessage(chat_id, "Acceso no autorizado", "");
      continue;
    }
    String texto = bot.messages[i].text;
    if (texto == "/start")
    {
      bot.sendMessage(chat_id, "AeroSense ESP32 - Estación Terrestre\n/datos: lecturas actuales\n/estado: estado de conexión LoRa", "");
    }
    else if (texto == "/datos")
    {
      enviarDatosTelegram(chat_id);
    }
    else if (texto == "/estado")
    {
      unsigned long s = (millis() - ultimaRecepcion) / 1000;
      bot.sendMessage(chat_id, "📡 Último paquete recibido hace " + String(s) + " segundos.", "");
    }
  }
}

// --- SERVIDOR WEB Y GRÁFICAS (CHART.JS) ---

void handleRoot()
{
  String html = "<html lang='es'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Estación Terrestre - Globo Amarrado</title>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; background: #121212; color: #e0e0e0; text-align: center; margin: 0; padding: 15px; }";
  html += ".card { background: #1e1e1e; max-width: 600px; margin: 15px auto; padding: 15px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }";
  html += "h1 { color: #4CAF50; font-size: 22px; }";
  html += "h3 { color: #e0e0e0; font-size: 16px; margin-bottom: 5px; }";
  html += ".data { font-size: 15px; margin: 6px 0; }";
  html += ".highlight { color: #2196F3; font-weight: bold; }";
  html += "canvas { background: #121212; border-radius: 8px; margin-top: 10px; max-height: 160px; }";
  html += "</style></head><body>";

  html += "<h1>🛰️ Estación Terrestre - Globo Amarrado (100m)</h1>";
  html += "<div class='card'>";
  html += "<p class='data'>Paquete ID: <span id='pid' class='highlight'>0</span></p>";
  html += "<p class='data'>🏔️ Altitud: <span id='palt' class='highlight'>0</span> m</p>";
  html += "<p class='data'>🌡️ Temperatura: <span id='ptemp' class='highlight'>0</span> °C | 💧 Humedad: <span id='phum' class='highlight'>0</span> %</p>";
  html += "<p class='data'>🌫️ PM1.0: <span id='ppm1' class='highlight'>0</span> | PM2.5: <span id='ppm25' class='highlight'>0</span> | PM10: <span id='ppm10' class='highlight'>0</span> ug/m3</p>";
  html += "<p class='data'>🔵 Presión: <span id='ppres' class='highlight'>0</span> hPa</p>";
  html += "</div>";

  // Contenedores de Gráficas
  html += "<div class='card'><h3>📈 Altitud (m)</h3><canvas id='altChart'></canvas></div>";
  html += "<div class='card'><h3>📈 Temperatura (°C) y Humedad (%)</h3><canvas id='envChart'></canvas></div>";
  html += "<div class='card'><h3>📈 Calidad del Aire - Material Particulado (ug/m3)</h3><canvas id='pmChart'></canvas></div>";

  html += "<script>";
  html += "const optComun = { responsive: true, scales: { x: { ticks: { color: '#888' } }, y: { ticks: { color: '#e0e0e0' } } } };";

  // Gráfica Altitud
  html += "const altChart = new Chart(document.getElementById('altChart').getContext('2d'), {";
  html += "  type: 'line', data: { labels: [], datasets: [{ label: 'Altitud (m)', data: [], borderColor: '#2196F3', backgroundColor: 'rgba(33, 150, 243, 0.1)', fill: true, tension: 0.3 }] }, options: optComun";
  html += "});";

  // Gráfica Ambiental (Temp / Hum)
  html += "const envChart = new Chart(document.getElementById('envChart').getContext('2d'), {";
  html += "  type: 'line', data: { labels: [], datasets: [";
  html += "    { label: 'Temp (°C)', data: [], borderColor: '#FF9800', backgroundColor: 'rgba(255, 152, 0, 0.1)', tension: 0.3 },";
  html += "    { label: 'Humedad (%)', data: [], borderColor: '#00BCD4', backgroundColor: 'rgba(0, 188, 212, 0.1)', tension: 0.3 }";
  html += "  ] }, options: optComun";
  html += "});";

  // Gráfica Calidad del Aire (PM2.5 y PM10)
  html += "const pmChart = new Chart(document.getElementById('pmChart').getContext('2d'), {";
  html += "  type: 'line', data: { labels: [], datasets: [";
  html += "    { label: 'PM2.5', data: [], borderColor: '#E91E63', tension: 0.3 },";
  html += "    { label: 'PM10', data: [], borderColor: '#9C27B0', tension: 0.3 }";
  html += "  ] }, options: optComun";
  html += "});";

  html += "function fetchData() {";
  html += "  fetch('/data').then(res => res.json()).then(data => {";
  html += "    document.getElementById('pid').innerText = data.id;";
  html += "    document.getElementById('palt').innerText = data.alt;";
  html += "    document.getElementById('ptemp').innerText = data.temp;";
  html += "    document.getElementById('phum').innerText = data.hum;";
  html += "    document.getElementById('ppm1').innerText = data.pm.pm1;";
  html += "    document.getElementById('ppm25').innerText = data.pm.pm25;";
  html += "    document.getElementById('ppm10').innerText = data.pm.pm10;";
  html += "    document.getElementById('ppres').innerText = data.pres;";

  html += "    let timeNow = new Date().toLocaleTimeString();";

  // Push Altitud
  html += "    if (altChart.data.labels.length > 15) { altChart.data.labels.shift(); altChart.data.datasets[0].data.shift(); }";
  html += "    altChart.data.labels.push(timeNow); altChart.data.datasets[0].data.push(data.alt); altChart.update();";

  // Push Temp/Hum
  html += "    if (envChart.data.labels.length > 15) { envChart.data.labels.shift(); envChart.data.datasets[0].data.shift(); envChart.data.datasets[1].data.shift(); }";
  html += "    envChart.data.labels.push(timeNow); envChart.data.datasets[0].data.push(data.temp); envChart.data.datasets[1].data.push(data.hum); envChart.update();";

  // Push PM2.5/PM10
  html += "    if (pmChart.data.labels.length > 15) { pmChart.data.labels.shift(); pmChart.data.datasets[0].data.shift(); pmChart.data.datasets[1].data.shift(); }";
  html += "    pmChart.data.labels.push(timeNow); pmChart.data.datasets[0].data.push(data.pm.pm25); pmChart.data.datasets[1].data.push(data.pm.pm10); pmChart.update();";

  html += "  });";
  html += "}";
  html += "setInterval(fetchData, 2000);";
  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleData()
{
  server.send(200, "application/json", lastJsonRaw);
}