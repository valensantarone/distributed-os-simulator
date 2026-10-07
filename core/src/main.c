#include <commons/config.h>
#include <commons/log.h>
#include <stdlib.h>
#include <stdbool.h>
#include <utils/conexion.h>
#include <utils/paquete.h>

#include "protocolo_core.h"
#include "instrucciones.h"

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

  //Bucle principal del ciclo de instrucción
  while (1) {
    uint32_t jid;
    t_contexto contexto;

    recibir_asignacion_job(fd_planificador, &jid, &contexto);

    bool ejecutando_job = true;

    while (ejecutando_job) {
      //Fetch
      log_info(logger, "## JID: %d - FETCH - Program Counter: %d", jid, contexto.pc);
      enviar_fetch(fd_placa, jid, contexto.pc);
      char* instruccion = recibir_instruccion(fd_placa);

      //Decode & Execute
      log_info(logger, "## JID: %d - Ejecutando: %s", jid, instruccion);

      bool pc_modificado = false;
      t_tipo_instruccion tipo = decodificar_y_ejecutar(instruccion, &contexto, &pc_modificado);

      if (tipo == INST_INIT_JOB) {
        char* archivo = obtener_parametro_archivo(instruccion);
        enviar_solicitud_init_job(fd_planificador, jid, archivo);
        free(archivo);
        recibir_ack(fd_planificador);
      } else if (tipo == INST_EXIT) {
        enviar_devolver_job(fd_planificador, jid, &contexto, 1);
        ejecutando_job = false;
      }

      free(instruccion);

      if (!ejecutando_job) break;

      //Actualización del Program Counter
      if (!pc_modificado) {
      contexto.pc++;
      }

      //Check Interrupt
      enviar_consultar_interrupcion(fd_planificador, jid);
      uint8_t desalojar = recibir_respuesta_interrupcion(fd_planificador);

      if (desalojar == 1) {
        log_info(logger, "## Interrupción recibida");
        enviar_devolver_job(fd_planificador, jid, &contexto, 0);
        ejecutando_job = false;
      }
    }
  }

  cerrar_conexion(fd_planificador);
  cerrar_conexion(fd_placa);
  log_destroy(logger);
  config_destroy(config);

  return EXIT_SUCCESS;
}
