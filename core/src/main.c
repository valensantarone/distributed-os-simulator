#include <commons/config.h>
#include <commons/log.h>
#include <stdlib.h>
#include <utils/conexion.h>
#include <utils/paquete.h>

t_log* logger;

int main(int argc, char* argv[]) {
  t_config* config = config_create(argv[1]);
  logger = log_create("core.log", "Core", 1, LOG_LEVEL_INFO);

  int fd_planificador =
      crear_conexion(config_get_string_value(config, "IP_PLANIFICADOR"),
                     config_get_string_value(config, "PUERTO_PLANIFICADOR"));
  enviar_handshake(fd_planificador, "CORE");
  log_info(logger, "## Conectado a Planificador exitosamente");
  enviar_mensaje(fd_planificador, "Mensaje de prueba desde CORE");

  int fd_placa =
      crear_conexion(config_get_string_value(config, "IP_PLACA"),
                     config_get_string_value(config, "PUERTO_PLACA"));
  enviar_handshake(fd_placa, "CORE");
  log_info(logger, "## Conectado a Placa exitosamente");
  enviar_mensaje(fd_placa, "Mensaje de prueba desde CORE");

  cerrar_conexion(fd_planificador);
  cerrar_conexion(fd_placa);
  log_destroy(logger);
  config_destroy(config);

  return EXIT_SUCCESS;
}
