#include <commons/collections/dictionary.h>
#include <commons/collections/list.h>
#include <commons/collections/queue.h>
#include <commons/config.h>
#include <commons/log.h>
#include <commons/string.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utils/conexion.h>
#include <utils/paquete.h>
#include <utils/servidor.h>

#include "placa_cliente.h"
#include "planificador.h"
#include "servicios.h"

// Los motivos de DEVOLVER_JOB (FIN_QUANTUM, FIN_EJECUCION, PAGE_FAULT,
// FIN_ERROR) ahora están en utils/paquete.h
typedef enum { NEW, READY, EXEC, BLOCKED, EXIT_ESTADO } t_estado_job;

typedef struct {
  uint32_t jid;
  t_estado_job estado;
  t_contexto contexto;
  char* archivo_pseudocodigo;
  struct timespec inicio_rafaga;  // cuándo se le asignó el Core (para RR)
  // Estadísticas
  struct timespec creado;         // cuándo entró a NEW
  struct timespec ultimo_cambio;  // cuándo entró al estado actual
  int64_t ms_en_new;
  int64_t ms_en_ready;
  int syscalls;
  int veces_en_exec;  // transiciones READY -> EXEC
  int page_faults;
  uint32_t memoria_actual;  // bytes de los ALLOC vigentes
  uint32_t memoria_maxima;
  t_dictionary* asignaciones;  // dir. lógica (string) -> uint32_t* tamaño
} t_job;

typedef struct {
  int fd;
  bool libre;
  t_job* job;  // Job que está ejecutando, NULL si está libre
} t_core;

t_log* logger;

typedef enum { ALG_FIFO, ALG_RR } t_algoritmo;

t_algoritmo algoritmo;
int rr_quantum;  // ms
int grado_multiprogramacion;

t_dictionary* jobs;  // JID (string) -> t_job*
t_queue* cola_new;
t_queue* cola_ready;
t_list* cores;  // t_core*
uint32_t proximo_jid = 0;
int jobs_admitidos = 0;  // Jobs en READY, EXEC o BLOCKED
pthread_mutex_t mutex_jobs =
    PTHREAD_MUTEX_INITIALIZER;  // protege todo lo de arriba

static int64_t ms_entre(struct timespec desde, struct timespec hasta) {
  return (hasta.tv_sec - desde.tv_sec) * 1000 +
         (hasta.tv_nsec - desde.tv_nsec) / 1000000;
}

static struct timespec ahora(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t;
}

static char* nombre_estado(t_estado_job estado) {
  switch (estado) {
    case NEW:
      return "NEW";
    case READY:
      return "READY";
    case EXEC:
      return "EXEC";
    case BLOCKED:
      return "BLOCKED";
    default:
      return "EXIT";
  }
}

// Llamar con mutex_jobs tomado
static void cambiar_estado(t_job* job, t_estado_job nuevo) {
  struct timespec t = ahora();
  int64_t ms = ms_entre(job->ultimo_cambio, t);
  if (job->estado == NEW) job->ms_en_new += ms;
  if (job->estado == READY) job->ms_en_ready += ms;
  if (nuevo == EXEC) job->veces_en_exec++;

  log_info(logger, "## (%u) Pasa del estado %s al estado %s", job->jid,
           nombre_estado(job->estado), nombre_estado(nuevo));
  job->estado = nuevo;
  job->ultimo_cambio = t;
}

// Largo plazo: NEW -> READY por FIFO mientras lo permita el grado
static void largo_plazo(void) {
  pthread_mutex_lock(&mutex_jobs);
  while (jobs_admitidos < grado_multiprogramacion &&
         !queue_is_empty(cola_new)) {
    t_job* job = queue_pop(cola_new);
    cambiar_estado(job, READY);
    queue_push(cola_ready, job);
    jobs_admitidos++;
  }
  pthread_mutex_unlock(&mutex_jobs);
}

static bool core_esta_libre(void* core) { return ((t_core*)core)->libre; }

// Corto plazo: READY -> EXEC mientras haya un Core libre.
// FIFO y RR eligen igual (el primero de la cola); cambia cuándo se desaloja.
// NUNCA llamar con mutex_jobs tomado.
static void corto_plazo(void) {
  pthread_mutex_lock(&mutex_jobs);
  while (!queue_is_empty(cola_ready)) {
    t_core* core = list_find(cores, core_esta_libre);
    if (core == NULL) break;

    t_job* job = queue_pop(cola_ready);
    cambiar_estado(job, EXEC);
    job->inicio_rafaga = ahora();
    core->libre = false;
    core->job = job;

    t_paquete* paquete = crear_paquete(ASIGNAR_JOB);
    agregar_a_paquete(paquete, &job->jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, &job->contexto, sizeof(t_contexto));
    enviar_paquete(paquete, core->fd);
    eliminar_paquete(paquete);
  }
  pthread_mutex_unlock(&mutex_jobs);
}

