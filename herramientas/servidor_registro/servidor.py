#!/usr/bin/env python3
"""AeroSense - servidor de registro.

Recibe por HTTP lo que manda la ESP32 receptora y lo guarda en SQLite (dos tablas:
`registros` y `errores`). Solo usa la biblioteca estándar de Python 3.8+: no hay que instalar nada.

    python servidor.py                 # escucha en 0.0.0.0:8000, base en registros.db
    python servidor.py --puerto 8080 --base otra.db

Visor: http://localhost:8000      CSV: /exportar/registros.csv y /exportar/errores.csv
"""
import argparse
import csv
import io
import json
import re
import sqlite3
import time
import threading
import traceback
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

AQUI = Path(__file__).resolve().parent
FORMATO_HORA = "%Y-%m-%d %H:%M:%S"

ESQUEMA = """
CREATE TABLE IF NOT EXISTS registros (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  recibido_en  TEXT    NOT NULL,   -- hora de la PC en que llegó el paquete al receptor
  sesion       INTEGER NOT NULL,   -- sube cada vez que el transmisor reinicia (su id vuelve a 0)
  id_paquete   INTEGER NOT NULL,   -- el "id" del JSON del transmisor
  alt REAL, pres REAL, temp REAL, hum INTEGER,
  pm1 INTEGER, pm25 INTEGER, pm10 INTEGER,  -- NULL si el PMS7003M no tenía lectura
  acc_x REAL, acc_y REAL, acc_z REAL,
  rssi_dbm     INTEGER,
  UNIQUE (sesion, id_paquete)      -- un reintento no duplica el registro
);
CREATE TABLE IF NOT EXISTS errores (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  ocurrido_en  TEXT NOT NULL,
  origen       TEXT NOT NULL,      -- lora, nube, telegram, sensor, receptor, servidor
  tipo         TEXT NOT NULL,      -- código corto: paquetes_perdidos, blynk_fallo...
  detalle      TEXT,
  cantidad     INTEGER NOT NULL DEFAULT 1   -- cuántos paquetes/eventos representa el renglón
);
CREATE INDEX IF NOT EXISTS ix_registros_hora ON registros (recibido_en);
CREATE INDEX IF NOT EXISTS ix_errores_hora ON errores (ocurrido_en);
"""

# Ocurrió en el receptor (pérdida real de radio) vs. visto por el servidor (huecos de cualquier causa)
TIPO_RADIO = "paquetes_perdidos"        # el receptor vio saltar el id: el paquete nunca llegó por LoRa
TIPO_COLA = "registros_descartados"     # el receptor los recibió pero no pudo mandarlos a la PC a tiempo
TIPO_HUECO = "huecos_en_registros"      # el servidor ve ids que faltan (radio + cola)
LENTA_S = 0.5                           # una petición que tarde más se anota en la consola


def hora_de(hace_ms):
    """Hora local en que ocurrió algo que la ESP32 reporta como 'hace N ms'."""
    return (datetime.now() - timedelta(milliseconds=max(0, int(hace_ms)))).strftime(FORMATO_HORA)


