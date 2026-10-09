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

typedef enum { NEW, READY, EXEC, EXIT_ESTADO } t_estado_job;
enum { FIN_QUANTUM = 0, FIN_EJECUCION = 1 };  // motivo de DEVOLVER_JOB

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
int jobs_admitidos = 0;  // Jobs en READY o EXEC
pthread_mutex_t mutex_jobs =
    PTHREAD_MUTEX_INITIALIZER;  // protege todo lo de arriba

int fd_placa;
pthread_mutex_t mutex_fd_placa = PTHREAD_MUTEX_INITIALIZER;

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

void crear_job_en_placa(uint32_t jid, char* archivo) {
  pthread_mutex_lock(&mutex_fd_placa);
  t_paquete* paquete = crear_paquete(CREAR_JOB);
  agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
  agregar_a_paquete(paquete, archivo, strlen(archivo) + 1);
  enviar_paquete(paquete, fd_placa);
  eliminar_paquete(paquete);
  recibir_ack(fd_placa);  // consume op_code + size antes de soltar el lock
  pthread_mutex_unlock(&mutex_fd_placa);
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

// Llamar con mutex_jobs tomado
static void finalizar_job(t_job* job, char* motivo) {
  cambiar_estado(job, EXIT_ESTADO);
  log_info(logger,
           "## (%u) finalizó su ejecución con motivo de %s. Estadísticas: "
           "Tiempo total: %" PRId64 "ms - Tiempo en NEW: %" PRId64
           "ms - Tiempo en READY: "
           "%" PRId64
           "ms - Memoria máxima: 0 - Syscalls: %d - Page Faults: 0 - "
           "Transiciones READY-EXEC: %d",
           job->jid, motivo, ms_entre(job->creado, job->ultimo_cambio),
           job->ms_en_new, job->ms_en_ready, job->syscalls, job->veces_en_exec);

  char* clave = string_itoa(job->jid);
  dictionary_remove(jobs, clave);
  free(clave);
  free(job->archivo_pseudocodigo);
  free(job);
  jobs_admitidos--;
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

  pthread_mutex_lock(&mutex_jobs);
  t_job* job = buscar_job(jid);
  if (job != NULL) {
    job->contexto = *contexto;  // copia el contenido, no el puntero
    if (motivo == FIN_EJECUCION) {
      job->syscalls++;
      log_info(logger, "## (%u) - Solicitó syscall: EXIT", jid);
      finalizar_job(job, "EXIT");
    } else {
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

  largo_plazo();
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

// El Core se desconectó: su Job vuelve a READY y el Core sale de la lista
static void desconectar_core(t_core* core) {
  pthread_mutex_lock(&mutex_jobs);
  if (core->job != NULL) {
    cambiar_estado(core->job, READY);
    queue_push(cola_ready, core->job);
  }
  list_remove_element(cores, core);
  pthread_mutex_unlock(&mutex_jobs);
  free(core);

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
  fd_placa = crear_conexion(config_get_string_value(config, "IP_PLACA"),
                            config_get_string_value(config, "PUERTO_PLACA"));
  enviar_handshake(fd_placa, "PLANIFICADOR");

  int fd_storage =
      crear_conexion(config_get_string_value(config, "IP_STORAGE"),
                     config_get_string_value(config, "PUERTO_STORAGE"));
  enviar_handshake(fd_storage, "PLANIFICADOR");

  // Job inicial (JID 0)
  crear_job(argv[2]);

  // Servidor para los Cores
  int fd_servidor =
      iniciar_servidor(config_get_string_value(config, "PUERTO_ESCUCHA"));
  log_info(logger, "Servidor listo para recibir Cores");
  escuchar_conexiones(fd_servidor, atender_conexion);  // no vuelve

  return EXIT_SUCCESS;
}
