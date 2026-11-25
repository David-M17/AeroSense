#include "web_server.h"

#include <WebServer.h>
#include <ArduinoJson.h>

#include "telemetria.h"
#include "web_page.h"
#include "modelo_glb.h"

static WebServer server(80);

static void handleRoot()
{
  server.send_P(200, "text/html", PAGINA_HTML);
}

// JSON que consume la página cada 2 s. Los datos que no existen van como null.
static void handleData()
{
  const Telemetria tel = telSnapshot(); // copia consistente (la escribe la tarea LoRa)
  JsonDocument doc;

  if (tel.ultimaRecepcion == 0)
  {
    // Todavía no llega ningún paquete del globo
    doc["edad"] = nullptr;
    doc["rssi"] = nullptr;
  }
  else
  {
    doc["id"] = tel.id;
    doc["alt"] = tel.alt;
    if (tel.alturaValida)
      doc["h"] = tel.altura;
    else
      doc["h"] = nullptr;
    doc["pres"] = tel.pres;
    doc["temp"] = tel.temp;
    doc["hum"] = tel.hum;

    JsonObject pm = doc["pm"].to<JsonObject>();
    if (tel.pmValido)
    {
      pm["pm1"] = tel.pm1;
      pm["pm25"] = tel.pm25;
      pm["pm10"] = tel.pm10;
    }
    else
    {
      pm["pm1"] = nullptr;
      pm["pm25"] = nullptr;
      pm["pm10"] = nullptr;
    }

    JsonObject acc = doc["acc"].to<JsonObject>();
    acc["x"] = tel.accX;
    acc["y"] = tel.accY;
    acc["z"] = tel.accZ;

    if (tel.rssiVigente())
      doc["rssi"] = tel.rssiDbm;
    else
      doc["rssi"] = nullptr;

    doc["edad"] = (millis() - tel.ultimaRecepcion) / 1000; // segundos desde el último paquete
  }

  String salida;
  serializeJson(doc, salida);
  server.send(200, "application/json", salida);
}

// Modelo 3D del módulo para el visor de actitud. Va comprimido con gzip en flash y el
// navegador lo descomprime solo; se cachea un día para no volver a pedirlo en cada recarga.
static void handleModelo()
{
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send_P(200, "model/gltf-binary", (const char *)MODELO_GLB_GZ, MODELO_GLB_GZ_LEN);
}

// El navegador pide el ícono solo; se responde "sin contenido" para no llenar el log de errores
static void handleFavicon()
{
  server.send(204);
}

static void handleNoEncontrado()
{
  server.send(404, "text/plain", "No encontrado");
}

void webIniciar()
{
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/modelo.glb", handleModelo);
  server.on("/favicon.ico", handleFavicon);
  server.onNotFound(handleNoEncontrado);
  server.begin();
}

void webAtender()
{
  server.handleClient();
}
