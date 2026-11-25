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

// --- Presión y altura ---
// Una sola lectura del BMP180 varía ±0.1 hPa (cerca de ±1 m). Se toma una muestra cada 100 ms y
// se promedian todas las del intervalo entre envíos (~20); el promedio además pasa por un filtro
// exponencial. Durante los primeros segundos tras encender se mide la presión del suelo: la
// "altura" que se envía es la diferencia respecto a ese punto. Hay que encender el módulo en el
// suelo y dejarlo quieto mientras calibra.
const unsigned long MUESTREO_BARO_MS = 100;
const unsigned long CALIBRACION_MS = 10000;
const float ALFA_FILTRO = 0.4; // 1 = sin filtro; más bajo = más suave pero más lento
const float PRESION_NIVEL_MAR_HPA = 1013.25;
bool bmpOk = false;
unsigned long ultimaMuestraBaro = 0;
float sumaPresion = 0; // muestras desde el último envío
unsigned int nMuestras = 0;
float presionFiltrada = NAN; // hPa
float presionSuelo = NAN;    // hPa; NAN mientras calibra
float sumaCalibracion = 0;
unsigned int nCalibracion = 0;

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

void muestrearBarometro()
{
  if (!bmpOk || millis() - ultimaMuestraBaro < MUESTREO_BARO_MS)
    return;
  ultimaMuestraBaro = millis();

  const float p = bmp.readPressure() / 100.0; // Pa -> hPa
  sumaPresion += p;
  nMuestras++;

  if (isnan(presionSuelo))
  {
    sumaCalibracion += p;
    nCalibracion++;
    if (millis() >= CALIBRACION_MS)
    {
      presionSuelo = sumaCalibracion / nCalibracion;
      Serial.printf("Referencia del suelo: %.2f hPa (%u muestras)\n", presionSuelo, nCalibracion);
    }
  }
}

// Cierra el intervalo entre envíos: promedia sus muestras y actualiza el filtro.
void actualizarPresion()
{
  if (nMuestras == 0)
    return;
  const float promedio = sumaPresion / nMuestras;
  sumaPresion = 0;
  nMuestras = 0;
  presionFiltrada = isnan(presionFiltrada) ? promedio : presionFiltrada + ALFA_FILTRO * (promedio - presionFiltrada);
}

// Fórmula barométrica internacional: altura entre una presión de referencia y la actual.
float alturaDesde(float presionRef, float presion)
{
  return 44330.0 * (1.0 - pow(presion / presionRef, 0.1903));
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
    bmpOk = true;
    Serial.println("BMP180 inicializado correctamente.");
    Serial.println("Calibrando la altura del suelo: deja el modulo quieto 10 s...");
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
  muestrearBarometro();

  // 2. Transmitir cada INTERVALO_ENVIO_MS y solo si el módulo LoRa está libre (AUX en HIGH)
  if (millis() - ultimoEnvio < INTERVALO_ENVIO_MS || digitalRead(LORA_AUX) == LOW)
  {
    return;
  }
  ultimoEnvio = millis();

  // 3. Lectura de variables meteorológicas
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  // Lecturas barométricas (promediadas y filtradas)
  actualizarPresion();
  const float pres = presionFiltrada;
  const float alt = alturaDesde(PRESION_NIVEL_MAR_HPA, pres); // msnm, con la atmósfera estándar

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
  doc["pres"] = round(pres * 100.0) / 100.0;
  if (isnan(presionSuelo))
    doc["h"] = nullptr; // aún calibrando
  else
    doc["h"] = round(alturaDesde(presionSuelo, pres) * 10.0) / 10.0; // altura sobre el suelo (m)
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
