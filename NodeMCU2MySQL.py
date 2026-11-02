#cd FILE_LOCATION
#$env:DB_PASS = "DB_PASSWORD"
#python NodeMCU2MySQL.py

from flask import Flask, abort, jsonify, request
from decimal import Decimal
from werkzeug.exceptions import HTTPException
import pymysql
import hashlib
import os
import re

app = Flask(__name__) #Creates a Flask object

TARIFA = Decimal("15.00")

def conectar():
    return pymysql.connect(host="localhost", user="root", password=os.environ.get("DB_PASS", ""), database="validador", cursorclass=pymysql.cursors.DictCursor) #Open a connection to MySQL

@app.errorhandler(HTTPException)
def error_http(e):
    return jsonify(error=e.description), e.code #Returns error message and code

@app.errorhandler(Exception)
def error_interno(e):
    app.logger.exception("error interno") #Prints full traceback
    return jsonify(error="error interno"), 500  #Prints generic error message

def cuerpo_json():
    cuerpo = request.get_json(silent=True) #Reads and converts JSON
    if not isinstance(cuerpo, dict): #If not a valid JSON, return None
        abort(400, "se esperaba un objeto JSON") 
    return cuerpo #If the body passed, the route gets the dictionary

def leer_uid(cuerpo): #Receives the body dictionary
    uid = cuerpo.get("uid") #Returns the value
    if not (isinstance(uid, str) and re.fullmatch(r"[0-9A-Fa-f]{2,20}", uid)): #The UID must be a hexadecimal 8 chars long
        abort(400, "uid debe ser hxadecimal de 2 a 20 caracteres")
    return uid.upper() #Returns the UID in uppercase

def autenticar(*tipos_permitidos): 
    llave = request.headers.get("X-API-Key", "") #Holds the request's headers
    hash_llave = hashlib.sha256(llave.encode()).hexdigest() #Computes the SHA-256 of the key
    con = conectar()
    try:
        with con.cursor() as c:
            c.execute("SELECT id, tipo_dispositivo FROM dispositivos WHERE api_key_hash = %s", (hash_llave,)) #Looks for a device with the given hash
            dispositivo = c.fetchone()
            if dispositivo is None:
                abort(401, "api_key invalida")
            if dispositivo["tipo_dispositivo"] not in tipos_permitidos: 
                abort(403, "dispositivo no autorizado")
            c.execute("UPDATE dispositivos SET ultima_conexion = NOW() WHERE id = %s", (dispositivo["id"],)) #Record the current time for the device that just connected
        con.commit() #Saves record
    finally:
        con.close() #Close connection
    return dispositivo #Get the device's row back

@app.post("/api/validador/consulta") #Called when NodeMCU sends a POST request
def consulta():
    autenticar("validador")
    uid = leer_uid(cuerpo_json()) #Reads an uid and returns a dictionary
    con = conectar() #Stores the connection
    try:
        with con.cursor() as c: #Creates a SQL cursor and reads the result
            c.execute("SELECT saldo, estado FROM tarjetas WHERE uid_rfid = %s", (uid,)) #Sends the query
            tarjeta = c.fetchone() #Returns the first row as a dictionary
    finally:
        con.close() #Close conenction to MySQL
    if tarjeta is None: #Checks if card is on the query
        return jsonify(valida=False, motivo="No registrada")
    if tarjeta["estado"] != "activa": #Checks if card is not blocked
        return jsonify(valida=False, motivo="Bloqueada")
    return jsonify(valida=True, saldo=float(tarjeta["saldo"]), tarifa=float(TARIFA)) #else return the card's data

