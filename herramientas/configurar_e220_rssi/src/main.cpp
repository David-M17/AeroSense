// Herramienta de un solo uso: activa "Enable RSSI byte" en el módulo E220-900T22D
// conectado al ESP32 del RECEPTOR (mismos pines que el firmware del receptor).
//
// Antes de subirla: apaga todo y pon M0 y M1 de ESE módulo en 3V3 (modo configuración).
// Cuando termine: apaga, regresa M0 y M1 a GND y vuelve a subir el firmware de receptor/.
#include <Arduino.h>

#define LORA_RX 16
#define LORA_TX 17
#define LORA_AUX 15

HardwareSerial loraSerial(2);

// Registros del E220 (lectura con C1 00 08):
//   0 ADDH | 1 ADDL | 2 REG0 (baudios/paridad/aire) | 3 REG1 (sub-paquete/potencia)
//   4 REG2 (canal) | 5 REG3 (bit7 = byte RSSI, bit6 = fija/transparente) | 6-7 CRYPT
const uint8_t REG3_IDX = 5;
const uint8_t BIT_RSSI = 0x80;

void vaciarRx()
{
  while (loraSerial.available() > 0)
    loraSerial.read();
}

bool esperarAux(unsigned long ms)
{
  unsigned long t = millis();
  while (digitalRead(LORA_AUX) == LOW)
  {
    if (millis() - t > ms)
      return false;
    delay(5);
  }
  return true;
}

// Espera hasta `primerByteMs` por el primer byte y luego hasta 100 ms de silencio.
size_t leerRespuesta(uint8_t *buf, size_t max, unsigned long primerByteMs)
{
  size_t n = 0;
  unsigned long t = millis();
  while (n < max)
  {
    if (loraSerial.available() > 0)
    {
      buf[n++] = loraSerial.read();
      t = millis();
    }
    else if (millis() - t > (n == 0 ? primerByteMs : 100UL))
    {
      break;
    }
  }
  return n;
}

void imprimirHex(const char *titulo, const uint8_t *d, size_t n)
{
  Serial.print(titulo);
  for (size_t i = 0; i < n; i++)
    Serial.printf(" %02X", d[i]);
  Serial.println();
}

bool enviarComando(const uint8_t *cmd, size_t n, uint8_t *resp, size_t &nResp)
{
  esperarAux(2000);
  vaciarRx();
  loraSerial.write(cmd, n);
  loraSerial.flush();
  nResp = leerRespuesta(resp, 16, 1000);
  return nResp > 0;
}

bool leerConfig(uint8_t reg[8])
{
  const uint8_t cmd[3] = {0xC1, 0x00, 0x08};
  uint8_t r[16];
  size_t n = 0;
  enviarComando(cmd, sizeof(cmd), r, n);
  if (n < 11 || r[0] != 0xC1 || r[1] != 0x00 || r[2] != 0x08)
  {
    imprimirHex("   Respuesta inesperada:", r, n);
    return false;
  }
  memcpy(reg, r + 3, 8);
  return true;
}

void mostrarConfig(const uint8_t reg[8])
{
  const uint32_t baudios[8] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
  imprimirHex("   Registros 00-07:", reg, 8);
  Serial.printf("   Dirección: 0x%02X%02X | Canal: %u | UART: %u baudios\n",
                reg[0], reg[1], reg[4], baudios[(reg[2] >> 5) & 0x07]);
  Serial.printf("   Transmisión: %s | Byte RSSI: %s\n",
                (reg[REG3_IDX] & 0x40) ? "FIJA" : "transparente",
                (reg[REG3_IDX] & BIT_RSSI) ? "ACTIVADO" : "desactivado");
}

void setup()
{
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Configurador E220: activar byte RSSI ===");

  // En modo configuración el E220 siempre usa 9600 8N1
  loraSerial.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
  pinMode(LORA_AUX, INPUT);
  if (!esperarAux(3000))
    Serial.println("Aviso: AUX sigue en LOW; continúo de todos modos.");

  Serial.println("1) Leyendo configuración actual...");
  uint8_t reg[8];
  if (!leerConfig(reg))
  {
    Serial.println("SIN RESPUESTA DEL MÓDULO. Revisa que:");
    Serial.println("  - M0 y M1 de este módulo estén en 3V3 (no en GND) y hayas reiniciado");
    Serial.println("  - TX/RX/AUX estén en los pines 17/16/15 como en el receptor");
    return;
  }
  mostrarConfig(reg);

  if (reg[REG3_IDX] & BIT_RSSI)
  {
    Serial.println("\nEl byte RSSI YA estaba activado. No hay nada que cambiar.");
  }
  else
  {
    Serial.println("\n2) Activando byte RSSI (solo se cambia el bit 7 del registro 05)...");
    uint8_t nuevo = reg[REG3_IDX] | BIT_RSSI;
    uint8_t r[16];
    size_t n = 0;

    // Intento A: escribir solo el registro 05 (guardado permanente)
    const uint8_t cmdA[4] = {0xC0, 0x05, 0x01, nuevo};
    enviarComando(cmdA, sizeof(cmdA), r, n);
    imprimirHex("   Respuesta:", r, n);

    uint8_t chk[8];
    bool ok = leerConfig(chk) && (chk[REG3_IDX] & BIT_RSSI);

    if (!ok)
    {
      // Intento B: reescribir los 8 registros tal como se leyeron, con el bit 7 activado
      Serial.println("   Intento A no surtió efecto; probando escritura completa...");
      uint8_t cmdB[11] = {0xC0, 0x00, 0x08};
      memcpy(cmdB + 3, reg, 8);
      cmdB[3 + REG3_IDX] = nuevo;
      enviarComando(cmdB, sizeof(cmdB), r, n);
      imprimirHex("   Respuesta:", r, n);
      ok = leerConfig(chk) && (chk[REG3_IDX] & BIT_RSSI);
    }

    Serial.println("\n3) Verificación:");
    if (ok)
    {
      mostrarConfig(chk);
      Serial.println("\nLISTO: byte RSSI ACTIVADO y guardado.");
    }
    else
    {
      Serial.println("NO se pudo activar. Copia estas líneas y mándamelas.");
      return;
    }
  }

  Serial.println("\nSiguiente paso:");
  Serial.println("  - Apaga todo, regresa M0 y M1 a GND");
  Serial.println("  - Vuelve a subir el firmware de la carpeta receptor/");
}

void loop()
{
  delay(1000);
}
