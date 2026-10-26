#include "ejecucion.h"

#include <commons/log.h>
#include <commons/string.h>
#include <stdlib.h>
#include <string.h>

#include "instrucciones.h"
#include "mmu.h"
#include "protocolo_core.h"

extern t_log* logger;

static t_resultado_ejecucion resultado_de(t_resultado_mmu resultado_mmu) {
  if (resultado_mmu == MMU_PAGE_FAULT) return EJEC_PAGE_FAULT;
  if (resultado_mmu == MMU_SIN_CONEXION) return EJEC_SIN_CONEXION;
  return EJEC_OK;
}

//E1, E2 y E3 son los operandos de los 3 primeros parámetros.
//FORWARD / BACKWARD leen E1 y E2 y escriben E3.
//UPDATE lee E1, E2 y E3 y escribe E2 y E3.
t_resultado_ejecucion ejecutar_entrenamiento(char* instruccion, uint32_t jid, t_contexto* contexto, uint32_t* pagina_faltante) {
  char** tokens = string_split(instruccion, " ");
  bool es_update = strcmp(tokens[0], "UPDATE") == 0;
  uint32_t* operandos[3] = {&contexto->e1, &contexto->e2, &contexto->e3};

  //1) Todas las traducciones, antes de tocar memoria o registros
  t_list* pares[3];
  t_resultado_mmu resultado_mmu = MMU_OK;
  for (int i = 0; i < 3; i++) {
    pares[i] = list_create();
    uint32_t dir_logica = obtener_valor(contexto, tokens[i + 1]);
    if (resultado_mmu == MMU_OK)
      resultado_mmu = mmu_traducir(jid, dir_logica, sizeof(uint32_t), pares[i], pagina_faltante);
  }
  t_resultado_ejecucion resultado = resultado_de(resultado_mmu);

  if (resultado == EJEC_OK) {
    //2) Cargar solo los operandos que la instrucción lee
    bool ok = true;
    for (int i = 0; i < (es_update ? 3 : 2); i++)
      ok = ok && mmu_leer(jid, pares[i], operandos[i]);

    //3) Ejecutar
    if (es_update) {
      contexto->e2 = (contexto->e2 + contexto->e1) / 2;
      contexto->e3 = contexto->e3 - contexto->e2;
    } else {
      uint32_t* acumulador = obtener_registro(contexto, tokens[4]);
      uint32_t producto = contexto->e1 * contexto->e2;
      if (strcmp(tokens[0], "FORWARD") == 0)
        *acumulador = *acumulador + producto;
      else
        *acumulador = *acumulador - producto;
      contexto->e3 = *acumulador;
    }

    //4) Escribir en memoria los operandos que se modificaron
    if (es_update) ok = ok && mmu_escribir(jid, pares[1], &contexto->e2);
    ok = ok && mmu_escribir(jid, pares[2], &contexto->e3);
    if (!ok) resultado = EJEC_SIN_CONEXION;
  }

  for (int i = 0; i < 3; i++) list_destroy_and_destroy_elements(pares[i], free);
  string_array_destroy(tokens);
  return resultado;
}

//ALLOC (Registro, Tamaño): el registro recibe la dirección lógica
t_resultado_ejecucion ejecutar_alloc(char* instruccion, uint32_t jid,  t_contexto* contexto, int fd_planificador) {
  char** tokens = string_split(instruccion, " ");
  uint32_t* registro = obtener_registro(contexto, tokens[1]);
  uint32_t tamanio = obtener_valor(contexto, tokens[2]);
  string_array_destroy(tokens);

  enviar_solicitud_alloc(fd_planificador, jid, tamanio);
  uint32_t dir_logica;
  int ok = recibir_respuesta_alloc(fd_planificador, &dir_logica);
  if (ok < 0) return EJEC_SIN_CONEXION;
  if (ok == 0) { //Sin lugar en el Offload: se devuelve el Job para que lo finalicen
    log_warning(logger, "JID: %u - ALLOC de %u bytes rechazado", jid, tamanio);
    enviar_devolver_job(fd_planificador, jid, contexto, FIN_ERROR);
    return EJEC_JOB_DEVUELTO;
  }
  *registro = dir_logica;
  return EJEC_OK;
}

//FREE (Registro): no hace falta traducir, viaja la dirección lógica
t_resultado_ejecucion ejecutar_free(char* instruccion, uint32_t jid, t_contexto* contexto, int fd_planificador) {
  char** tokens = string_split(instruccion, " ");
  uint32_t dir_logica = obtener_valor(contexto, tokens[1]);
  string_array_destroy(tokens);

  enviar_solicitud_free(fd_planificador, jid, dir_logica);
  return recibir_ack(fd_planificador) ? EJEC_OK : EJEC_SIN_CONEXION;
}

static uint8_t tipo_syscall(char* operacion) {
  if (strcmp(operacion, "LOAD_BATCH") == 0) return SYS_LOAD_BATCH;
  if (strcmp(operacion, "LABEL") == 0) return SYS_LABEL;
  if (strcmp(operacion, "REPORT") == 0) return SYS_REPORT;
  if (strcmp(operacion, "SAVE_CHECKPOINT") == 0) return SYS_SAVE_CHECKPOINT;
  if (strcmp(operacion, "LOAD_CHECKPOINT") == 0) return SYS_LOAD_CHECKPOINT;
  return SYS_DELETE_CHECKPOINT;
}

// LOAD_BATCH archivo dir tam | LABEL dir tam | REPORT dir tam
// SAVE_CHECKPOINT / LOAD_CHECKPOINT nombre dir tam | DELETE_CHECKPOINT nombre
t_resultado_ejecucion ejecutar_syscall_bloqueante(
    char* instruccion,
    uint32_t jid,
    t_contexto* contexto,
    int fd_planificador,
    uint32_t* pagina_faltante) {
  char** tokens = string_split(instruccion, " ");
  int cantidad = string_array_size(tokens);
  uint8_t tipo = tipo_syscall(tokens[0]);
  char* nombre = (tipo == SYS_LABEL || tipo == SYS_REPORT) ? "" : tokens[1];
  uint32_t dir_logica = 0, tamanio = 0;
  if (tipo != SYS_DELETE_CHECKPOINT) {  //dirección y tamaño: los 2 últimos
    dir_logica = obtener_valor(contexto, tokens[cantidad - 2]);
    tamanio = obtener_valor(contexto, tokens[cantidad - 1]);
  }

  //El Core traduce el rango antes de devolver el Job
  t_list* pares = list_create();
  t_resultado_ejecucion resultado = resultado_de(mmu_traducir(jid, dir_logica, tamanio, pares, pagina_faltante));
  if (resultado == EJEC_OK) {
    contexto->pc++;  //al volver de BLOCKED sigue con la próxima instrucción
    enviar_syscall_bloqueante(fd_planificador, jid, contexto, tipo, nombre, tamanio, pares);
    mmu_olvidar_lockeos();  //quedan lockeadas hasta que termine el servicio
    resultado = EJEC_JOB_DEVUELTO;
  }

  list_destroy_and_destroy_elements(pares, free);
  string_array_destroy(tokens);
  return resultado;
}