static void crear_job(char* archivo) {
  pthread_mutex_lock(&mutex_jobs);
  uint32_t jid = proximo_jid++;
  pthread_mutex_unlock(&mutex_jobs);

  // Primero la Placa: el Job no entra a las colas hasta que ella confirma
  crear_job_en_placa(jid, archivo);

  t_job* job = calloc(1, sizeof(t_job));  // contexto y contadores en 0
  job->jid = jid;
  job->estado = NEW;
  job->archivo_pseudocodigo = strdup(archivo);
  job->creado = ahora();
  job->ultimo_cambio = job->creado;
  job->asignaciones = dictionary_create();

  pthread_mutex_lock(&mutex_jobs);
  char* clave = string_itoa(jid);
  dictionary_put(jobs, clave, job);
  free(clave);
  queue_push(cola_new, job);
  log_info(logger, "## (%u) Se crea el Job - Estado: NEW", jid);
  pthread_mutex_unlock(&mutex_jobs);

  largo_plazo();
  corto_plazo();
}

// Llamar con mutex_jobs tomado
static t_job* buscar_job(uint32_t jid) {
  char* clave = string_itoa(jid);
  t_job* job = dictionary_get(jobs, clave);
  free(clave);
  return job;
}

// Llamar con mutex_jobs tomado. Saca al Job de las estructuras del
// Planificador; el que llama, ya sin el mutex, tiene que llamar a
// destruir_job (que le avisa a la Placa: es un pedido de red).
static void finalizar_job(t_job* job, char* motivo) {
  cambiar_estado(job, EXIT_ESTADO);
  log_info(logger,
           "## (%u) finalizó su ejecución con motivo de %s. Estadísticas: "
           "Tiempo total: %" PRId64 "ms - Tiempo en NEW: %" PRId64
           "ms - Tiempo en READY: "
           "%" PRId64
           "ms - Memoria máxima: %u bytes - Syscalls: %d - Page Faults: %d - "
           "Transiciones READY-EXEC: %d",
           job->jid, motivo, ms_entre(job->creado, job->ultimo_cambio),
           job->ms_en_new, job->ms_en_ready, job->memoria_maxima, job->syscalls,
           job->page_faults, job->veces_en_exec);

  t_contexto* c = &job->contexto;
  log_info(logger,
           "## (%u) Contexto final: PC=%u AX=%u BX=%u CX=%u DX=%u P0=%u P1=%u "
           "P2=%u P3=%u P4=%u P5=%u E1=%u E2=%u E3=%u",
           job->jid, c->pc, c->ax, c->bx, c->cx, c->dx, c->p0, c->p1, c->p2,
           c->p3, c->p4, c->p5, c->e1, c->e2, c->e3);

  char* clave = string_itoa(job->jid);
  dictionary_remove(jobs, clave);
  free(clave);
  jobs_admitidos--;
}

// Llamar SIN mutex_jobs: la Placa libera marcos, Offload y tabla de páginas
static void destruir_job(t_job* job) {
  finalizar_job_en_placa(job->jid);
  dictionary_destroy_and_destroy_elements(job->asignaciones, free);
  free(job->archivo_pseudocodigo);
  free(job);
}

// Campos de un mensaje armado con agregar_a_paquete. NULL si se cortó la
// conexión o llegaron menos campos de los esperados.
static t_list* recibir_campos(int fd_conexion, int esperados) {
  int size;
  void* buffer = recibir_buffer(&size, fd_conexion);
  if (buffer == NULL) return NULL;
  t_list* campos = deserializar_paquete(buffer, size);
  free(buffer);
  if (list_size(campos) < esperados) {
    list_destroy_and_destroy_elements(campos, free);
    return NULL;
  }
  return campos;
}

static uint32_t campo_u32(t_list* campos, int indice) {
  return *(uint32_t*)list_get(campos, indice);
}

static void registrar_syscall(uint32_t jid, char* nombre) {
  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(jid);
  if (job != NULL) job->syscalls++;
  log_info(logger, "## (%u) - Solicitó syscall: %s", jid, nombre);
  pthread_mutex_unlock(&mutex_jobs);
}

void desbloquear_job(uint32_t jid) {
  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(jid);
  if (job != NULL && job->estado == BLOCKED) {
    cambiar_estado(job, READY);
    queue_push(cola_ready, job);
  }
  pthread_mutex_unlock(&mutex_jobs);
  corto_plazo();
}

