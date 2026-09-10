#ifndef UTILS_SRC_UTILS_SERVIDOR_H_
#define UTILS_SRC_UTILS_SERVIDOR_H_

typedef void (*atender_conexion_t)(int fd_conexion);

// Loop infinito: accept() + pthread por cada cliente nuevo. No vuelve.
void escuchar_conexiones(int fd_servidor, atender_conexion_t atender);

#endif  // UTILS_SRC_UTILS_SERVIDOR_H_
