#include <commons/config.h>
#include <commons/log.h>
#include <stdlib.h>
#include <utils/conexion.h>
#include <utils/paquete.h>
#include <utils/servidor.h>

t_log* logger;

void atender_conexion(int fd_conexion) {
  int operacion = recibir_operacion(fd_conexion);
  while (operacion != -1) {
    switch (operacion) {
      case HANDSHAKE: {
        char* nombre_modulo = recibir_handshake(fd_conexion);
        log_info(logger, "## Módulo: %s", nombre_modulo);
        free(nombre_modulo);
        break;
      }
      case MENSAJE: {
        char* mensaje = recibir_mensaje(fd_conexion);
        log_info(logger, "Mensaje de prueba recibido: %s", mensaje);
        free(mensaje);
        break;
      }
    }
    operacion = recibir_operacion(fd_conexion);
  }
  cerrar_conexion(fd_conexion);
}

int main(int argc, char* argv[]) {
  t_config* config = config_create(argv[1]);
  logger = log_create("planificador.log", "Planificador", 1, LOG_LEVEL_INFO);

  // Cliente de Placa y Storage (la consigna pide conectarse a ambos ANTES de
  // levantar el servidor propio)
  int fd_placa =
      crear_conexion(config_get_string_value(config, "IP_PLACA"),
                     config_get_string_value(config, "PUERTO_PLACA"));
  enviar_handshake(fd_placa, "PLANIFICADOR");

  int fd_storage =
      crear_conexion(config_get_string_value(config, "IP_STORAGE"),
                     config_get_string_value(config, "PUERTO_STORAGE"));
  enviar_handshake(fd_storage, "PLANIFICADOR");

  // Servidor para los Cores
  int fd_servidor =
      iniciar_servidor(config_get_string_value(config, "PUERTO_ESCUCHA"));
  log_info(logger, "Servidor listo para recibir Cores");
  escuchar_conexiones(fd_servidor, atender_conexion);  // no vuelve

  return EXIT_SUCCESS;
}
