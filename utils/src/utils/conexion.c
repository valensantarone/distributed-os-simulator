// conexion.c
#include "conexion.h"

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int crear_conexion(char* ip, char* puerto) {
  struct addrinfo hints, *server_info;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  getaddrinfo(ip, puerto, &hints, &server_info);
  int fd = socket(server_info->ai_family, server_info->ai_socktype,
                  server_info->ai_protocol);
  connect(fd, server_info->ai_addr, server_info->ai_addrlen);
  freeaddrinfo(server_info);
  return fd;
}

int iniciar_servidor(char* puerto) {
  struct addrinfo hints, *server_info;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  getaddrinfo(NULL, puerto, &hints, &server_info);
  int fd = socket(server_info->ai_family, server_info->ai_socktype,
                  server_info->ai_protocol);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &(int){1},
             sizeof(int));  // reusar puerto al reiniciar en dev
  bind(fd, server_info->ai_addr, server_info->ai_addrlen);
  listen(fd, SOMAXCONN);
  freeaddrinfo(server_info);
  return fd;
}

int esperar_cliente(int fd_servidor) { return accept(fd_servidor, NULL, NULL); }

void cerrar_conexion(int fd) { close(fd); }