typedef struct {
  uint32_t jid;
  uint32_t nro_pagina;
} t_page_fault;

// Un hilo por Page Fault: así el hilo que atiende al Core no queda esperando
// a la Placa (y al Offload) y el Core puede recibir otro Job enseguida
static void* atender_page_fault(void* arg) {
  t_page_fault* page_fault = arg;
  cargar_pagina_en_placa(page_fault->jid, page_fault->nro_pagina);
  desbloquear_job(page_fault->jid);
  free(page_fault);
  return NULL;
}

static void atender_consultar_interrupcion(int fd_conexion) {
  int size;
  void* buffer = recibir_buffer(&size, fd_conexion);
  if (buffer == NULL) return;
  t_list* campos = deserializar_paquete(buffer, size);
  uint32_t jid = *(uint32_t*)list_get(campos, 0);
  list_destroy_and_destroy_elements(campos, free);
  free(buffer);

  uint8_t desalojar = 0;
  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(jid);
  if (job != NULL && algoritmo == ALG_RR &&
      ms_entre(job->inicio_rafaga, ahora()) >= rr_quantum) {
    desalojar = 1;
    log_info(logger, "## (%u) - Desalojado por fin de quantum", jid);
  }
  pthread_mutex_unlock(&mutex_jobs);

  enviar_respuesta_interrupcion(fd_conexion, desalojar);
}

static void atender_devolver_job(int fd_conexion, t_core* core) {
  int size;
  void* buffer = recibir_buffer(&size, fd_conexion);
  if (buffer == NULL) return;
  t_list* campos = deserializar_paquete(buffer, size);
  uint32_t jid = *(uint32_t*)list_get(campos, 0);
  t_contexto* contexto = list_get(campos, 1);
  uint8_t motivo = *(uint8_t*)list_get(campos, 2);

  t_job* finalizado = NULL;
  t_page_fault* page_fault = NULL;
  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(jid);
  if (job != NULL) {
    job->contexto = *contexto;  // copia el contenido, no el puntero
    if (motivo == FIN_EJECUCION) {
      job->syscalls++;
      log_info(logger, "## (%u) - Solicitó syscall: EXIT", jid);
      finalizar_job(job, "EXIT");
      finalizado = job;
    } else if (motivo == FIN_ERROR) {  // ALLOC sin lugar en el Offload
      finalizar_job(job, "OUT_OF_MEMORY");
      finalizado = job;
    } else if (motivo == PAGE_FAULT) {
      job->page_faults++;
      cambiar_estado(job, BLOCKED);
      page_fault = malloc(sizeof(t_page_fault));
      page_fault->jid = jid;
      page_fault->nro_pagina = *(uint32_t*)list_get(campos, 3);
    } else {  // FIN_QUANTUM
      cambiar_estado(job, READY);
      queue_push(cola_ready, job);
    }
  }
  if (core != NULL) {
    core->libre = true;
    core->job = NULL;
  }
  pthread_mutex_unlock(&mutex_jobs);  // soltar ANTES de planificar

  list_destroy_and_destroy_elements(campos, free);
  free(buffer);

  if (finalizado != NULL) destruir_job(finalizado);
  if (page_fault != NULL) {
    pthread_t hilo;
    pthread_create(&hilo, NULL, atender_page_fault, page_fault);
    pthread_detach(hilo);
  }
  largo_plazo();
  corto_plazo();
}

// ALLOC: no bloquea. Se deriva a la Placa y se le contesta al Core.
static void atender_alloc(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 2);
  if (campos == NULL) return;
  uint32_t jid = campo_u32(campos, 0);
  uint32_t tamanio = campo_u32(campos, 1);
  list_destroy_and_destroy_elements(campos, free);

  registrar_syscall(jid, "ALLOC");
  uint32_t dir_logica = 0;
  uint8_t ok = asignar_espacio_en_placa(jid, tamanio, &dir_logica);
  if (ok) {
    pthread_mutex_lock(&mutex_jobs);
    t_job* job = buscar_job(jid);
    if (job != NULL) {
      uint32_t* guardado = malloc(sizeof(uint32_t));
      *guardado = tamanio;
      char* clave = string_itoa(dir_logica);
      dictionary_put(job->asignaciones, clave, guardado);
      free(clave);
      job->memoria_actual += tamanio;
      if (job->memoria_actual > job->memoria_maxima)
        job->memoria_maxima = job->memoria_actual;
    }
    pthread_mutex_unlock(&mutex_jobs);
  }
  // Si no hubo lugar (ok = 0) el Core devuelve el Job con FIN_ERROR
  t_paquete* paquete = crear_paquete(RESPUESTA_ALLOC);
  agregar_a_paquete(paquete, &ok, sizeof(uint8_t));
  agregar_a_paquete(paquete, &dir_logica, sizeof(uint32_t));
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

