#include <commons/config.h>
#include <commons/log.h>
#include <commons/string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <utils/conexion.h>
#include <utils/paquete.h>

#include "protocolo_core.h"
#include "instrucciones.h"
#include "ejecucion.h"
#include "mmu.h"

t_log* logger;

static void loguear_ejecucion(uint32_t jid, char* instruccion) {
  char* espacio = strchr(instruccion, ' ');
  if (espacio == NULL) {
    log_info(logger, "## JID: %u - Ejecutando: %s", jid, instruccion);
  } else {
    log_info(logger, "## JID: %u - Ejecutando: %.*s - %s", jid, (int)(espacio - instruccion), instruccion, espacio + 1);
  }
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Uso: %s [Archivo Config] [Identificador]\n", argv[0]);
    return EXIT_FAILURE;
  }
  t_config* config = config_create(argv[1]);
  char* nombre_log = string_from_format("core_%s.log", argv[2]);
  logger = log_create(nombre_log, "Core", 1, log_level_from_string(config_get_string_value(config, "LOG_LEVEL")));
  free(nombre_log);

  int fd_planificador =
      crear_conexion(config_get_string_value(config, "IP_PLANIFICADOR"), config_get_string_value(config, "PUERTO_PLANIFICADOR"));
  enviar_handshake(fd_planificador, "CORE");
  log_info(logger, "## Conectado a Planificador exitosamente");
  enviar_mensaje(fd_planificador, "Mensaje de prueba desde CORE");

  int fd_placa =
      crear_conexion(config_get_string_value(config, "IP_PLACA"),
                     config_get_string_value(config, "PUERTO_PLACA"));
  enviar_handshake(fd_placa, "CORE");
  log_info(logger, "## Conectado a Placa exitosamente");
  enviar_mensaje(fd_placa, "Mensaje de prueba desde CORE");

  //Check 3: el tamaño de página no está en el config del Core, lo da la Placa
  if (!mmu_iniciar(fd_placa)) {
    log_error(logger, "No se pudo obtener el tamaño de página de la Placa");
    return EXIT_FAILURE;
  }

  //Bucle principal del ciclo de instrucción
  uint32_t jid;
  t_contexto contexto;
  bool conectado = true;

  while (conectado && recibir_asignacion_job(fd_planificador, &jid, &contexto)) {
    bool ejecutando_job = true;

    while (ejecutando_job) {
      //Fetch
      log_info(logger, "## JID: %u - FETCH - Program Counter: %u", jid, contexto.pc);
      enviar_fetch(fd_placa, jid, contexto.pc);
      char* instruccion = recibir_instruccion(fd_placa);
      if (instruccion == NULL) {
        log_error(logger, "Se perdió la conexión con la Placa");
        conectado = false;
        break;
      }

      //Decode & Execute
      bool pc_modificado = false;
      t_tipo_instruccion tipo = decodificar_y_ejecutar(instruccion, &contexto, &pc_modificado);

      //Check 3: las que usan la MMU o al Planificador
      t_resultado_ejecucion resultado = EJEC_OK;
      uint32_t pagina_faltante = 0;
      switch (tipo) {
        case INST_ENTRENAMIENTO:
          resultado = ejecutar_entrenamiento(instruccion, jid, &contexto, &pagina_faltante);
          break;
        case INST_ALLOC:
          resultado = ejecutar_alloc(instruccion, jid, &contexto, fd_planificador);
          break;
        case INST_FREE:
          resultado = ejecutar_free(instruccion, jid, &contexto, fd_planificador);
          break;
        case INST_SYSCALL_BLOQUEANTE:
          resultado = ejecutar_syscall_bloqueante(instruccion, jid, &contexto, fd_planificador, &pagina_faltante);
          break;
        default:
          break;
      }

      if (tipo == INST_INVALIDA) {
        log_warning(logger, "JID: %u - Instrucción inválida o fin de archivo sin EXIT (PC %u): '%s'",
                    jid, contexto.pc, instruccion);
      } else if (resultado == EJEC_OK || resultado == EJEC_JOB_DEVUELTO) {
        loguear_ejecucion(jid, instruccion);  //con Page Fault no se ejecutó
      }

      if (tipo == INST_INIT_JOB) {
        char* archivo = obtener_parametro_archivo(instruccion);
        enviar_solicitud_init_job(fd_planificador, jid, archivo);
        free(archivo);
        recibir_ack(fd_planificador);
      } else if (tipo == INST_EXIT || tipo == INST_INVALIDA) {
        enviar_devolver_job(fd_planificador, jid, &contexto, FIN_EJECUCION);
        ejecutando_job = false;
      }

      free(instruccion);

      if (resultado == EJEC_SIN_CONEXION) {
        log_error(logger, "Se perdió la conexión con la Placa o el Planificador");
        conectado = false;
        break;
      }
      if (resultado == EJEC_PAGE_FAULT) {
        //Se suelta lo que se haya lockeado y se devuelve el Job SIN tocar el PC
        mmu_deslockear(jid);
        enviar_page_fault(fd_planificador, jid, &contexto, pagina_faltante);
        ejecutando_job = false;
      } else if (resultado == EJEC_JOB_DEVUELTO) {
        ejecutando_job = false;
      }

      if (!ejecutando_job) break;

      //Ciclo terminado con éxito: se deslockean las páginas que se usaron
      if (!mmu_deslockear(jid)) {
        log_error(logger, "Se perdió la conexión con la Placa");
        conectado = false;
        break;
      }

      //Actualización del Program Counter
      if (!pc_modificado) {
        contexto.pc++;
      }

      //Check Interrupt
      enviar_consultar_interrupcion(fd_planificador, jid);
      int desalojar = recibir_respuesta_interrupcion(fd_planificador);

      if (desalojar < 0) {
        log_error(logger, "Se perdió la conexión con el Planificador");
        conectado = false;
        break;
      }
      if (desalojar == 1) {
        log_info(logger, "## Interrupción recibida");
        enviar_devolver_job(fd_planificador, jid, &contexto, 0);
        ejecutando_job = false;
      }
    }
  }

  log_info(logger, "Finalizando Core");

  cerrar_conexion(fd_planificador);
  cerrar_conexion(fd_placa);
  log_destroy(logger);
  config_destroy(config);

  return EXIT_SUCCESS;
}