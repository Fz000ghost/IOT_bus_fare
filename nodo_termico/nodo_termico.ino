#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Red y API
const char* WIFI_SSID = "NOMBRE_DE_LA_RED";
const char* WIFI_PASS = "CONTRASENA";
const char* API_BASE  = "http://192.168.1.100:5000";
const char* API_KEY   = "termico-unidad-01";

// Pines
const uint8_t PIN_DHT        = D2;
#define TIPO_DHT DHT11                 // cambiar a DHT11 si se usa ese sensor
const uint8_t PIN_VENTILADOR = D5;
const uint8_t PIN_BRILLO_LCD = D6;
const uint8_t PIN_LDR        = A0;

// Parámetros
const float   TEMP_ENCENDER  = 28.0;   // °C, se enciende al superar este valor
const float   TEMP_APAGAR    = 26.5;   // °C, se apaga al bajar de este valor
const uint8_t FALLAS_MAX_DHT = 3;      // lecturas fallidas seguidas antes de encender por seguridad
const int  BRILLO_MIN    = 40;         // 0-255, brillo con poca luz (de noche)
const int  BRILLO_MAX    = 255;        // 0-255, brillo con mucha luz (de día)
const bool LDR_INVERTIDO = false;      // true si tu LDR da valores ALTOS con poca luz
const unsigned long T_LECTURA_DHT  = 2500;   // el DHT necesita ~2 s entre lecturas
const unsigned long T_REPORTE_TEMP = 10000;
const unsigned long T_LECTURA_LDR  = 200;
const unsigned long T_REPORTE_LDR  = 10000;
const uint16_t      TIMEOUT_HTTP   = 1500;
const int ERR_SIN_WIFI = -100;

// Estado
DHT dht(PIN_DHT, TIPO_DHT);
float   temperatura = NAN;
float   humedad     = NAN;
bool    ventiladorEncendido = false;
uint8_t fallasDHT   = 0;
float   ldrFiltrado = -1;              // -1 = todavía sin lecturas

//  API REST
int apiPost(const char* ruta, JsonDocument& cuerpo) {
  if (WiFi.status() != WL_CONNECTED) return ERR_SIN_WIFI;
  WiFiClient cliente;
  HTTPClient http;
  if (!http.begin(cliente, String(API_BASE) + ruta)) return HTTPC_ERROR_CONNECTION_FAILED;
  http.setTimeout(TIMEOUT_HTTP);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  String payload;
  serializeJson(cuerpo, payload);
  int codigo = http.POST(payload);
  http.end();
  Serial.printf("POST %s %s -> %d\n", ruta, payload.c_str(), codigo);
  return codigo;
}
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando a WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Sin WiFi, el control local sigue funcionando y se reintentará");
  }
}

//  Temperatura y ventilador
void setVentilador(bool encender) {
  ventiladorEncendido = encender;
  digitalWrite(PIN_VENTILADOR, encender ? HIGH : LOW);
}
void reportarTemperatura() {
  if (isnan(temperatura)) return;
  JsonDocument datos;
  datos["temperatura"]          = temperatura;
  datos["humedad"]              = humedad;
  datos["ventilador_encendido"] = ventiladorEncendido;
  apiPost("/api/temperatura", datos);
}
void actualizarTemperatura() {
  static unsigned long tLectura = 0, tReporte = 0;
  if (millis() - tLectura < T_LECTURA_DHT) return;
  tLectura = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    if (fallasDHT < 255) fallasDHT++;
    Serial.println("Error leyendo el DHT");
    if (fallasDHT >= FALLAS_MAX_DHT && !ventiladorEncendido) {
      setVentilador(true);
      Serial.println("DHT sin respuesta: ventilador encendido por seguridad");
    }
    return;
  }
  fallasDHT   = 0;
  temperatura = t;
  humedad     = h;
  bool antes = ventiladorEncendido;
  if (t > TEMP_ENCENDER)     setVentilador(true);
  else if (t < TEMP_APAGAR)  setVentilador(false);
  // entre 26.5 y 28 °C se mantiene como estaba (histéresis)
  Serial.printf("Temp: %.1f C  Humedad: %.1f %%  Ventilador: %s\n",
                t, h, ventiladorEncendido ? "ON" : "OFF");
  // Se reporta al cambiar el ventilador y de forma periódica
  if (ventiladorEncendido != antes || millis() - tReporte >= T_REPORTE_TEMP) {
    tReporte = millis();
    reportarTemperatura();
  }
}

//  Luz ambiente y brillo de la LCD
void actualizarLuz() {
  static unsigned long tLectura = 0, tReporte = 0;
  if (millis() - tLectura < T_LECTURA_LDR) return;   // leer A0 muy seguido afecta al WiFi
  tLectura = millis();
  int lectura = analogRead(PIN_LDR);                  // 0-1023
  // Filtro para que el brillo cambie suave y no parpadee
  ldrFiltrado = (ldrFiltrado < 0) ? lectura : ldrFiltrado * 0.85 + lectura * 0.15;
  int luz    = LDR_INVERTIDO ? 1023 - (int)ldrFiltrado : (int)ldrFiltrado;
  int brillo = map(luz, 0, 1023, BRILLO_MIN, BRILLO_MAX);
  analogWrite(PIN_BRILLO_LCD, constrain(brillo, 0, 255));
  if (millis() - tReporte >= T_REPORTE_LDR) {
    tReporte = millis();
    Serial.printf("LDR: %d  Brillo LCD: %d\n", (int)ldrFiltrado, brillo);
    JsonDocument datos;
    datos["tipo"]  = "ldr";
    datos["valor"] = (int)ldrFiltrado;
    apiPost("/api/sensores", datos);
  }
}

//  Setup y loop
void setup() {
  Serial.begin(115200);
  Serial.println("\nNodo termico iniciando");

  pinMode(PIN_VENTILADOR, OUTPUT);
  setVentilador(false);

  pinMode(PIN_BRILLO_LCD, OUTPUT);
  analogWriteRange(255);
  analogWrite(PIN_BRILLO_LCD, BRILLO_MAX);

  dht.begin();
  conectarWiFi();
}

void loop() {
  actualizarTemperatura();
  actualizarLuz();
}
