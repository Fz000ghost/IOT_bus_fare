#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// Red y API (CAMBIAR)
const char* WIFI_SSID = "WIFINAME";            // red de 2.4 GHz
const char* WIFI_PASS = "WIFIPASS";
const char* API_BASE  = "http://0.0.0.0:5000";   // IP de la laptop que corre app.py
const char* API_KEY   = "validador-unidad-01";         // debe existir en la tabla dispositivos

// Pines
const uint8_t PIN_RFID_SS = D8;
const uint8_t PIN_SDA     = D2;
const uint8_t PIN_SCL     = D1;
const uint8_t PIN_BOTON   = D3;

// Parámetros
const float TARIFA_RESPALDO = 15.0;           // solo si la API no manda la tarifa
const unsigned long T_SESION    = 10000;      // ms para presionar el botón tras leer la tarjeta
const unsigned long T_RESULTADO = 3000;       // ms que se muestra el resultado del cobro
const unsigned long T_AVISO     = 1500;       // ms de mensajes cortos
const unsigned long ANTIRREBOTE = 50;         // ms
const uint16_t      TIMEOUT_API = 3000;       // ms de espera de la API

// Códigos de error propios (los negativos de HTTPClient van de -1 a -11)
const int ERR_SIN_WIFI = -100;
const int ERR_JSON     = -101;

// Objetos
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(PIN_RFID_SS, MFRC522::UNUSED_PIN);   // RST del módulo conectado a 3.3V

// Estado
enum Estado { ESPERANDO, TARJETA_LEIDA, MOSTRANDO };

Estado estado = ESPERANDO;
unsigned long tEstado = 0;          // cuándo se entró al estado actual
unsigned long duracionMensaje = 0;  // cuánto dura el estado MOSTRANDO
String uidSesion;                   // tarjeta leída que espera el botón

//  Pantalla LCD
void mostrar(const String& linea1, const String& linea2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(linea1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(linea2.substring(0, 16));
}

// La API manda pesos (float); se pasan a centavos para comparar sin errores de redondeo.
long aCentavos(float pesos) {
  return (long)(pesos * 100 + 0.5);   // el saldo nunca es negativo (CHECK en MySQL)
}

String textoSaldo(long centavos) {
  char buf[17];
  snprintf(buf, sizeof(buf), "Saldo: $%ld.%02ld", centavos / 100, centavos % 100);
  return String(buf);
}

//  Cambios de estado
void irAEspera() {
  estado = ESPERANDO;
  uidSesion = "";
  mostrar("Acerque tarjeta", WiFi.status() == WL_CONNECTED ? "" : "Sin conexion");
}

void mostrarMensaje(const String& l1, const String& l2, unsigned long duracion) {
  mostrar(l1, l2);
  uidSesion = "";
  estado = MOSTRANDO;
  tEstado = millis();
  duracionMensaje = duracion;
}

//  WiFi y API REST
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);      // si se cae la red, se reconecta solo
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  mostrar("Conectando WiFi", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado, IP: ");
    Serial.println(WiFi.localIP());
    mostrar("WiFi conectado", WiFi.localIP().toString());
  } else {
    Serial.println("No se pudo conectar al WiFi, se seguirá intentando");
    mostrar("Sin WiFi", "Reintentando...");
  }
  delay(1500);
}

// Si la red se cae o regresa mientras se espera tarjeta, actualiza la pantalla.
void revisarWiFi() {
  static bool conectadoPrevio = true;
  bool conectado = (WiFi.status() == WL_CONNECTED);
  if (conectado != conectadoPrevio) {
    conectadoPrevio = conectado;
    Serial.println(conectado ? "WiFi reconectado" : "WiFi perdido");
    if (estado == ESPERANDO) irAEspera();
  }
}

// Envía un POST con JSON y guarda en 'respuesta' el JSON que regresa la API.
// Devuelve el código HTTP (200, 400, 401...) o un código negativo si hubo error.
int apiPost(const char* ruta, JsonDocument& cuerpo, JsonDocument& respuesta) {
  if (WiFi.status() != WL_CONNECTED) return ERR_SIN_WIFI;
  WiFiClient cliente;
  HTTPClient http;
  if (!http.begin(cliente, String(API_BASE) + ruta)) return HTTPC_ERROR_CONNECTION_FAILED;
  http.setTimeout(TIMEOUT_API);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  String payload;
  serializeJson(cuerpo, payload);
  int codigo = http.POST(payload);
  String texto = codigo > 0 ? http.getString() : HTTPClient::errorToString(codigo);
  http.end();
  Serial.printf("POST %s %s -> %d %s\n", ruta, payload.c_str(), codigo, texto.c_str());
  if (codigo == 200 && deserializeJson(respuesta, texto)) codigo = ERR_JSON;
  return codigo;
}

