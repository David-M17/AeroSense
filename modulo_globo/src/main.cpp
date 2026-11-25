#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_ADXL345_U.h>
#include <ArduinoJson.h>
#include <PMS.h>

// Definición de pines UART
// Serial1: Sensor de partículas PMS7003M
#define PMS_RX 18 // Conectar al TX del sensor PMS7003M
#define PMS_TX 19 // Conectar al RX del sensor PMS7003M

// Serial2: Módulo LoRa E220
#define LORA_RX 16 // Conectar al TX del LoRa E220
#define LORA_TX 17 // Conectar al RX del LoRa E220
#define LORA_AUX 15

// Definición de pines para Sensores I2C / OneWire
#define DHTPIN 4
#define DHTTYPE DHT22

// Instancias de Puertos Seriales Hardware
HardwareSerial pmsSerial(1);
HardwareSerial loraSerial(2);

// Instancia del sensor PMS7003M
PMS pms(pmsSerial);
PMS::DATA pmsData;

// Instancias de Sensores Ambientales e Inerciales
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP085 bmp;
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);

// Contador de paquetes de telemetría
unsigned int packetId = 0;

// --- Lectura del PMS7003M ---
// La librería PMS procesa UN solo byte por llamada a read() y una trama completa mide
// 32 bytes, así que hay que vaciar el buffer UART en cada pasada del loop().
// Se conserva la última lectura válida; si no llegan tramas, se envía null (no 0).
int pm1 = 0, pm25 = 0, pm10 = 0;
bool pmValido = false;
unsigned long ultimaTramaPms = 0;
const unsigned long PMS_CADUCIDAD_MS = 10000; // lectura "vieja" tras 10 s sin tramas

// Control de tiempos de transmisión (sin delay() para no perder tramas del PMS)
unsigned long ultimoEnvio = 0;
const unsigned long INTERVALO_ENVIO_MS = 2000;

void leerPms()
{
  while (pmsSerial.available() > 0)
  {
    if (pms.read(pmsData))
    {
      pm1 = pmsData.PM_AE_UG_1_0;
      pm25 = pmsData.PM_AE_UG_2_5;
      pm10 = pmsData.PM_AE_UG_10_0;
      ultimaTramaPms = millis();
      pmValido = true;
    }
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==============================================");
  Serial.println("🛰️ INICIANDO PAYLOAD GLOBO AMARRADO (ESP32)");
  Serial.println("==============================================");

  // Inicialización de comunicaciones UART
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  loraSerial.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
  pinMode(LORA_AUX, INPUT);

  // Inicialización del sensor DHT22
  dht.begin();

  // Inicialización del sensor BMP180
  if (!bmp.begin())
  {
    Serial.println("ERROR: No se encontró el sensor BMP180 en I2C.");
  }
  else
  {
    Serial.println("BMP180 inicializado correctamente.");
  }

  // Inicialización del acelerómetro ADXL345
  if (!accel.begin())
  {
    Serial.println("ERROR: No se encontró el acelerómetro ADXL345 en I2C.");
  }
  else
  {
    accel.setRange(ADXL345_RANGE_16_G);
    Serial.println("ADXL345 inicializado correctamente.");
  }

  Serial.println("--- SENSOR PMS7003M EN MODO ACTIVO (UART 9600) ---");
  Serial.println("Sistema listo. Comenzando transmisión por LoRa...\n");
}

void loop()
{
  // 1. Vaciar siempre el buffer del PMS7003M (no bloquear el loop)
  leerPms();

  // 2. Transmitir cada INTERVALO_ENVIO_MS y solo si el módulo LoRa está libre (AUX en HIGH)
  if (millis() - ultimoEnvio < INTERVALO_ENVIO_MS || digitalRead(LORA_AUX) == LOW)
  {
    return;
  }
  ultimoEnvio = millis();

  // 3. Lectura de variables meteorológicas
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  // Lecturas barométricas
  float pres = bmp.readPressure() / 100.0; // Convertir Pa a hPa (millibares)
  float alt = bmp.readAltitude(101325);    // Altitud estimada con referencia Estándar (1013.25 hPa)

  // Manejo del error de lectura del DHT22
  if (isnan(temp) || isnan(hum))
  {
    temp = 0.0;
    hum = 0.0;
  }

  // 4. Lectura de Inercia y Aceleración en 3 Ejes
  sensors_event_t event;
  accel.getEvent(&event);

  // 5. Calidad del aire: válida solo si llegó una trama reciente del PMS7003M
  bool pmFresco = pmValido && (millis() - ultimaTramaPms < PMS_CADUCIDAD_MS);

  // 6. Construcción del JSON de Telemetría
  JsonDocument doc;
  doc["id"] = packetId++;
  doc["alt"] = round(alt * 10.0) / 10.0;
  doc["pres"] = round(pres * 10.0) / 10.0;
  doc["temp"] = round(temp * 10.0) / 10.0;
  doc["hum"] = (int)hum;

  // Sub-objeto para Material Particulado (null si el sensor no tiene lectura reciente)
  JsonObject pm = doc["pm"].to<JsonObject>();
  if (pmFresco)
  {
    pm["pm1"] = pm1;
    pm["pm25"] = pm25;
    pm["pm10"] = pm10;
  }
  else
  {
    pm["pm1"] = nullptr;
    pm["pm25"] = nullptr;
    pm["pm10"] = nullptr;
  }

  // Sub-objeto para Aceleración
  JsonObject acc = doc["acc"].to<JsonObject>();
  acc["x"] = round(event.acceleration.x * 100.0) / 100.0;
  acc["y"] = round(event.acceleration.y * 100.0) / 100.0;
  acc["z"] = round(event.acceleration.z * 100.0) / 100.0;

  // 7. Transmisión Serial por LoRa E220
  serializeJson(doc, loraSerial);
  loraSerial.println(); // Delimitador de paquete por salto de línea '\n'

  // Impresión en la consola local para depuración
  Serial.print("Enviado [LoRa JSON]: ");
  serializeJson(doc, Serial);
  Serial.println();
}
