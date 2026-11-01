from flask import Flask, abort, jsonify, request
from decimal import Decimal
import pymysql
import hashlib

app = Flask(__name__) #Creates a Flask object

TARIFA = Decimal("15.00")

def conectar():
    return pymysql.connect(host="localhost", user="root", password="NuevaPass123", database="validador", cursorclass=pymysql.cursors.DictCursor) #Open a connection to MySQL

def autenticar(*tipos_permitidos):
    llave = request.headers.get("X-API-Key", "")
    hash_llave = hashlib.sha256(llave.encode()).hexdigest()
    con = conectar()
    try:
        with con.cursor() as c:
            c.execute("SELECT id, tipo_dispositivo FROM dispositivos WHERE api_key_hash = %s", (hash_llave,))
            dispositivo = c.fetchone()
            if dispositivo is None:
                abort(401, "api_key invalida")
            if dispositivo["tipo_dispositivo"] not in tipos_permitidos:
                abort(403, "dispositivo no autorizado")

            c.execute("UPDATE dispositivos SET ultima_conexion = NOW() WHERE id = %s", (dispositivo["id"],))
        con.commit()
    finally:
        con.close()
    return dispositivo

@app.get("/hi") #Test function to return a JSON message that answers request /hi
def hola():
    return jsonify(mensaje="hii")

@app.post("/api/validador/consulta") #Called when NodeMCU sends a POST request
def consulta():
    autenticar("validador")
    uid = request.get_json()["uid"].upper() #Reads an uid and returns a dictionary
    con = conectar() #Stores the connection
    with con.cursor() as c: #Creates a SQL cursor and reads the result
        c.execute("SELECT saldo, FROM tarjetas WHERE uid_rfid = %s", (uid,)) #Sends the query
        tarjeta = c.fetchone() #Returns the first row as a dictionary
    con.close() #Close conenction to MySQL

    if tarjeta is None: #Checks if card is on the query
        return jsonify(valida=False, motivo="No registrada")
    if tarjeta ["estado"] != "activa": #Checks if card is not blocked
        return jsonify(valida=False, motivo="Bloqueada")
    return jsonify(valida=True, saldo=float(tarjeta["Saldo"]), tarifa=float(TARIFA)) #else return the card's data

@app.post("/api/validador/cobro")
def cobro():
    dispositivo = autenticar("validador")
    uid = request.get_json()["uid"].upper()
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
            c.execute("INSERT INTO transacciones (id_tarjeta, id_dispositivo, monto, resultado, saldo_resultante)" "VALUES (%s, %s, %s, %s, %s)", (id_tarjeta, id_dispositivo, TARIFA, resultado, saldo),) #Save all results, including rejected ones on one row
        con.commit() #Saves the transaction
    except Exception:
        con.rollback() #Undoes everything
        raise
    finally:
        con.close() #Close connection
    saldo_json = 0.0 if saldo is None else float(saldo) #Prepares the balance for the response
    return jsonify(resultado=resultado, saldo=saldo_json, tarifa=float(TARIFA)) #Sends the response to the NodeMCU

if __name__ == "__main__": #Start server only if the file was run directly
    app.run(host="0.0.0.0", port=5000, debug=True) #Waits for requests forever