// FREE: no bloquea
static void atender_free(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 2);
  if (campos == NULL) return;
  uint32_t jid = campo_u32(campos, 0);
  uint32_t dir_logica = campo_u32(campos, 1);
  list_destroy_and_destroy_elements(campos, free);

  registrar_syscall(jid, "FREE");
  liberar_espacio_en_placa(jid, dir_logica);

  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(jid);
  if (job != NULL) {
    char* clave = string_itoa(dir_logica);
    uint32_t* tamanio = dictionary_remove(job->asignaciones, clave);
    free(clave);
    if (tamanio != NULL) job->memoria_actual -= *tamanio;
    free(tamanio);
  }
  pthread_mutex_unlock(&mutex_jobs);
  enviar_ack(fd_conexion);
}

static char* nombre_syscall(uint8_t tipo) {
  char* nombres[] = {"LOAD_BATCH",      "LABEL",           "REPORT",
                     "SAVE_CHECKPOINT", "LOAD_CHECKPOINT", "DELETE_CHECKPOINT"};
  return tipo <= SYS_DELETE_CHECKPOINT ? nombres[tipo] : "DESCONOCIDA";
}
// LOAD_BATCH / LABEL / REPORT (y Checkpoints): el Job se bloquea, el Core
// queda libre y el pedido va a la cola del servicio
static void atender_syscall_bloqueante(int fd_conexion, t_core* core) {
  t_list* campos = recibir_campos(fd_conexion, 7);
  if (campos == NULL) return;
  t_pedido_servicio* pedido = malloc(sizeof(t_pedido_servicio));
  pedido->jid = campo_u32(campos, 0);
  t_contexto* contexto = list_get(campos, 1);
  pedido->tipo = *(uint8_t*)list_get(campos, 2);
  pedido->nombre = strdup(list_get(campos, 3));
  pedido->tamanio = campo_u32(campos, 4);
  pedido->cant_pares = campo_u32(campos, 5);
  pedido->pares = malloc(sizeof(t_par_fisico) * (pedido->cant_pares + 1));
  memcpy(pedido->pares, list_get(campos, 6),
         sizeof(t_par_fisico) * pedido->cant_pares);

  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(pedido->jid);
  if (job != NULL) {
    job->contexto = *contexto;  // el PC ya apunta a la instrucción siguiente
    job->syscalls++;
    log_info(logger, "## (%u) - Solicitó syscall: %s", pedido->jid,
             nombre_syscall(pedido->tipo));
    cambiar_estado(job, BLOCKED);
  }
  if (core != NULL) {
    core->libre = true;
    core->job = NULL;
  }
  pthread_mutex_unlock(&mutex_jobs);
  list_destroy_and_destroy_elements(campos, free);

  if (pedido->tipo >= SYS_SAVE_CHECKPOINT) {
    // Check 4: todavía no hay Storage. El Job sigue como si hubiera terminado.
    log_warning(logger, "## (%u) - %s se implementa en el Check 4", pedido->jid,
                nombre_syscall(pedido->tipo));
    deslockear_paginas_en_placa(pedido->jid);
    desbloquear_job(pedido->jid);
    destruir_pedido_servicio(pedido);
  } else {
    encolar_pedido_servicio(pedido);
  }
  corto_plazo();
}

static void atender_init_job(int fd_conexion) {
  int size;
  void* buffer = recibir_buffer(&size, fd_conexion);
  if (buffer == NULL) return;
  t_list* campos = deserializar_paquete(buffer, size);
  uint32_t jid = *(uint32_t*)list_get(campos, 0);
  char* archivo = list_get(campos, 1);

  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(jid);
  if (job != NULL) job->syscalls++;
  log_info(logger, "## (%u) - Solicitó syscall: INIT_JOB", jid);
  pthread_mutex_unlock(&mutex_jobs);

  crear_job(archivo);  // adentro hace strdup, se puede destruir la lista

  list_destroy_and_destroy_elements(campos, free);
  free(buffer);

  enviar_ack(fd_conexion);  // no bloquea: el Core sigue con el mismo Job
}

