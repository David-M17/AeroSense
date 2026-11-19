#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

#include "secrets.h" // BOT_TOKEN, CHAT_ID (ignorado por Git)
#include "telegram_bot.h"
#include "telemetria.h"

static constexpr unsigned long INTERVALO_SONDEO_MS = 2000;    // revisar mensajes cada 2 s
static constexpr unsigned long ENFRIAMIENTO_MS = 5UL * 60UL * 1000UL; // entre avisos iguales
static constexpr unsigned long SIN_SENAL_MS = 60000;          // 1 min sin paquetes => alerta
static constexpr unsigned long CONSULTA_LENTA_MS = 1500;      // para el aviso de diagnóstico
static constexpr float PM25_ALTO = 45.0;                      // umbral de ejemplo (µg/m3)

// Aviso de enlace débil: por debajo de este RSSI el margen empieza a ser corto y conviene
// acercar el globo. Ajústalo después de una prueba de alcance real.
static constexpr int RSSI_BAJO_DBM = -100;
static constexpr uint32_t TAREA_PILA = 12288;                 // TLS necesita mucha pila
static constexpr UBaseType_t TAREA_PRIORIDAD = 1;
static constexpr BaseType_t TAREA_NUCLEO = 1;

// Tipos de alerta, para llevar un enfriamiento independiente por cada una
enum Alerta
{
  ALERTA_SIN_SENAL,
  ALERTA_RSSI_BAJO,
  ALERTA_PM25,
  ALERTA_TOTAL
};

static WiFiClientSecure clienteSeguro;
static UniversalTelegramBot bot(BOT_TOKEN, clienteSeguro);
static unsigned long ultimaAlerta[ALERTA_TOTAL] = {0};

static void tareaTelegram(void *parametro);
static void sondearMensajes();
static void atenderMensaje(const telegramMessage &mensaje);
static void responderStart(const String &chatId);
static void responderDatos(const String &chatId);
static void responderEstado(const String &chatId);
static void enviarAlerta(Alerta tipo, const String &texto);
static void revisarAlertas();
static String lineaRssi(const Telemetria &t);

void telegramIniciar()
{
  clienteSeguro.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  bot.sendMessage(CHAT_ID, "🛰️ AeroSense Terrestre en línea y escuchando al globo", "");

  xTaskCreatePinnedToCore(tareaTelegram, "telegram", TAREA_PILA, nullptr,
                          TAREA_PRIORIDAD, nullptr, TAREA_NUCLEO);
}

// --- Tarea ---
// Las consultas a Telegram pueden tardar segundos (TLS sobre el hotspot). En su propia tarea,
// esas esperas no congelan el panel web ni la lectura LoRa.
static void tareaTelegram(void *parametro)
{
  for (;;)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      const unsigned long inicio = millis();
      sondearMensajes();
      revisarAlertas();

      const unsigned long duracion = millis() - inicio;
      if (duracion > CONSULTA_LENTA_MS)
        Serial.printf("⏱ Telegram tardó %lu ms (no bloquea el panel)\n", duracion);
    }
    vTaskDelay(pdMS_TO_TICKS(INTERVALO_SONDEO_MS));
  }
}

static void sondearMensajes()
{
  int n = bot.getUpdates(bot.last_message_received + 1);
  while (n)
  {
    for (int i = 0; i < n; i++)
      atenderMensaje(bot.messages[i]);
    n = bot.getUpdates(bot.last_message_received + 1);
  }
}

// --- Comandos ---

static void atenderMensaje(const telegramMessage &mensaje)
{
  const String chatId = String(mensaje.chat_id);
  if (chatId != CHAT_ID)
  {
    bot.sendMessage(chatId, "Acceso no autorizado", "");
    return;
  }

  if (mensaje.text == "/start")
    responderStart(chatId);
  else if (mensaje.text == "/datos")
    responderDatos(chatId);
  else if (mensaje.text == "/estado")
    responderEstado(chatId);
}

static void responderStart(const String &chatId)
{
  bot.sendMessage(chatId,
                  "AeroSense ESP32 - Estación Terrestre\n"
                  "/datos: lecturas actuales\n"
                  "/estado: estado de conexión LoRa",
                  "");
}

static String lineaRssi(const Telemetria &t)
{
  if (!t.rssiVigente())
    return "📶 RSSI: no disponible";
  return "📶 RSSI: " + String(t.rssiDbm) + " dBm (" + calidadRssi(t.rssiDbm) + ")";
}

static void responderDatos(const String &chatId)
{
  const Telemetria t = telSnapshot();
  if (!t.hayPaquete())
  {
    bot.sendMessage(chatId, "📡 Aún no se recibe ningún paquete del globo.", "");
    return;
  }

  String m = "📊 *Datos más recientes (Globo amarrado)*\n";
  m += "📦 ID Paquete: " + String(t.id) + "\n";
  m += "🏔️ Altitud: " + String(t.alt, 1) + " m\n";
  m += "🌡️ Temperatura: " + String(t.temp, 1) + " °C\n";
  m += "💧 Humedad: " + String(t.hum) + " %\n";
  if (t.pmValido)
  {
    m += "🌫️ PM1.0: " + String(t.pm1) + " µg/m3\n";
    m += "🌫️ PM2.5: " + String(t.pm25) + " µg/m3\n";
    m += "🌫️ PM10: " + String(t.pm10) + " µg/m3\n";
  }
  else
  {
    m += "🌫️ PM: sin lectura del sensor\n";
  }
  m += "🔵 Presión: " + String(t.pres, 1) + " hPa\n";
  m += lineaRssi(t);
  bot.sendMessage(chatId, m, "Markdown");
}

static void responderEstado(const String &chatId)
{
  const Telemetria t = telSnapshot();
  if (!t.hayPaquete())
  {
    bot.sendMessage(chatId, "📡 Aún no se recibe ningún paquete LoRa.", "");
    return;
  }

  String m = "📡 Último paquete recibido hace " + String(t.edadMs() / 1000) + " segundos.";
  if (t.rssiVigente())
    m += "\n" + lineaRssi(t);
  bot.sendMessage(chatId, m, "");
}

// --- Alertas ---

static void enviarAlerta(Alerta tipo, const String &texto)
{
  const bool enfriado = ultimaAlerta[tipo] == 0 || millis() - ultimaAlerta[tipo] > ENFRIAMIENTO_MS;
  if (!enfriado)
    return;

  bot.sendMessage(CHAT_ID, "⚠️ *ALERTA AEROSENSE:* " + texto, "Markdown");
  ultimaAlerta[tipo] = millis();
}

static void revisarAlertas()
{
  const Telemetria t = telSnapshot();

  if (t.hayPaquete() && t.edadMs() > SIN_SENAL_MS)
    enviarAlerta(ALERTA_SIN_SENAL, "¡Sin señal LoRa del globo desde hace más de 1 minuto!");

  if (t.rssiVigente() && t.rssiDbm < RSSI_BAJO_DBM)
    enviarAlerta(ALERTA_RSSI_BAJO, "Enlace LoRa débil: " + String(t.rssiDbm) + " dBm (" +
                                       calidadRssi(t.rssiDbm) + "). El globo se está alejando del alcance.");

  if (t.pmValido && t.pm25 > PM25_ALTO)
    enviarAlerta(ALERTA_PM25, "Nivel alto de partículas PM2.5 detectado: " + String(t.pm25) + " µg/m3");
}
