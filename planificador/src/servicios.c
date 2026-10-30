#define _POSIX_C_SOURCE 200809L
#include "servicios.h"

#include <commons/collections/queue.h>
#include <commons/log.h>
#include <commons/string.h>
#include <pthread.h>
#include <readline/readline.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "placa_cliente.h"
#include "planificador.h"

/*  Cada servicio atiende un pedido a la vez, en orden de llegada (FIFO): una
    cola propia, un mutex para la cola y un semáforo que cuenta los pendientes
    (el hilo duerme en sem_wait mientras no haya nada que hacer). */
typedef struct {
  char* nombre;
  t_queue* cola;
  pthread_mutex_t mutex;
  sem_t pendientes;
  void (*operar)(t_pedido_servicio*);
} t_servicio;

static t_servicio loader;
static t_servicio labeler;
static t_servicio logger_servicio;  // "logger" ya es el log del módulo

static char* path_datos;
static int retardo_loader;
static t_log* reportes;  // archivo de reportes, distinto del log del módulo

static void dormir_ms(int ms) {
  struct timespec espera = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&espera, NULL);
}

void destruir_pedido_servicio(t_pedido_servicio* pedido) {
  free(pedido->nombre);
  free(pedido->pares);
  free(pedido);
}

// Reparte los datos entre los pedazos del rango (uno por página)
static void escribir_en_pares(t_pedido_servicio* pedido, uint8_t* datos) {
  for (uint32_t i = 0; i < pedido->cant_pares; i++) {
    escribir_memoria_job(pedido->jid, pedido->pares[i].dir_fisica, pedido->pares[i].tamanio, datos);
    datos += pedido->pares[i].tamanio;
  }
}

static void leer_de_pares(t_pedido_servicio* pedido, uint8_t* destino) {
  for (uint32_t i = 0; i < pedido->cant_pares; i++) {
    leer_memoria_job(pedido->jid, pedido->pares[i].dir_fisica, pedido->pares[i].tamanio, destino);
    destino += pedido->pares[i].tamanio;
  }
}

// Loader (LOAD_BATCH): <tamaño> bytes del archivo en PATH_DATOS
static void operar_loader(t_pedido_servicio* pedido) {
  uint8_t* datos = calloc(1, pedido->tamanio + 1);
  char* path = string_starts_with(pedido->nombre, "/")
                   ? string_duplicate(pedido->nombre)
               : string_ends_with(path_datos, "/")
                   ? string_from_format("%s%s", path_datos, pedido->nombre)
                   : string_from_format("%s/%s", path_datos, pedido->nombre);
  FILE* archivo = fopen(path, "rb");
  if (archivo == NULL) {
    log_error(logger, "## (%u) - Loader: no existe %s, se cargan ceros",
              pedido->jid, path);
  } else {
    size_t leidos = fread(datos, 1, pedido->tamanio, archivo);
    if (leidos < pedido->tamanio)
      log_warning(logger, "## (%u) - Loader: %s tiene %zu bytes, faltan %u",
                  pedido->jid, path, leidos, pedido->tamanio);
    fclose(archivo);
  }
  free(path);
  dormir_ms(retardo_loader);
  escribir_en_pares(pedido, datos);
  free(datos);
}

// Labeler (LABEL): <tamaño> caracteres por teclado
static void operar_labeler(t_pedido_servicio* pedido) {
  char* datos = calloc(1, pedido->tamanio + 1);
  uint32_t cargados = 0;
  log_info(logger, "## (%u) - Ingrese %u caracteres:", pedido->jid,
           pedido->tamanio);
  while (cargados < pedido->tamanio) {
    char* linea = readline("> ");
    if (linea == NULL) break;  // se cerró stdin: lo que falta queda en 0
    uint32_t faltan = pedido->tamanio - cargados;
    uint32_t largo = strlen(linea) < faltan ? strlen(linea) : faltan;
    memcpy(datos + cargados, linea, largo);  // lo que sobra se descarta
    cargados += largo;
    free(linea);
  }
  escribir_en_pares(pedido, (uint8_t*)datos);
  free(datos);
}

// Logger (REPORT): <tamaño> bytes de memoria, en hexa, al archivo de reportes
static void operar_logger(t_pedido_servicio* pedido) {
  uint8_t* datos = calloc(1, pedido->tamanio + 1);
  leer_de_pares(pedido, datos);
  char* hexa = string_new();
  for (uint32_t i = 0; i < pedido->tamanio; i++)
    string_append_with_format(&hexa, "%02X", datos[i]);
  log_info(reportes, "## (%u) - %s", pedido->jid, hexa);
  free(hexa);
  free(datos);
}

static void* atender_servicio(void* arg) {
  t_servicio* servicio = arg;
  while (1) {
    sem_wait(&servicio->pendientes);
    pthread_mutex_lock(&servicio->mutex);
    t_pedido_servicio* pedido = queue_pop(servicio->cola);
    pthread_mutex_unlock(&servicio->mutex);

    log_info(logger, "## (%u) inicia %s", pedido->jid, servicio->nombre);
    servicio->operar(pedido);
    deslockear_paginas_en_placa(pedido->jid);  // antes de pasarlo a READY
    log_info(logger, "## (%u) finalizó %s y pasa a READY", pedido->jid,
             servicio->nombre);
    desbloquear_job(pedido->jid);
    destruir_pedido_servicio(pedido);
  }
  return NULL;
}

static void iniciar_servicio(t_servicio* servicio, char* nombre,
                             void (*operar)(t_pedido_servicio*)) {
  servicio->nombre = nombre;
  servicio->cola = queue_create();
  pthread_mutex_init(&servicio->mutex, NULL);
  sem_init(&servicio->pendientes, 0, 0);
  servicio->operar = operar;
  pthread_t hilo;
  pthread_create(&hilo, NULL, atender_servicio, servicio);
  pthread_detach(hilo);
}

void servicios_iniciar(t_config* config) {
  path_datos = config_get_string_value(config, "PATH_DATOS");
  retardo_loader = config_get_int_value(config, "RETARDO_LOADER");
  reportes = log_create(config_get_string_value(config, "PATH_REPORTES"),
                        "Logger", false, LOG_LEVEL_INFO);
  // Sin esto, mientras el Labeler espera teclado readline se queda con las
  // señales y Ctrl+C no corta el Planificador
  rl_catch_signals = 0;
  iniciar_servicio(&loader, "LOADER", operar_loader);
  iniciar_servicio(&labeler, "LABELER", operar_labeler);
  iniciar_servicio(&logger_servicio, "LOGGER", operar_logger);
}

void encolar_pedido_servicio(t_pedido_servicio* pedido) {
  t_servicio* servicio = &logger_servicio;  // SYS_REPORT
  if (pedido->tipo == SYS_LOAD_BATCH) servicio = &loader;
  if (pedido->tipo == SYS_LABEL) servicio = &labeler;

  pthread_mutex_lock(&servicio->mutex);
  queue_push(servicio->cola, pedido);
  pthread_mutex_unlock(&servicio->mutex);
  sem_post(&servicio->pendientes);
}