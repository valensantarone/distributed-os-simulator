#include "storage_cliente.h"

#include <commons/collections/list.h>
#include <commons/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <utils/conexion.h>

#include "planificador.h"

static char* ip_storage;
static char* puerto_storage;
static int fd_storage = -1;  // -1: no hay conexión
static pthread_mutex_t mutex_fd_storage = PTHREAD_MUTEX_INITIALIZER;

static void conectar(void) {
  fd_storage = crear_conexion(ip_storage, puerto_storage);
  if (fd_storage >= 0) enviar_handshake(fd_storage, "PLANIFICADOR");
}

void storage_iniciar(char* ip, char* puerto) {
  ip_storage = strdup(ip);
  puerto_storage = strdup(puerto);
  conectar();
  if (fd_storage < 0)
    log_warning(logger,
                "No se pudo conectar con el Storage: se reintenta en el primer "
                "Checkpoint");
}

// Un intercambio: manda el pedido y lee RESPUESTA_CHECKPOINT (resultado,
// tamaño y contenido). CHECKPOINT_SIN_CONEXION si se cortó.
static t_resultado_checkpoint intercambiar(t_paquete* paquete, void** contenido,
                                           uint32_t* tamanio) {
  enviar_paquete(paquete, fd_storage);
  if (recibir_operacion(fd_storage) != RESPUESTA_CHECKPOINT)
    return CHECKPOINT_SIN_CONEXION;
  int size;
  void* buffer = recibir_buffer(&size, fd_storage);
  if (buffer == NULL) return CHECKPOINT_SIN_CONEXION;
  t_list* campos = deserializar_paquete(buffer, size);
  free(buffer);
  t_resultado_checkpoint resultado = CHECKPOINT_SIN_CONEXION;
  if (list_size(campos) == 3) {
    resultado = *(uint8_t*)list_get(campos, 0);
    if (contenido != NULL && resultado == CHECKPOINT_OK) {
      *tamanio = *(uint32_t*)list_get(campos, 1);
      *contenido = list_remove(campos, 2);  // ya no lo libera la lista
    }
  }
  list_destroy_and_destroy_elements(campos, free);
  return resultado;
}

// El pedido completo, con el socket tomado. Si la conexión se cortó (el
// Storage se cayó, o se reinició y esta conexión quedó vieja), se reconecta y
// se intenta una vez más: repetir cualquiera de los tres pedidos deja el
// volumen igual.
static t_resultado_checkpoint pedir(t_paquete* paquete, void** contenido,
                                    uint32_t* tamanio) {
  t_resultado_checkpoint resultado = CHECKPOINT_SIN_CONEXION;
  pthread_mutex_lock(&mutex_fd_storage);
  for (int intento = 0; intento < 2 && resultado == CHECKPOINT_SIN_CONEXION;
       intento++) {
    if (fd_storage < 0) conectar();
    if (fd_storage < 0) break;  // el Storage no está levantado
    resultado = intercambiar(paquete, contenido, tamanio);
    if (resultado == CHECKPOINT_SIN_CONEXION) {
      cerrar_conexion(fd_storage);
      fd_storage = -1;
    }
  }
  pthread_mutex_unlock(&mutex_fd_storage);
  eliminar_paquete(paquete);
  return resultado;
}

t_resultado_checkpoint guardar_checkpoint_en_storage(char* nombre,
                                                     void* contenido,
                                                     uint32_t tamanio) {
  t_paquete* paquete = crear_paquete(GUARDAR_CHECKPOINT);
  agregar_a_paquete(paquete, nombre, strlen(nombre) + 1);
  agregar_a_paquete(paquete, &tamanio, sizeof(uint32_t));
  agregar_a_paquete(paquete, contenido, tamanio);
  return pedir(paquete, NULL, NULL);
}

t_resultado_checkpoint cargar_checkpoint_de_storage(char* nombre,
                                                    void** contenido,
                                                    uint32_t* tamanio) {
  t_paquete* paquete = crear_paquete(CARGAR_CHECKPOINT);
  agregar_a_paquete(paquete, nombre, strlen(nombre) + 1);
  return pedir(paquete, contenido, tamanio);
}

t_resultado_checkpoint eliminar_checkpoint_en_storage(char* nombre) {
  t_paquete* paquete = crear_paquete(ELIMINAR_CHECKPOINT);
  agregar_a_paquete(paquete, nombre, strlen(nombre) + 1);
  return pedir(paquete, NULL, NULL);
}
