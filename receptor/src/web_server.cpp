#include "web_server.h"

#include <WebServer.h>
#include <ArduinoJson.h>

#include "telemetria.h"
#include "web_page.h"

static WebServer server(80);

static void handleRoot()
{
  server.send_P(200, "text/html", PAGINA_HTML);
}

// JSON que consume la página cada 2 s. Los datos que no existen van como null.
static void handleData()
{
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

void webIniciar()
{
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void webAtender()
{
  server.handleClient();
}
