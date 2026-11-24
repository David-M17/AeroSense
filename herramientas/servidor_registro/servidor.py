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
import sqlite3
import threading
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
  origen       TEXT NOT NULL,      -- lora, nube, telegram, sensor, receptor
  tipo         TEXT NOT NULL,      -- código corto: paquetes_perdidos, blynk_fallo...
  detalle      TEXT
);
CREATE INDEX IF NOT EXISTS ix_registros_hora ON registros (recibido_en);
CREATE INDEX IF NOT EXISTS ix_errores_hora ON errores (ocurrido_en);
"""


def hora_de(hace_ms):
    """Hora local en que ocurrió algo que la ESP32 reporta como 'hace N ms'."""
    return (datetime.now() - timedelta(milliseconds=max(0, int(hace_ms)))).strftime(FORMATO_HORA)


class Almacen:
    """Toda la lógica de la base. Un candado serializa las escrituras (el servidor es multihilo)."""

    def __init__(self, ruta):
        self._con = sqlite3.connect(ruta, check_same_thread=False)
        self._con.row_factory = sqlite3.Row
        self._con.execute("PRAGMA journal_mode=WAL")
        self._con.executescript(ESQUEMA)
        self._candado = threading.Lock()
        ultimo = self._con.execute(
            "SELECT sesion, id_paquete FROM registros ORDER BY id DESC LIMIT 1").fetchone()
        self._sesion = ultimo["sesion"] if ultimo else 0
        self._ultimo_id = ultimo["id_paquete"] if ultimo else None

    # --- escritura -------------------------------------------------------------------------

    def guardar_registro(self, d):
        """Guarda un paquete. Devuelve True si era nuevo, False si ya estaba (reintento)."""
        with self._candado:
            id_paquete = int(d["id"])
            hora = hora_de(d.get("hace_ms", 0))

            if self._ultimo_id is not None and id_paquete < self._ultimo_id:
                self._sesion += 1  # el id retrocedió: el transmisor se reinició
            elif self._ultimo_id is not None and id_paquete > self._ultimo_id + 1:
                faltan = id_paquete - self._ultimo_id - 1
                self._insertar_error(hora, "lora", "paquetes_perdidos",
                                     f"faltaron {faltan} paquete(s) entre el id {self._ultimo_id} y el {id_paquete}")

            pm = d.get("pm") or {}
            acc = d.get("acc") or {}
            temp, hum = d.get("temp"), d.get("hum")
            if temp == 0 and hum == 0:  # el transmisor manda 0/0 cuando falla el DHT22
                temp = hum = None
                self._insertar_error(hora, "sensor", "dht22_sin_lectura", f"paquete {id_paquete}")

            cur = self._con.execute(
                "INSERT OR IGNORE INTO registros (recibido_en, sesion, id_paquete, alt, pres, temp, hum,"
                " pm1, pm25, pm10, acc_x, acc_y, acc_z, rssi_dbm) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (hora, self._sesion, id_paquete, d.get("alt"), d.get("pres"), temp, hum,
                 pm.get("pm1"), pm.get("pm25"), pm.get("pm10"),
                 acc.get("x"), acc.get("y"), acc.get("z"), d.get("rssi")))
            self._con.commit()
            if cur.rowcount:
                self._ultimo_id = id_paquete
            return bool(cur.rowcount)

    def guardar_error(self, d):
        with self._candado:
            self._insertar_error(hora_de(d.get("hace_ms", 0)), str(d["origen"])[:20],
                                 str(d["tipo"])[:40], str(d.get("detalle", ""))[:300])
            self._con.commit()

    def _insertar_error(self, hora, origen, tipo, detalle):
        self._con.execute("INSERT INTO errores (ocurrido_en, origen, tipo, detalle) VALUES (?,?,?,?)",
                          (hora, origen, tipo, detalle))

    # --- lectura ---------------------------------------------------------------------------

    def ultimos(self, tabla, n):
        orden = "recibido_en" if tabla == "registros" else "ocurrido_en"
        filas = self._con.execute(f"SELECT * FROM {tabla} ORDER BY {orden} DESC, id DESC LIMIT ?", (n,))
        return [dict(f) for f in filas]

    def todo(self, tabla):
        return [dict(f) for f in self._con.execute(f"SELECT * FROM {tabla} ORDER BY id")]

    def resumen(self):
        sesiones = []
        for s in self._con.execute(
                "SELECT sesion, COUNT(*) AS recibidos, MIN(id_paquete) AS primero, MAX(id_paquete) AS ultimo,"
                " MIN(recibido_en) AS desde, MAX(recibido_en) AS hasta FROM registros GROUP BY sesion ORDER BY sesion"):
            esperados = s["ultimo"] - s["primero"] + 1
            sesiones.append({**dict(s), "esperados": esperados, "perdidos": esperados - s["recibidos"],
                             "perdida_pct": round(100 * (esperados - s["recibidos"]) / esperados, 1)})
        errores = [dict(f) for f in self._con.execute(
            "SELECT origen, tipo, COUNT(*) AS veces, MAX(ocurrido_en) AS ultimo FROM errores"
            " GROUP BY origen, tipo ORDER BY veces DESC")]
        return {"sesiones": sesiones, "errores": errores}


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
 tabla('ses',[['Sesión','sesion'],['Desde','desde',1],['Hasta','hasta',1],['Recibidos','recibidos'],['Esperados','esperados'],
  ['Perdidos','perdidos'],['Pérdida %','perdida_pct',0,x=>'<span class="'+(x.perdida_pct>5?'malo':'ok')+'">'+x.perdida_pct+'</span>']],s.sesiones);
 tabla('err',[['Origen','origen',1],['Tipo','tipo',1],['Veces','veces'],['Última vez','ultimo',1]],s.errores);
 tabla('reg',[['Hora','recibido_en',1],['Ses','sesion'],['Id','id_paquete'],['Alt m','alt',0,0,1],['hPa','pres',0,0,1],['°C','temp',0,0,1],
  ['%HR','hum'],['PM1','pm1'],['PM2.5','pm25'],['PM10','pm10'],['ax','acc_x',0,0,2],['ay','acc_y',0,0,2],['az','acc_z',0,0,2],['RSSI','rssi_dbm']],r);
 tabla('ult',[['Hora','ocurrido_en',1],['Origen','origen',1],['Tipo','tipo',1],['Detalle','detalle',1]],e);
}
cargar();setInterval(cargar,5000);
</script></html>"""


