#ifndef UTILS_SRC_UTILS_CONEXION_H_
#define UTILS_SRC_UTILS_CONEXION_H_

int crear_conexion(char* ip, char* puerto);  // cliente: socket + connect
int iniciar_servidor(
    char* puerto);  // servidor: socket + bind + listen, devuelve fd de escucha
int esperar_cliente(
    int fd_servidor);          // accept(), devuelve fd de la conexión nueva
void cerrar_conexion(int fd);  // close()

#endif  // UTILS_SRC_UTILS_CONEXION_H_
