CREATE TABLE tarjetas (
  id         INT AUTO_INCREMENT PRIMARY KEY,
  uid_rfid   VARCHAR(20)  NOT NULL UNIQUE,
  saldo      DECIMAL(8,2) NOT NULL DEFAULT 0 CHECK (saldo >= 0),
  estado     ENUM('activa','bloqueada') NOT NULL DEFAULT 'activa',
  fecha_alta DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE usuarios (
  id           INT AUTO_INCREMENT PRIMARY KEY,
  nombre       VARCHAR(100) NOT NULL,
  correo       VARCHAR(120) NOT NULL UNIQUE,
  password     VARCHAR(255) NOT NULL,
  tipo_usuario ENUM('pasajero','administrador') NOT NULL DEFAULT 'pasajero',
  id_tarjeta   INT NULL,
  FOREIGN KEY (id_tarjeta) REFERENCES tarjetas(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE rutas (
  id     INT AUTO_INCREMENT PRIMARY KEY,
  clave  VARCHAR(20)  NOT NULL UNIQUE,
  nombre VARCHAR(100) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE unidades (
  id               INT AUTO_INCREMENT PRIMARY KEY,
  numero_economico VARCHAR(20) NOT NULL UNIQUE,
  id_ruta          INT NOT NULL,
  capacidad        INT NOT NULL,
  FOREIGN KEY (id_ruta) REFERENCES rutas(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE dispositivos (
  id               INT AUTO_INCREMENT PRIMARY KEY,
  id_unidad        INT NOT NULL,
  tipo_dispositivo ENUM('validador','termico') NOT NULL,
  api_key_hash     CHAR(64) NOT NULL UNIQUE,
  ultima_conexion  DATETIME NULL,
  FOREIGN KEY (id_unidad) REFERENCES unidades(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE transacciones (
  id               INT AUTO_INCREMENT PRIMARY KEY,
  id_tarjeta       INT NULL,
  id_dispositivo   INT NOT NULL,
  monto            DECIMAL(8,2) NOT NULL,
  resultado        ENUM('exitoso','saldo_insuficiente','tarjeta_invalida') NOT NULL,
  saldo_resultante DECIMAL(8,2) NULL,
  fecha_hora       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (id_tarjeta)     REFERENCES tarjetas(id),
  FOREIGN KEY (id_dispositivo) REFERENCES dispositivos(id),
  INDEX (fecha_hora)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE temperatura (
  id                   INT AUTO_INCREMENT PRIMARY KEY,
  id_dispositivo       INT NOT NULL,
  temperatura          DECIMAL(5,2) NOT NULL,
  humedad              DECIMAL(5,2) NOT NULL,
  ventilador_encendido BOOLEAN NOT NULL,
  fecha_hora           DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (id_dispositivo) REFERENCES dispositivos(id),
  INDEX (fecha_hora)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE sensores (
  id             INT AUTO_INCREMENT PRIMARY KEY,
  id_dispositivo INT NOT NULL,
  tipo           ENUM('pir','ldr') NOT NULL,
  valor          INT NOT NULL,
  fecha_hora     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (id_dispositivo) REFERENCES dispositivos(id),
  INDEX (fecha_hora)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Datos de prueba

INSERT INTO rutas (clave, nombre) VALUES ('R1', 'Ruta de prueba');
INSERT INTO unidades (numero_economico, id_ruta, capacidad) VALUES ('101', 1, 40);

INSERT INTO dispositivos (id_unidad, tipo_dispositivo, api_key_hash) VALUES
  (1, 'validador', SHA2('validador-unidad-01', 256)),
  (1, 'termico',   SHA2('termico-unidad-01', 256));

INSERT INTO tarjetas (uid_rfid, saldo, estado) VALUES
  ('ADAE1A06', 100.00, 'activa'),     -- cobro normal
  ('4163ED05', 100.00, 'bloqueada');  -- Tarjeta invalida