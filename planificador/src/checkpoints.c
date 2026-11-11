#include "checkpoints.h"

#include <commons/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "placa_cliente.h"
#include "planificador.h"
#include "storage_cliente.h"

// El rango del pedido, pedazo por pedazo (un par por página)
static void leer_rango(t_pedido_servicio* pedido, uint8_t* destino) {
  for (uint32_t i = 0; i < pedido->cant_pares; i++) {
    leer_memoria_job(pedido->jid, pedido->pares[i].dir_fisica,
                     pedido->pares[i].tamanio, destino);
    destino += pedido->pares[i].tamanio;
  }
}

static void escribir_rango(t_pedido_servicio* pedido, uint8_t* origen) {
  for (uint32_t i = 0; i < pedido->cant_pares; i++) {
    escribir_memoria_job(pedido->jid, pedido->pares[i].dir_fisica,
                         pedido->pares[i].tamanio, origen);
    origen += pedido->pares[i].tamanio;
  }
}

// SAVE_CHECKPOINT: lee el rango de la memoria del Job y lo manda al Storage
static t_resultado_checkpoint guardar(t_pedido_servicio* pedido) {
  uint8_t* contenido = malloc(pedido->tamanio + 1);
  leer_rango(pedido, contenido);
  t_resultado_checkpoint resultado =
      guardar_checkpoint_en_storage(pedido->nombre, contenido, pedido->tamanio);
  free(contenido);
  return resultado;
}

// LOAD_CHECKPOINT: pide el contenido al Storage y lo escribe en el rango. Si
// el Checkpoint es más corto que el rango, el resto queda en 0 (como el
// Loader con un archivo corto); si es más largo, se escribe lo que entra.
static t_resultado_checkpoint cargar(t_pedido_servicio* pedido) {
  void* guardado = NULL;
  uint32_t tamanio_guardado = 0;
  t_resultado_checkpoint resultado = cargar_checkpoint_de_storage(
      pedido->nombre, &guardado, &tamanio_guardado);
  if (resultado == CHECKPOINT_OK) {
    uint8_t* contenido = calloc(1, pedido->tamanio + 1);
    memcpy(contenido, guardado,
           tamanio_guardado < pedido->tamanio ? tamanio_guardado
                                              : pedido->tamanio);
    escribir_rango(pedido, contenido);
    free(contenido);
  }
  free(guardado);
  return resultado;
}

// DELETE_CHECKPOINT: si no existe se avisa, pero el Job sigue (consigna)
static t_resultado_checkpoint eliminar(t_pedido_servicio* pedido) {
  t_resultado_checkpoint resultado =
      eliminar_checkpoint_en_storage(pedido->nombre);
  if (resultado == CHECKPOINT_NO_EXISTE) {
    log_warning(logger, "## (%u) - DELETE_CHECKPOINT: no existe %s",
                pedido->jid, pedido->nombre);
    return CHECKPOINT_OK;
  }
  return resultado;
}

// Con qué motivo se finaliza el Job cuando el Storage no pudo
static char* motivo_de_error(t_resultado_checkpoint resultado) {
  switch (resultado) {
    case CHECKPOINT_SIN_ESPACIO:
    case CHECKPOINT_DIRECTORIO_LLENO:
      return "OUT_OF_STORAGE";
    case CHECKPOINT_NO_EXISTE:
      return "CHECKPOINT_NOT_FOUND";
    default:
      return "STORAGE_DISCONNECTED";  // el Storage no responde
  }
}

static void* operar(void* arg) {
  t_pedido_servicio* pedido = arg;
  t_resultado_checkpoint resultado;
  if (pedido->tipo == SYS_SAVE_CHECKPOINT)
    resultado = guardar(pedido);
  else if (pedido->tipo == SYS_LOAD_CHECKPOINT)
    resultado = cargar(pedido);
  else
    resultado = eliminar(pedido);

  if (resultado == CHECKPOINT_OK) {
    // Las páginas del rango quedaron lockeadas desde que el Core las tradujo.
    // DELETE no tiene rango: no hay nada que deslockear.
    if (pedido->cant_pares > 0) deslockear_paginas_en_placa(pedido->jid);
    desbloquear_job(pedido->jid);
  } else {
    // La Placa libera todo el Job, también las páginas lockeadas
    finalizar_job_bloqueado(pedido->jid, motivo_de_error(resultado));
  }
  destruir_pedido_servicio(pedido);
  return NULL;
}

// Un hilo por pedido, como con los Page Faults: el hilo del Core queda libre
// mientras se espera al Storage (que puede tardar varios RETARDO_*)
void atender_checkpoint(t_pedido_servicio* pedido) {
  pthread_t hilo;
  pthread_create(&hilo, NULL, operar, pedido);
  pthread_detach(hilo);
}