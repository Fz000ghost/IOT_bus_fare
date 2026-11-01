USE validador;
SELECT id, tipo_dispositivo, ultima_conexion FROM dispositivos;
SELECT id_dispositivo, resultado, fecha_hora FROM transacciones ORDER BY id DESC LIMIT 3;