def crear_manejador(almacen):
    class Manejador(BaseHTTPRequestHandler):
        def log_message(self, formato, *args):  # silencia el log por petición
            pass

        def _responder(self, codigo, cuerpo, tipo="application/json"):
            datos = cuerpo.encode("utf-8") if isinstance(cuerpo, str) else cuerpo
            self.send_response(codigo)
            self.send_header("Content-Type", tipo + "; charset=utf-8")
            self.send_header("Content-Length", str(len(datos)))
            self.end_headers()
            self.wfile.write(datos)

        def _csv(self, tabla):
            filas = almacen.todo(tabla)
            salida = io.StringIO()
            if filas:
                escritor = csv.DictWriter(salida, fieldnames=list(filas[0]))
                escritor.writeheader()
                escritor.writerows(filas)
            self._responder(200, "﻿" + salida.getvalue(), "text/csv")  # BOM: Excel lee bien los acentos

        def do_GET(self):
            url = urlparse(self.path)
            n = min(max(int(parse_qs(url.query).get("n", ["100"])[0]), 1), 5000)
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
            try:
                largo = int(self.headers.get("Content-Length", 0))
                datos = json.loads(self.rfile.read(largo) or b"{}")
                if self.path == "/api/registro":
                    nuevo = almacen.guardar_registro(datos)
                    self._responder(200, json.dumps({"ok": True, "nuevo": nuevo}))
                elif self.path == "/api/error":
                    almacen.guardar_error(datos)
                    self._responder(200, '{"ok":true}')
                else:
                    self._responder(404, '{"error":"no existe"}')
            except (ValueError, KeyError, TypeError) as e:
                self._responder(400, json.dumps({"error": f"petición inválida: {e}"}))

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