void errorComunicacion(int codigo) {
  String detalle;
  if (codigo == ERR_SIN_WIFI)  detalle = "Sin WiFi";
  else if (codigo == ERR_JSON) detalle = "Resp. invalida";
  else if (codigo < 0)         detalle = "API no responde";
  else                         detalle = "HTTP " + String(codigo);
  mostrarMensaje("Error de red", detalle, T_RESULTADO);
}

//  Lector RFID
bool leerTarjeta(String& uid) { // Devuelve true si se leyó una tarjeta nueva y guarda su UID en hexadecimal.
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return false;

  uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  rfid.PICC_HaltA();        // la tarjeta no se vuelve a leer hasta retirarla
  rfid.PCD_StopCrypto1();
  Serial.println("Tarjeta: " + uid);
  return true;
}

// Al acercar la tarjeta se consulta el saldo en la API.
void consultarSaldo(const String& uid) {
  mostrar("Consultando...", uid);
  JsonDocument peticion;
  peticion["uid"] = uid;
  JsonDocument respuesta;
  int codigo = apiPost("/api/validador/consulta", peticion, respuesta);
  if (codigo != 200) { errorComunicacion(codigo); return; }

  if (!(respuesta["valida"] | false)) {
    mostrarMensaje("Tarjeta invalida", respuesta["motivo"] | "No registrada", T_RESULTADO);
    return;
  }

  uidSesion = uid;
  mostrar(textoSaldo(aCentavos(respuesta["saldo"] | 0.0f)), "Boton = pagar");
  estado = TARJETA_LEIDA;
  tEstado = millis();
}

// Al presionar el botón, la API hace el cobro y responde el resultado.
void cobrar() {
  mostrar("Procesando pago");
  JsonDocument peticion;
  peticion["uid"] = uidSesion;
  JsonDocument respuesta;
  int codigo = apiPost("/api/validador/cobro", peticion, respuesta);
  if (codigo != 200) { errorComunicacion(codigo); return; }

  const char* resultado = respuesta["resultado"] | "error";
  long saldo  = aCentavos(respuesta["saldo"]  | 0.0f);
  long tarifa = aCentavos(respuesta["tarifa"] | TARIFA_RESPALDO);

  if (strcmp(resultado, "exitoso") == 0) {
    if (saldo < 2 * tarifa) mostrarMensaje("OK. Saldo bajo", textoSaldo(saldo), T_RESULTADO);
    else                    mostrarMensaje("Cobro exitoso",  textoSaldo(saldo), T_RESULTADO);
  } else if (strcmp(resultado, "saldo_insuficiente") == 0) {
    mostrarMensaje("Saldo insuf.", textoSaldo(saldo), T_RESULTADO);
  } else {
    mostrarMensaje("Tarjeta invalida", "Cobro rechazado", T_RESULTADO);
  }
}

//  Botón
bool botonPresionado() {
  static bool estable = HIGH, ultimaLectura = HIGH;
  static unsigned long tCambio = 0;

  bool lectura = digitalRead(PIN_BOTON);
  if (lectura != ultimaLectura) {
    ultimaLectura = lectura;
    tCambio = millis();
  }
  if (millis() - tCambio > ANTIRREBOTE && lectura != estable) {
    estable = lectura;
    return estable == LOW;
  }
  return false;
}

//  Setup y loop
void setup() {
  Serial.begin(115200);
  Serial.println("\nValidador (simple + API) iniciando");

  pinMode(PIN_BOTON, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  mostrar("Validador", "Iniciando...");

  SPI.begin();
  rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("No se detecta el RC522, revisa las conexiones");
    mostrar("Error RFID", "Revisa conexion");
    delay(3000);
  }

  conectarWiFi();
  irAEspera();
}

void loop() {
  bool boton = botonPresionado();
  revisarWiFi();

  String uid;
  switch (estado) {
    case ESPERANDO:
      if (leerTarjeta(uid)) {
        consultarSaldo(uid);
      } else if (boton) {
        mostrarMensaje("Primero acerque", "su tarjeta", T_AVISO);
      }
      break;

    case TARJETA_LEIDA:
      if (boton) {
        cobrar();
      } else if (leerTarjeta(uid)) {          // otra tarjeta reemplaza la sesión
        consultarSaldo(uid);
      } else if (millis() - tEstado > T_SESION) {
        irAEspera();
      }
      break;

    case MOSTRANDO:
      if (millis() - tEstado > duracionMensaje) irAEspera();
      break;
  }
}
