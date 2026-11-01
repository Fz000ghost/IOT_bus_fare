#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Pines
const uint8_t PIN_RFID_SS = D8;
const uint8_t PIN_SDA     = D2;
const uint8_t PIN_SCL     = D1;
const uint8_t PIN_BOTON   = D3;

// Parámetros
const long TARIFA = 1500;                     // en centavos ($15.00); enteros para no perder centavos

const unsigned long T_SESION    = 10000;      // ms para presionar el botón tras leer la tarjeta
const unsigned long T_RESULTADO = 3000;       // ms que se muestra el resultado del cobro
const unsigned long T_AVISO     = 1500;       // ms de mensajes cortos
const unsigned long ANTIRREBOTE = 50;         // ms

// Tarjetas (CAMBIAR)
struct Tarjeta {
  const char* uid;
  long saldo;        // centavos
  bool activa;
};

Tarjeta TARJETAS[] = {
  { "ADAE1A06", 10000, true  },   // $100: cobro exitoso
  { "11223344",  4000, true  },   // $40: tras un cobro quedan $25 < 2 viajes
  { "4163ED05", 10000, false },   // bloqueada: cobro rechazado
};

// Objetos
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(PIN_RFID_SS, MFRC522::UNUSED_PIN);   // RST del módulo conectado a 3.3V

// Estado
enum Estado { ESPERANDO, TARJETA_LEIDA, MOSTRANDO };

Estado estado = ESPERANDO;
unsigned long tEstado = 0;          // cuándo se entró al estado actual
unsigned long duracionMensaje = 0;  // cuánto dura el estado MOSTRANDO

Tarjeta* tarjetaSesion = nullptr;   // tarjeta leída que espera el botón

//  Pantalla LCD
void mostrar(const String& linea1, const String& linea2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(linea1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(linea2.substring(0, 16));
}

String textoSaldo(long centavos) {
  char buf[17];
  snprintf(buf, sizeof(buf), "Saldo: $%ld.%02ld", centavos / 100, centavos % 100);
  return String(buf);
}

//  Cambios de estado
void irAEspera() {
  estado = ESPERANDO;
  tarjetaSesion = nullptr;
  mostrar("Acerque tarjeta");
}

void mostrarMensaje(const String& l1, const String& l2, unsigned long duracion) {
  mostrar(l1, l2);
  tarjetaSesion = nullptr;
  estado = MOSTRANDO;
  tEstado = millis();
  duracionMensaje = duracion;
}

//  Lector RFID y tabla de tarjetas
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

Tarjeta* buscarTarjeta(const String& uid) {
  for (Tarjeta& t : TARJETAS) {
    if (uid.equalsIgnoreCase(t.uid)) return &t;
  }
  return nullptr;
}

// Al acercar la tarjeta se muestra el saldo.
void consultarSaldo(const String& uid) {
  Tarjeta* t = buscarTarjeta(uid);
  if (t == nullptr) {
    mostrarMensaje("Tarjeta invalida", "No registrada", T_RESULTADO);
    return;
  }
  if (!t->activa) {
    mostrarMensaje("Tarjeta invalida", "Bloqueada", T_RESULTADO);
    return;
  }

  tarjetaSesion = t;
  mostrar(textoSaldo(t->saldo), "Boton = pagar");
  estado = TARJETA_LEIDA;
  tEstado = millis();
}

// Al presionar el botón se hace el cobro.
void cobrar() {
  Tarjeta* t = tarjetaSesion;

  if (t->saldo < TARIFA) {
    Serial.printf("Cobro %s: saldo_insuficiente, saldo %ld\n", t->uid, t->saldo);
    mostrarMensaje("Saldo insuf.", textoSaldo(t->saldo), T_RESULTADO);
    return;
  }

  t->saldo -= TARIFA;
  Serial.printf("Cobro %s: exitoso, saldo %ld\n", t->uid, t->saldo);

  if (t->saldo < 2 * TARIFA) mostrarMensaje("OK. Saldo bajo", textoSaldo(t->saldo), T_RESULTADO);
  else                       mostrarMensaje("Cobro exitoso",  textoSaldo(t->saldo), T_RESULTADO);
}

//  Botón
bool botonPresionado() { // Devuelve true una sola vez por cada pulsación (con antirrebote sin delay).
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
  Serial.println("\nValidador (sin WiFi) iniciando");

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

  irAEspera();
}

void loop() {
  bool boton = botonPresionado();

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