// El Core se desconectó: el Core sale de la lista, se deslockean las páginas
// de su Job y el Job vuelve a READY
static void desconectar_core(t_core* core) {
  pthread_mutex_lock(&mutex_jobs);
  t_job* job = core->job;
  list_remove_element(cores, core);
  pthread_mutex_unlock(&mutex_jobs);
  free(core);

  if (job != NULL) {
    deslockear_paginas_en_placa(job->jid);  // pedido a la Placa: sin mutex
    pthread_mutex_lock(&mutex_jobs);
    cambiar_estado(job, READY);
    queue_push(cola_ready, job);
    pthread_mutex_unlock(&mutex_jobs);
  }
  corto_plazo();
}

void atender_conexion(int fd_conexion) {
  t_core* core = NULL;  // se registra recién con el Handshake
  int operacion = recibir_operacion(fd_conexion);
  while (operacion != -1) {
    switch (operacion) {
      case HANDSHAKE: {
        char* nombre_modulo = recibir_handshake(fd_conexion);
        log_info(logger, "## Módulo: %s", nombre_modulo);
        free(nombre_modulo);

        if (core == NULL) {
          core = malloc(sizeof(t_core));
          core->fd = fd_conexion;
          core->libre = true;
          core->job = NULL;
          pthread_mutex_lock(&mutex_jobs);
          list_add(cores, core);
          pthread_mutex_unlock(&mutex_jobs);
          corto_plazo();
        }
        break;
      }
      case MENSAJE: {
        char* mensaje = recibir_mensaje(fd_conexion);
        log_info(logger, "Mensaje de prueba recibido: %s", mensaje);
        free(mensaje);
        break;
      }
      case CONSULTAR_INTERRUPCION: {
        atender_consultar_interrupcion(fd_conexion);
        break;
      }
      case DEVOLVER_JOB: {
        atender_devolver_job(fd_conexion, core);
        break;
      }
      case SOLICITUD_INIT_JOB: {
        atender_init_job(fd_conexion);
        break;
      }
      case SOLICITUD_ALLOC: {
        atender_alloc(fd_conexion);
        break;
      }
      case SOLICITUD_FREE: {
        atender_free(fd_conexion);
        break;
      }
      case SYSCALL_BLOQUEANTE: {
        atender_syscall_bloqueante(fd_conexion, core);
        break;
      }
      default: {
        // Siempre se consume el buffer, si no el socket queda desfasado
        int size;
        free(recibir_buffer(&size, fd_conexion));
        log_warning(logger, "Operación desconocida: %d", operacion);
        break;
      }
    }
    operacion = recibir_operacion(fd_conexion);
  }
  if (core != NULL) desconectar_core(core);
  cerrar_conexion(fd_conexion);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Uso: %s [Archivo Config] [Path Job Inicial]\n", argv[0]);
    return EXIT_FAILURE;
  }
  t_config* config = config_create(argv[1]);
  logger = log_create(
      "planificador.log", "Planificador", 1,
      log_level_from_string(config_get_string_value(config, "LOG_LEVEL")));

  char* nombre_algoritmo =
      config_get_string_value(config, "ALGORITMO_PLANIFICACION");
  algoritmo = strcmp(nombre_algoritmo, "RR") == 0 ? ALG_RR : ALG_FIFO;
  rr_quantum = config_get_int_value(config, "RR_QUANTUM");
  grado_multiprogramacion =
      config_get_int_value(config, "GRADO_MULTIPROGRAMACION");

  jobs = dictionary_create();
  cola_new = queue_create();
  cola_ready = queue_create();
  cores = list_create();

  // Cliente de Placa y Storage (la consigna pide conectarse a ambos ANTES de
  // levantar el servidor propio)
  int fd_placa =
      crear_conexion(config_get_string_value(config, "IP_PLACA"),
                     config_get_string_value(config, "PUERTO_PLACA"));
  enviar_handshake(fd_placa, "PLANIFICADOR");
  placa_iniciar(fd_placa);  // Check 3: el fd lo maneja placa_cliente.c

  int fd_storage =
      crear_conexion(config_get_string_value(config, "IP_STORAGE"),
                     config_get_string_value(config, "PUERTO_STORAGE"));
  enviar_handshake(fd_storage, "PLANIFICADOR");

  servicios_iniciar(config);  // Check 3: hilos de Loader, Labeler y Logger

  // Job inicial (JID 0)
  crear_job(argv[2]);

  // Servidor para los Cores
  int fd_servidor =
      iniciar_servidor(config_get_string_value(config, "PUERTO_ESCUCHA"));
  log_info(logger, "Servidor listo para recibir Cores");
  escuchar_conexiones(fd_servidor, atender_conexion);  // no vuelve

  return EXIT_SUCCESS;
}
