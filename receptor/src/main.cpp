// AeroSense - Estación terrestre (receptor)
//
// Módulos:
//   lora_rx      recibe y decodifica los paquetes del globo (tarea propia)
//   telemetria   estado compartido y protegido con mutex
//   web_server   panel web con gráficas (se atiende desde loop())
//   telegram_bot comandos y alertas (tarea propia)
//   nube         publicación en Blynk y ThingSpeak (una tarea por servicio)

#include <Arduino.h>
#include <WiFi.h>

#include "secrets.h" // WIFI_SSID, WIFI_PASSWORD
#include "lora_rx.h"
#include "nube.h"
#include "telegram_bot.h"
#include "telemetria.h"
#include "web_server.h"

// Si el panel web bloquea el loop más de esto, se avisa por Serial (diagnóstico)
static constexpr unsigned long PANEL_LENTO_MS = 1500;

static void conectarWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Conectando a Wi-Fi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n¡Conectado a Wi-Fi con éxito!");
  Serial.print("🌐 Panel Web en Vivo: http://");
  Serial.println(WiFi.localIP());
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==============================================");
  Serial.println("            ESTACIÓN TERRESTRE                ");
  Serial.println("==============================================");

  telIniciar();  // primero: las demás tareas dependen del mutex
  loraIniciar(); // antes del Wi-Fi: no se pierde ningún paquete mientras conecta

  conectarWifi();
  webIniciar();
  telegramIniciar();
  nubeIniciar();
}

void loop()
{
  const unsigned long inicio = millis();
  webAtender();
  const unsigned long duracion = millis() - inicio;
  if (duracion > PANEL_LENTO_MS)
    Serial.printf("⏱ El panel web bloqueó el loop %lu ms\n", duracion);

  delay(2);
}