@app.post("/api/validador/cobro")
def cobro():
    dispositivo = autenticar("validador")
    uid = leer_uid(cuerpo_json())
    id_dispositivo = dispositivo["id"]
    con = conectar()
    try:
        with con.cursor() as c: #Creates a SQL cursor and reads the result
            c.execute("SELECT id, saldo, estado FROM tarjetas WHERE uid_rfid = %s FOR UPDATE", (uid,)) #Lock a card's row until the transaction ends
            tarjeta = c.fetchone() #Returns the first row as a dictionary
            if tarjeta is None: #Card does not exist
                id_tarjeta = None
                saldo = None
                resultado = "tarjeta_invalida"
            else:
                id_tarjeta = tarjeta["id"]
                saldo = tarjeta["saldo"]
                if tarjeta["estado"] != "activa": #Blocked card
                    resultado = "tarjeta_invalida"
                elif saldo < TARIFA: #Not enough money to pay
                    resultado = "saldo_insuficiente"
                else: #All conditions met, update saldo
                    resultado = "exitoso"
                    saldo = saldo - TARIFA
                    c.execute("UPDATE tarjetas SET saldo = %s WHERE id = %s", (saldo, id_tarjeta))
            c.execute("INSERT INTO transacciones (id_tarjeta, id_dispositivo, monto, resultado, saldo_resultante) ""VALUES (%s, %s, %s, %s, %s)", (id_tarjeta, id_dispositivo, TARIFA, resultado, saldo),) #Save all results, including rejected ones on one row
        con.commit() #Saves the transaction
    except Exception:
        con.rollback() #Undoes everything
        raise
    finally:
        con.close() #Close connection
    saldo_json = 0.0 if saldo is None else float(saldo) #Prepares the balance for the response
    return jsonify(resultado=resultado, saldo=saldo_json, tarifa=float(TARIFA)) #Sends the response to the NodeMCU

@app.post("/api/sensores")
def sensores():
    dispositivo = autenticar("validador", "termico") #Only "validador" and "termico" devices are allowed to call
    datos = cuerpo_json #Read json data
    tipo = datos.get("tipo")
    valor = datos.get("valor")
    if tipo == "pir": #PIR only sends 0 or 1
        maximo = 1    
    if tipo == "ldr": #Analog read from 0 to 1023
        maximo = 1023
    else:
        abort(400, "tipo debe ser pir o ldr") #Not PIR or LDR, then rejected
    if not (type(valor) is int and 0 <= valor <= maximo):
        abort(400, f"valor debe ser un entero entre 0 y {maximo}")
    if tipo == "pir" and dispositivo["tipo_dispositivo"] != "validador":
        abort(403, "solo el validador envia pir")
    if tipo == "ldr" and dispositivo["tipo_dispositivo"] != "termico":
        abort(403, "solo el nodo termico envia ldr")
    con = conectar()
    try:
        with con.cursor() as c:
            c.execute("INSERT INTO sensores (id_dispositivo, tipo, valor) VALUES (%s, %s, %s)", (dispositivo["id"], tipo, valor),) #Sends SQL instruction
        con.commit() #Saves the row
    finally:
        con.close() #Close connection
    return jsonify(ok=True), 201

@app.post("/api/temperatura")
def temperatura():
    dispositivo = autenticar("termico") #Only "termico" devices are allowed to call
    datos = cuerpo_json() #Read json data
    
    t = datos.get("temperatura")
    h = datos.get("humedad")
    v = datos.get("ventilador_encendido")
    if not (type(t) in (int, float) and -40 <= t <= 80):
        abort(400, "temperatura debe estar entre -40 y 80")
    if not (type(h) in (int, float) and 0 <= h <= 100):
        abort(400, "humedad debe estar entre 0 y 100")
        if type(v) is not bool:
            abort(400, "ventilador_encendido debe ser true o false")
    
    con = conectar()
    try:
        with con.cursor() as c:
            c.execute("INSERT INTO temperatura (id_dispositivo, temperatura, humedad, ventilador_encendido) ""VALUES (%s, %s, %s, %s)", (dispositivo["id"], t, h, v)) #Sends SQL instruction
        con.commit() #Saves the row
    finally:
        con.close() #Close connection
    return jsonify(ok=True), 201 

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)

