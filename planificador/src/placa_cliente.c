#include "placa_cliente.h"

#include <commons/collections/list.h>
#include <commons/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <utils/paquete.h>

#include "planificador.h"

static int fd_placa;
static pthread_mutex_t mutex_fd_placa = PTHREAD_MUTEX_INITIALIZER;

void placa_iniciar(int fd) { fd_placa = fd; }

// Manda un pedido de 1 o 2 números (jid y opcionalmente otro valor)
static void enviar_pedido(op_code codigo, uint32_t jid, uint32_t* valor) {
  t_paquete* paquete = crear_paquete(codigo);
  agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
  if (valor != NULL) agregar_a_paquete(paquete, valor, sizeof(uint32_t));
  enviar_paquete(paquete, fd_placa);
  eliminar_paquete(paquete);
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

void finalizar_job_en_placa(uint32_t jid) {
  pthread_mutex_lock(&mutex_fd_placa);
  enviar_pedido(FINALIZAR_JOB, jid, NULL);
  recibir_ack(fd_placa);
  pthread_mutex_unlock(&mutex_fd_placa);
}

bool asignar_espacio_en_placa(uint32_t jid, uint32_t tamanio,
                              uint32_t* dir_logica) {
  bool ok = false;
  pthread_mutex_lock(&mutex_fd_placa);
  enviar_pedido(ASIGNAR_ESPACIO, jid, &tamanio);
  if (recibir_operacion(fd_placa) == RESPUESTA_ALLOC) {
    int size;
    void* buffer = recibir_buffer(&size, fd_placa);
    t_list* campos = deserializar_paquete(buffer, size);
    if (list_size(campos) == 2) {
      ok = *(uint8_t*)list_get(campos, 0);
      *dir_logica = *(uint32_t*)list_get(campos, 1);
    }
    list_destroy_and_destroy_elements(campos, free);
    free(buffer);
  }
  pthread_mutex_unlock(&mutex_fd_placa);
  return ok;
}

void liberar_espacio_en_placa(uint32_t jid, uint32_t dir_logica) {
  pthread_mutex_lock(&mutex_fd_placa);
  enviar_pedido(LIBERAR_ESPACIO, jid, &dir_logica);
  recibir_ack(fd_placa);
  pthread_mutex_unlock(&mutex_fd_placa);
}

void cargar_pagina_en_placa(uint32_t jid, uint32_t nro_pagina) {
  pthread_mutex_lock(&mutex_fd_placa);
  enviar_pedido(CARGAR_PAGINA, jid, &nro_pagina);
  recibir_ack(fd_placa);
  pthread_mutex_unlock(&mutex_fd_placa);
}

void deslockear_paginas_en_placa(uint32_t jid) {
  log_info(logger, "## (%u) - Deslockeo de páginas", jid);
  pthread_mutex_lock(&mutex_fd_placa);
  placa_deslockear(fd_placa, jid);
  pthread_mutex_unlock(&mutex_fd_placa);
}

void leer_memoria_job(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                      void* destino) {
  pthread_mutex_lock(&mutex_fd_placa);
  placa_leer(fd_placa, jid, dir_fisica, tamanio, destino);
  pthread_mutex_unlock(&mutex_fd_placa);
  log_info(logger, "## (%u) - Lectura - Dirección Física: %u - Tamaño: %u", jid,
           dir_fisica, tamanio);
}

void escribir_memoria_job(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                          void* origen) {
  pthread_mutex_lock(&mutex_fd_placa);
  placa_escribir(fd_placa, jid, dir_fisica, tamanio, origen);
  pthread_mutex_unlock(&mutex_fd_placa);
  log_info(logger, "## (%u) - Escritura - Dirección Física: %u - Tamaño: %u",
           jid, dir_fisica, tamanio);
}
