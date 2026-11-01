#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// Red y API
const char* WIFI_SSID = "NOMBRE_DE_LA_RED";
const char* WIFI_PASS = "CONTRASENA";
const char* API_BASE  = "http://192.168.1.100:5000";  // IP de la compu donde corre Flask
const char* API_KEY   = "validador-unidad-01";        // la API guarda su hash en dispositivos.api_key_hash

// Pines
const uint8_t PIN_RFID_SS = D8;
const uint8_t PIN_SDA     = D2;
const uint8_t PIN_SCL     = D1;
const uint8_t PIN_BUZZER  = D0;
const uint8_t PIN_LED_R   = D3;
const uint8_t PIN_LED_G   = D4;
const uint8_t PIN_BOTON   = 3;    // RX (GPIO3)
const uint8_t PIN_PIR     = A0;

// Parámetros
const float TARIFA_RESPALDO = 15.0;           // solo si la API no manda la tarifa
const unsigned long T_SESION        = 10000;  // ms para presionar el botón tras leer la tarjeta
const unsigned long T_RESULTADO     = 3000;   // ms que se muestra el resultado del cobro
const unsigned long T_AVISO         = 1500;   // ms de mensajes cortos
const unsigned long T_LCD_ENCENDIDA = 15000;  // apagar la pantalla sin movimiento
const unsigned long T_LECTURA_PIR   = 100;    // ms entre lecturas del PIR
const unsigned long T_REPORTE_PIR   = 5000;   // reporte periódico del PIR a la API
const unsigned long ANTIRREBOTE     = 50;     // ms
const uint16_t TIMEOUT_COBRO      = 3000;     // ms de espera para consulta y cobro
const uint16_t TIMEOUT_TELEMETRIA = 800;      // ms de espera para datos de sensores
const int UMBRAL_PIR = 512;                   // A0 > 512 significa salida del PIR en ALTO
const bool    LED_ANODO_COMUN = true;         // false si usas LED de cátodo común (NO en D3/D4)
const uint8_t AMARILLO_R = 255;               // ajusta si el amarillo se ve naranja o verdoso
const uint8_t AMARILLO_G = 100;

// Objetos
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(PIN_RFID_SS, MFRC522::UNUSED_PIN);   // RST del módulo conectado a 3.3V

// Estado
enum Estado { ESPERANDO, TARJETA_LEIDA, MOSTRANDO };
enum ColorLed { APAGADO, VERDE, ROJO, AMARILLO };
// Códigos de error propios (los negativos de HTTPClient van de -1 a -11)
const int ERR_SIN_WIFI   = -100;
const int ERR_JSON       = -101;
Estado estado = ESPERANDO;
unsigned long tEstado = 0;          // cuándo se entró al estado actual
unsigned long duracionMensaje = 0;  // cuánto dura el estado MOSTRANDO
String uidSesion;                   // tarjeta leída que espera el botón
float  saldoSesion  = 0;
float  tarifaSesion = TARIFA_RESPALDO;
bool lcdEncendida = false;
unsigned long tUltimoMovimiento = 0;

//  LED de estado
void escribirCanal(uint8_t pin, uint8_t valor) {
  analogWrite(pin, LED_ANODO_COMUN ? 255 - valor : valor);
}
void setLed(ColorLed color) {
  switch (color) {
    case VERDE:    escribirCanal(PIN_LED_R, 0);          escribirCanal(PIN_LED_G, 255);        break;
    case ROJO:     escribirCanal(PIN_LED_R, 255);        escribirCanal(PIN_LED_G, 0);          break;
    case AMARILLO: escribirCanal(PIN_LED_R, AMARILLO_R); escribirCanal(PIN_LED_G, AMARILLO_G); break;
    default:       escribirCanal(PIN_LED_R, 0);          escribirCanal(PIN_LED_G, 0);          break;
  }
}

