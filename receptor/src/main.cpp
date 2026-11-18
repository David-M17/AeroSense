#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

#include "secrets.h" // ssid, password, BOT_TOKEN, CHAT_ID (ignorado por Git)
#include "telemetria.h"
#include "web_server.h"

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Definición de pines LoRa E220 en la Estación Terrestre
#define LORA_RX 16
#define LORA_TX 17
#define LORA_AUX 15

HardwareSerial loraSerial(2);

// Última telemetría recibida (compartida con web_server.cpp)
Telemetria tel;

// Línea que se va armando con los bytes que llegan del E220
String lineaLora = "";

// Control de tiempos para Telegram
unsigned long ultimoSondeo = 0;
unsigned long ultimaAlerta[2] = {0, 0};
const unsigned long INTERVALO_SONDEO = 2000;            // revisar mensajes de Telegram cada 2 s
const unsigned long ENFRIAMIENTO = 5UL * 60UL * 1000UL; // 5 min entre avisos iguales
const unsigned long SIN_SENAL_MS = 60000;               // 1 minuto sin paquetes para disparar alerta

// Umbrales de ejemplo para alertas en Telegram
const float PM25_ALTO = 45.0;

// Declaración de funciones
void leerLora();
void procesarPaquete(const String &linea);
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
  loraSerial.setRxBufferSize(512);
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

  // Panel web (ver web_server.cpp y web_page.h)
  webIniciar();
}

void loop()
{
  // 1. Manejar peticiones HTTP del navegador (Dashboard con gráficas)
  webAtender();

  // 2. Leer lo que llegue del LoRa E220 (paquetes JSON + byte RSSI)
  leerLora();

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

// --- RECEPCIÓN LORA ---

// Si el E220 receptor tiene habilitada la opción "RSSI byte", añade 1 byte al final de cada
// paquete recibido: dBm = -(256 - byte). El JSON es ASCII puro (< 0x80), y todo RSSI válido
// es >= 0x80 (<= -1 dBm), así que cualquier byte >= 0x80 se interpreta como RSSI y no se
// mezcla con el texto. Sin esa opción activa, simplemente nunca llega ese byte.
void leerLora()
{
  while (loraSerial.available() > 0)
  {
    uint8_t b = loraSerial.read();

    if (b >= 0x80)
    {
      tel.rssiDbm = -(256 - (int)b);
      tel.rssiTs = millis();
      Serial.printf("   RSSI: %d dBm (%s)\n", tel.rssiDbm, calidadRssi(tel.rssiDbm));
      continue;
    }

    if (b == '\r')
      continue;

    if (b == '\n')
    {
      lineaLora.trim();
      procesarPaquete(lineaLora);
      lineaLora = "";
      continue;
    }

    if (lineaLora.length() < 512)
      lineaLora += (char)b;
    else
      lineaLora = ""; // línea demasiado larga: ruido, se descarta
  }
}

void procesarPaquete(const String &linea)
{
  if (!linea.startsWith("{") || !linea.endsWith("}"))
    return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, linea);
  if (error)
    return;

  tel.id = doc["id"];
  tel.alt = doc["alt"];
  tel.pres = doc["pres"];
  tel.temp = doc["temp"];
  tel.hum = doc["hum"];

  // El transmisor manda null en pm cuando el PMS7003M no tiene lectura reciente
  JsonObject pm = doc["pm"];
  tel.pmValido = !pm["pm1"].isNull() && !pm["pm25"].isNull() && !pm["pm10"].isNull();
  if (tel.pmValido)
  {
    tel.pm1 = pm["pm1"];
    tel.pm25 = pm["pm25"];
    tel.pm10 = pm["pm10"];
  }

  JsonObject acc = doc["acc"];
  tel.accX = acc["x"];
  tel.accY = acc["y"];
  tel.accZ = acc["z"];

  tel.ultimaRecepcion = millis(); // Actualizar marca de tiempo de última señal

  Serial.println("-> Paquete LoRa procesado correctamente:");
  Serial.println(linea);
}

// --- FUNCIONES DE TELEGRAM ---

void enviarDatosTelegram(const String &chat_id)
{
  if (tel.ultimaRecepcion == 0)
  {
    bot.sendMessage(chat_id, "📡 Aún no se recibe ningún paquete del globo.", "");
    return;
  }

  String m = "📊 *Datos más recientes (Globo amarrado)*\n";
  m += "📦 ID Paquete: " + String(tel.id) + "\n";
  m += "🏔️ Altitud: " + String(tel.alt, 1) + " m\n";
  m += "🌡️ Temperatura: " + String(tel.temp, 1) + " °C\n";
  m += "💧 Humedad: " + String(tel.hum) + " %\n";
  if (tel.pmValido)
  {
    m += "🌫️ PM1.0: " + String(tel.pm1) + " µg/m3\n";
    m += "🌫️ PM2.5: " + String(tel.pm25) + " µg/m3\n";
    m += "🌫️ PM10: " + String(tel.pm10) + " µg/m3\n";
  }
  else
  {
    m += "🌫️ PM: sin lectura del sensor\n";
  }
  m += "🔵 Presión: " + String(tel.pres, 1) + " hPa\n";
  if (tel.rssiVigente())
    m += "📶 RSSI: " + String(tel.rssiDbm) + " dBm (" + calidadRssi(tel.rssiDbm) + ")";
  else
    m += "📶 RSSI: no disponible";
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
  if (tel.ultimaRecepcion > 0 && (millis() - tel.ultimaRecepcion > SIN_SENAL_MS))
  {
    alertaTelegram(0, "¡Sin señal LoRa del globo desde hace más de 1 minuto!");
  }
  // Alerta si los niveles de PM2.5 rebasan el umbral (solo con lectura válida)
  if (tel.pmValido && tel.pm25 > PM25_ALTO)
  {
    alertaTelegram(1, "Nivel alto de partículas PM2.5 detectado: " + String(tel.pm25) + " µg/m3");
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
      if (tel.ultimaRecepcion == 0)
      {
        bot.sendMessage(chat_id, "📡 Aún no se recibe ningún paquete LoRa.", "");
      }
      else
      {
        unsigned long s = (millis() - tel.ultimaRecepcion) / 1000;
        String m = "📡 Último paquete recibido hace " + String(s) + " segundos.";
        if (tel.rssiVigente())
          m += "\n📶 RSSI: " + String(tel.rssiDbm) + " dBm (" + calidadRssi(tel.rssiDbm) + ")";
        bot.sendMessage(chat_id, m, "");
      }
    }
  }
}