class Almacen:
    """Toda la lógica de la base. Un candado serializa las escrituras (el servidor es multihilo);
    cada lectura abre su propia conexión para no competir con ellas."""

    def __init__(self, ruta):
        self._ruta = ruta
        self._con = self._conectar()
        self._con.executescript(ESQUEMA)
        self._migrar()
        self._candado = threading.Lock()
        ultimo = self._con.execute(
            "SELECT sesion, id_paquete FROM registros ORDER BY id DESC LIMIT 1").fetchone()
        self._sesion = ultimo["sesion"] if ultimo else 0
        self._ultimo_id = ultimo["id_paquete"] if ultimo else None

    def _conectar(self):
        con = sqlite3.connect(self._ruta, timeout=5, check_same_thread=False)
        con.row_factory = sqlite3.Row
        con.execute("PRAGMA journal_mode=WAL")
        con.execute("PRAGMA synchronous=NORMAL")  # con WAL basta y evita un fsync por cada commit
        return con

    def _migrar(self):
        """Bases de la primera versión: sin `cantidad`, y con los huecos anotados como si fueran
        pérdida real del radio. Se corrige para no mezclar las dos cosas."""
        columnas = [c["name"] for c in self._con.execute("PRAGMA table_info(errores)")]
        if "cantidad" in columnas:
            return
        copia = sqlite3.connect(self._ruta + ".respaldo")  # por si algo sale mal: la base original queda intacta
        self._con.backup(copia)
        copia.close()
        print(f"  Base de la version anterior: copia de seguridad en {self._ruta}.respaldo")
        self._con.execute("ALTER TABLE errores ADD COLUMN cantidad INTEGER NOT NULL DEFAULT 1")
        for fila in self._con.execute("SELECT id, detalle FROM errores WHERE tipo = ?", (TIPO_RADIO,)).fetchall():
            m = re.search(r"faltaron (\d+)", fila["detalle"] or "")
            self._con.execute("UPDATE errores SET origen='servidor', tipo=?, cantidad=? WHERE id=?",
                              (TIPO_HUECO, int(m.group(1)) if m else 1, fila["id"]))
        self._con.execute("UPDATE errores SET tipo='registros_descartados', "
                          "cantidad=1 WHERE tipo='cola_llena'")
        self._con.commit()

    # --- escritura -------------------------------------------------------------------------

    def guardar_lote(self, paquetes, errores):
        """Guarda todo en una sola transacción. Un elemento defectuoso se anota y se salta: nunca
        debe impedir que el resto (ni los siguientes reintentos) se guarden. Devuelve (nuevos, rechazados)."""
        nuevos = rechazados = 0
        with self._candado:
            for e in errores:
                try:
                    self._insertar_error(hora_de(e.get("hace_ms", 0)), str(e["origen"])[:20],
                                         str(e["tipo"])[:40], str(e.get("detalle", ""))[:300],
                                         int(e.get("cantidad", 1)))
                except (ValueError, KeyError, TypeError, AttributeError):
                    rechazados += 1
            for p in paquetes:
                try:
                    nuevos += self._guardar_paquete(p)
                except (ValueError, KeyError, TypeError, AttributeError) as ex:
                    rechazados += 1
                    self._insertar_error(hora_de(0), "servidor", "paquete_invalido", f"{ex!r}"[:200], 1)
            self._con.commit()
        return nuevos, rechazados

    def _guardar_paquete(self, d):
        """Devuelve 1 si el paquete era nuevo y 0 si ya estaba (reintento tras una respuesta perdida)."""
        id_paquete = int(d["id"])
        hora = hora_de(d.get("hace_ms", 0))

        # Un reintento (la respuesta se perdió pero el paquete sí se guardó) trae el mismo id y la misma
        # hora de llegada. Un transmisor reiniciado repite ids, pero minutos después, no en el mismo instante.
        t = datetime.strptime(hora, FORMATO_HORA)
        ventana = ((t - timedelta(seconds=5)).strftime(FORMATO_HORA), (t + timedelta(seconds=5)).strftime(FORMATO_HORA))
        if self._con.execute("SELECT 1 FROM registros WHERE id_paquete=? AND recibido_en BETWEEN ? AND ?",
                             (id_paquete, *ventana)).fetchone():
            return 0

        sesion = self._sesion
        if self._ultimo_id is not None and id_paquete < self._ultimo_id:
            sesion += 1  # el id retrocedió: el transmisor se reinició

        if sesion != self._sesion:
            self._sesion = sesion
        elif self._ultimo_id is not None and id_paquete > self._ultimo_id + 1:
            faltan = id_paquete - self._ultimo_id - 1
            self._insertar_error(hora, "servidor", TIPO_HUECO,
                                 f"faltan {faltan} registro(s) entre el id {self._ultimo_id} y el {id_paquete}", faltan)

        pm = d.get("pm") or {}
        acc = d.get("acc") or {}
        temp, hum = d.get("temp"), d.get("hum")
        if temp == 0 and hum == 0:  # el transmisor manda 0/0 cuando falla el DHT22
            temp = hum = None
            self._insertar_error(hora, "sensor", "dht22_sin_lectura", f"paquete {id_paquete}", 1)

        self._con.execute(
            "INSERT INTO registros (recibido_en, sesion, id_paquete, alt, pres, temp, hum,"
            " pm1, pm25, pm10, acc_x, acc_y, acc_z, rssi_dbm) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (hora, sesion, id_paquete, d.get("alt"), d.get("pres"), temp, hum,
             pm.get("pm1"), pm.get("pm25"), pm.get("pm10"),
             acc.get("x"), acc.get("y"), acc.get("z"), d.get("rssi")))
        self._ultimo_id = id_paquete
        return 1

    def _insertar_error(self, hora, origen, tipo, detalle, cantidad):
        self._con.execute("INSERT INTO errores (ocurrido_en, origen, tipo, detalle, cantidad) VALUES (?,?,?,?,?)",
                          (hora, origen, tipo, detalle, cantidad))

    # --- lectura (conexión propia en cada consulta) ----------------------------------------

    def _leer(self, sql, params=()):
        con = self._conectar()
        try:
            return [dict(f) for f in con.execute(sql, params)]
        finally:
            con.close()

    def ultimos(self, tabla, n):
        orden = "recibido_en" if tabla == "registros" else "ocurrido_en"
        return self._leer(f"SELECT * FROM {tabla} ORDER BY {orden} DESC, id DESC LIMIT ?", (n,))

    def todo(self, tabla):
        return self._leer(f"SELECT * FROM {tabla} ORDER BY id")

    def resumen(self):
        sesiones = []
        for s in self._leer(
                "SELECT sesion, COUNT(*) AS recibidos, MIN(id_paquete) AS primero, MAX(id_paquete) AS ultimo,"
                " MIN(recibido_en) AS desde, MAX(recibido_en) AS hasta FROM registros GROUP BY sesion ORDER BY sesion"):
            esperados = s["ultimo"] - s["primero"] + 1
            faltan = esperados - s["recibidos"]
            sesiones.append({**s, "esperados": esperados, "faltantes": faltan,
                             "faltantes_pct": round(100 * faltan / esperados, 1)})
        errores = self._leer(
            "SELECT origen, tipo, SUM(cantidad) AS cantidad, COUNT(*) AS veces, MAX(ocurrido_en) AS ultimo"
            " FROM errores GROUP BY origen, tipo ORDER BY veces DESC")
        suma = {f["tipo"]: f["cantidad"] for f in errores}
        totales = {"perdidos_radio": suma.get(TIPO_RADIO, 0), "descartados_cola": suma.get(TIPO_COLA, 0)}
        return {"sesiones": sesiones, "errores": errores, "totales": totales}