//  Buzzer (pasivo, con tonos)
void pitido(unsigned int frecuencia, unsigned long ms) {
  tone(PIN_BUZZER, frecuencia, ms);
  delay(ms + 40);
}
void sonidoAcercamiento() { pitido(2200, 50); }
void sonidoTarjeta()      { pitido(1800, 80); }
void sonidoExito()        { pitido(1500, 90); pitido(2500, 140); }
void sonidoSaldoBajo()    { pitido(2000, 120); pitido(2000, 120); pitido(2000, 120); }
void sonidoError()        { pitido(400, 600); }
void sonidoAviso()        { pitido(1000, 150); }

//  Pantalla LCD
void mostrar(const String& linea1, const String& linea2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(linea1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(linea2.substring(0, 16));
}
String textoSaldo(float saldo) {
  char buf[17];
  snprintf(buf, sizeof(buf), "Saldo: $%.2f", saldo);
  return String(buf);
}
// Enciende la retroiluminación (bit del módulo I2C) y reinicia el temporizador.
void encenderLCD() {
  if (!lcdEncendida) {
    lcd.backlight();
    lcdEncendida = true;
  }
  tUltimoMovimiento = millis();
}
// Apaga la pantalla si nadie se ha movido y no hay una operación en curso.
void revisarRetroiluminacion() {
  if (lcdEncendida && estado == ESPERANDO &&
      millis() - tUltimoMovimiento > T_LCD_ENCENDIDA) {
    lcd.noBacklight();
    lcdEncendida = false;
  }
}

//  Cambios de estado
void irAEspera() {
  estado = ESPERANDO;
  uidSesion = "";
  setLed(APAGADO);
  mostrar("Acerque tarjeta", WiFi.status() == WL_CONNECTED ? "" : "Sin conexion");
}
void mostrarMensaje(const String& l1, const String& l2, ColorLed color, unsigned long duracion) {
  encenderLCD();
  mostrar(l1, l2);
  setLed(color);
  uidSesion = "";
  estado = MOSTRANDO;
  tEstado = millis();
  duracionMensaje = duracion;
}

//  Comunicación con la API REST
// Envía un POST con JSON. Si 'respuesta' no es nullptr, interpreta el JSON que
// regresa la API. Devuelve el código HTTP o un código negativo si hubo error.
int apiPost(const char* ruta, JsonDocument& cuerpo, JsonDocument* respuesta, uint16_t timeout) {
  if (WiFi.status() != WL_CONNECTED) return ERR_SIN_WIFI;
  WiFiClient cliente;
  HTTPClient http;
  if (!http.begin(cliente, String(API_BASE) + ruta)) return HTTPC_ERROR_CONNECTION_FAILED;
  http.setTimeout(timeout);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  String payload;
  serializeJson(cuerpo, payload);
  int codigo = http.POST(payload);
  if (codigo > 0 && respuesta != nullptr) {
    if (deserializeJson(*respuesta, http.getString())) codigo = ERR_JSON;
  }
  http.end();
  Serial.printf("POST %s -> %d\n", ruta, codigo);
  return codigo;
}
void errorComunicacion(int codigo) {
  String detalle;
  if (codigo == ERR_SIN_WIFI)      detalle = "Sin WiFi";
  else if (codigo == ERR_JSON)     detalle = "Resp. invalida";
  else if (codigo < 0)             detalle = "API no responde";
  else                             detalle = "HTTP " + String(codigo);
  mostrarMensaje("Error de red", detalle, ROJO, T_RESULTADO);
  sonidoError();
}
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

//  Lector RFID
// Devuelve true si se leyó una tarjeta nueva y guarda su UID en hexadecimal.
bool leerTarjeta(String& uid) {
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
// Paso 2: al acercar la tarjeta se consulta y muestra el saldo.
void consultarSaldo(const String& uid) {
  encenderLCD();
  sonidoTarjeta();
  mostrar("Consultando...", "UID " + uid);
  JsonDocument peticion;
  peticion["uid"] = uid;
  JsonDocument respuesta;
  int codigo = apiPost("/api/validador/consulta", peticion, &respuesta, TIMEOUT_COBRO);
  if (codigo != 200) { errorComunicacion(codigo); return; }
  if (!(respuesta["valida"] | false)) {
    mostrarMensaje("Tarjeta invalida", respuesta["motivo"] | "No registrada", ROJO, T_RESULTADO);
    sonidoError();
    return;
  }
  uidSesion    = uid;
  saldoSesion  = respuesta["saldo"]  | 0.0f;
  tarifaSesion = respuesta["tarifa"] | TARIFA_RESPALDO;
  mostrar(textoSaldo(saldoSesion), "Boton = pagar");
  estado = TARJETA_LEIDA;
  tEstado = millis();
}
// Paso 3: al presionar el botón, la API hace el cobro y responde el resultado.
void cobrar() {
  mostrar("Procesando pago", "");
  JsonDocument peticion;
  peticion["uid"] = uidSesion;
  JsonDocument respuesta;
  int codigo = apiPost("/api/validador/cobro", peticion, &respuesta, TIMEOUT_COBRO);
  if (codigo != 200) { errorComunicacion(codigo); return; }
  const char* resultado = respuesta["resultado"] | "error";
  float saldo  = respuesta["saldo"]  | saldoSesion;
  float tarifa = respuesta["tarifa"] | tarifaSesion;
  if (strcmp(resultado, "exitoso") == 0) {
    if (saldo < 2 * tarifa) {
      mostrarMensaje("OK. Saldo bajo", textoSaldo(saldo), AMARILLO, T_RESULTADO);
      sonidoSaldoBajo();
    } else {
      mostrarMensaje("Cobro exitoso", textoSaldo(saldo), VERDE, T_RESULTADO);
      sonidoExito();
    }
  } else if (strcmp(resultado, "saldo_insuficiente") == 0) {
    mostrarMensaje("Saldo insuf.", textoSaldo(saldo), ROJO, T_RESULTADO);
    sonidoError();
  } else {
    mostrarMensaje("Tarjeta invalida", "Cobro rechazado", ROJO, T_RESULTADO);
    sonidoError();
  }
}

//  Botón y PIR
// Devuelve true una sola vez por cada pulsación (con antirrebote sin delay).
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
void actualizarPIR() {
  static unsigned long tLectura = 0, tReporte = 0;
  static bool previo = false;
  if (millis() - tLectura < T_LECTURA_PIR) return;
  tLectura = millis();
  bool movimiento = analogRead(PIN_PIR) > UMBRAL_PIR;
  if (movimiento) {
    if (!lcdEncendida && estado == ESPERANDO) sonidoAcercamiento();
    encenderLCD();
  }
  // Se reporta cuando cambia y cada T_REPORTE_PIR (también actualiza ultima_conexion)
  if (movimiento != previo || millis() - tReporte >= T_REPORTE_PIR) {
    previo = movimiento;
    tReporte = millis();
    JsonDocument datos;
    datos["tipo"]  = "pir";
    datos["valor"] = movimiento ? 1 : 0;
    apiPost("/api/sensores", datos, nullptr, TIMEOUT_TELEMETRIA);
  }
}

//  Setup y loop
void setup() {
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);   // RX queda libre para el botón
  Serial.println("\nValidador iniciando");
  pinMode(PIN_BOTON, INPUT_PULLUP);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  analogWriteRange(255);
  setLed(APAGADO);
  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  encenderLCD();
  mostrar("Validador", "Iniciando...");
  SPI.begin();
  rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("No se detecta el RC522, revisa las conexiones");
    mostrar("Error RFID", "Revisa conexion");
    setLed(ROJO);
    sonidoError();
    delay(3000);
  }
  conectarWiFi();
  irAEspera();
}

void loop() {
  bool boton = botonPresionado();
  actualizarPIR();
  revisarRetroiluminacion();
  revisarWiFi();
  String uid;
  switch (estado) {
    case ESPERANDO:
      if (leerTarjeta(uid)) {
        consultarSaldo(uid);
      } else if (boton) {
        mostrarMensaje("Primero acerque", "su tarjeta", APAGADO, T_AVISO);
        sonidoAviso();
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
