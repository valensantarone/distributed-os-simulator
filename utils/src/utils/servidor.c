// servidor.c
#include "servidor.h"

#include <pthread.h>
#include <stdlib.h>

#include "conexion.h"

typedef struct {
  int fd_conexion;
  atender_conexion_t atender;
} args_hilo;

static void* hilo_conexion(void* args_void) {
  args_hilo* args = (args_hilo*)args_void;
  args->atender(args->fd_conexion);
  free(args);
  return NULL;
}

void escuchar_conexiones(int fd_servidor, atender_conexion_t atender) {
  while (1) {
    int fd_conexion = esperar_cliente(fd_servidor);

    args_hilo* args =
        malloc(sizeof(args_hilo));  // IMPORTANTE: mallocado, no una variable de
                                    // stack (ver nota abajo)
    args->fd_conexion = fd_conexion;
    args->atender = atender;

    pthread_t hilo;
    pthread_create(&hilo, NULL, hilo_conexion, args);
    pthread_detach(hilo);  // no hacemos join: el hilo muere solo cuando el
                           // cliente se desconecta
  }
}