PAGINA = """<!doctype html><html lang="es"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>AeroSense - registro</title>
<style>
 body{font:14px system-ui,sans-serif;margin:20px;background:#0f1520;color:#e6edf5}
 h1{font-size:20px} h2{font-size:15px;margin-top:26px;color:#8fb4e8}
 table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
 th,td{padding:4px 8px;border-bottom:1px solid #243044;text-align:right;white-space:nowrap}
 th{color:#8fb4e8;font-weight:600} td:first-child,th:first-child,td.t,th.t{text-align:left}
 .wrap{overflow-x:auto} a{color:#6cb2ff} .malo{color:#ff8a8a} .ok{color:#7be0a0} small{color:#7f8da3}
</style>
<h1>AeroSense · registro en la PC</h1>
<small>Se actualiza cada 5 s · <a href="/exportar/registros.csv">registros.csv</a> ·
<a href="/exportar/errores.csv">errores.csv</a></small>
<h2>Enlace por sesión</h2><div class="wrap"><table id="ses"></table></div>
<p id="tot"></p>
<h2>Errores por tipo</h2><div class="wrap"><table id="err"></table></div>
<h2>Últimos registros</h2><div class="wrap"><table id="reg"></table></div>
<h2>Últimos errores</h2><div class="wrap"><table id="ult"></table></div>
<script>
const f=(v,d)=>v===null||v===undefined?'—':(typeof v==='number'&&d!==undefined?v.toFixed(d):v);
function tabla(id,cols,filas){document.getElementById(id).innerHTML=
 '<tr>'+cols.map(c=>'<th'+(c[2]?' class="t"':'')+'>'+c[0]+'</th>').join('')+'</tr>'+
 filas.map(r=>'<tr>'+cols.map(c=>'<td'+(c[2]?' class="t"':'')+'>'+(c[3]?c[3](r):f(r[c[1]],c[4]))+'</td>').join('')+'</tr>').join('')}
async function cargar(){
 const [r,e,s]=await Promise.all(['/api/registros?n=30','/api/errores?n=30','/api/resumen'].map(u=>fetch(u).then(x=>x.json())));
 tabla('ses',[['Sesión','sesion'],['Desde','desde',1],['Hasta','hasta',1],['Registros en la PC','recibidos'],['Esperados','esperados'],
  ['Faltan','faltantes'],['Faltan %','faltantes_pct',0,x=>'<span class="'+(x.faltantes_pct>5?'malo':'ok')+'">'+x.faltantes_pct+'</span>']],s.sesiones);
 document.getElementById('tot').innerHTML='Perdidos de verdad por el radio (los cuenta el receptor): <b>'+s.totales.perdidos_radio+
  '</b> · Descartados por el receptor porque la PC no alcanzó a recibirlos: <b>'+s.totales.descartados_cola+'</b><br><small>«Faltan» suma las dos causas.</small>';
 tabla('err',[['Origen','origen',1],['Tipo','tipo',1],['Veces','veces'],['Cantidad','cantidad'],['Última vez','ultimo',1]],s.errores);
 tabla('reg',[['Hora','recibido_en',1],['Ses','sesion'],['Id','id_paquete'],['Alt m','alt',0,0,1],['hPa','pres',0,0,1],['°C','temp',0,0,1],
  ['%HR','hum'],['PM1','pm1'],['PM2.5','pm25'],['PM10','pm10'],['ax','acc_x',0,0,2],['ay','acc_y',0,0,2],['az','acc_z',0,0,2],['RSSI','rssi_dbm']],r);
 tabla('ult',[['Hora','ocurrido_en',1],['Origen','origen',1],['Tipo','tipo',1],['Cant.','cantidad'],['Detalle','detalle',1]],e);
}
cargar();setInterval(cargar,5000);
</script></html>"""


def crear_manejador(almacen):
    class Manejador(BaseHTTPRequestHandler):
        timeout = 10  # un cliente que se cuelga no debe dejar un hilo esperando para siempre

        def log_message(self, formato, *args):  # silencia el log por petición
            pass

        def handle(self):
            try:
                super().handle()
            except (ConnectionError, TimeoutError):
                pass  # la ESP32 cortó la conexión (Wi-Fi, reinicio): no es un fallo del servidor

        def _responder(self, codigo, cuerpo, tipo="application/json"):
            datos = cuerpo.encode("utf-8") if isinstance(cuerpo, str) else cuerpo
            self.send_response(codigo)
            self.send_header("Content-Type", tipo + "; charset=utf-8")
            self.send_header("Content-Length", str(len(datos)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(datos)

        def _csv(self, tabla):
            filas = almacen.todo(tabla)
            salida = io.StringIO()
            if filas:
                escritor = csv.DictWriter(salida, fieldnames=list(filas[0]))
                escritor.writeheader()
                escritor.writerows(filas)
            self._responder(200, "\ufeff" + salida.getvalue(), "text/csv")  # BOM: Excel lee bien los acentos

        def do_GET(self):
            url = urlparse(self.path)
            try:
                n = min(max(int(parse_qs(url.query).get("n", ["100"])[0]), 1), 5000)
            except ValueError:
                n = 100
            if url.path == "/":
                self._responder(200, PAGINA, "text/html")
            elif url.path == "/api/registros":
                self._responder(200, json.dumps(almacen.ultimos("registros", n)))
            elif url.path == "/api/errores":
                self._responder(200, json.dumps(almacen.ultimos("errores", n)))
            elif url.path == "/api/resumen":
                self._responder(200, json.dumps(almacen.resumen()))
            elif url.path == "/exportar/registros.csv":
                self._csv("registros")
            elif url.path == "/exportar/errores.csv":
                self._csv("errores")
            else:
                self._responder(404, '{"error":"no existe"}')

        def do_POST(self):
            inicio = time.monotonic()
            try:
                largo = int(self.headers.get("Content-Length", 0))
                crudo = self.rfile.read(largo) or b"{}"
                leido = time.monotonic() - inicio
                datos = json.loads(crudo)
                if self.path == "/api/lote":
                    nuevos, rechazados = almacen.guardar_lote(datos.get("paquetes", []), datos.get("errores", []))
                    cuerpo = {"ok": True, "nuevos": nuevos, "rechazados": rechazados}
                elif self.path == "/api/registro":
                    nuevos, rechazados = almacen.guardar_lote([datos], [])
                    cuerpo = {"ok": True, "nuevos": nuevos, "rechazados": rechazados}
                elif self.path == "/api/error":
                    _, rechazados = almacen.guardar_lote([], [datos])
                    cuerpo = {"ok": True, "rechazados": rechazados}
                else:
                    self._responder(404, '{"error":"no existe"}')
                    return
                self._responder(200, json.dumps(cuerpo))
                total = time.monotonic() - inicio
                if total > LENTA_S:
                    print(f"[{datetime.now():%H:%M:%S}] peticion lenta {self.path}: {total:.2f} s "
                          f"(leer cuerpo {leido:.2f} s, guardar+responder {total - leido:.2f} s)")
            except (ConnectionError, TimeoutError):
                raise  # lo silencia handle()
            except (ValueError, KeyError, TypeError, AttributeError) as e:
                self._responder(400, json.dumps({"error": f"peticion invalida: {e}"}))
            except Exception:  # cualquier otro fallo: respuesta explícita en vez de cortar la conexión
                print(f"[{datetime.now():%H:%M:%S}] ERROR en {self.path}:\n{traceback.format_exc()}")
                self._responder(500, '{"error":"fallo interno del servidor"}')

    return Manejador


def main():
    ap = argparse.ArgumentParser(description="Servidor de registro de AeroSense")
    ap.add_argument("--puerto", type=int, default=8000)
    ap.add_argument("--base", default=str(AQUI / "registros.db"))
    args = ap.parse_args()

    almacen = Almacen(args.base)
    servidor = ThreadingHTTPServer(("0.0.0.0", args.puerto), crear_manejador(almacen))
    print(f"AeroSense registro escuchando en el puerto {args.puerto}")
    print(f"  Base de datos: {args.base}")
    print(f"  Visor:         http://localhost:{args.puerto}")
    print("  Ctrl+C para detener")
    try:
        servidor.serve_forever()
    except KeyboardInterrupt:
        print("\nDetenido.")


if __name__ == "__main__":
    main()
