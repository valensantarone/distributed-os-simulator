#ifndef UTILS_SRC_UTILS_PAQUETE_H_
#define UTILS_SRC_UTILS_PAQUETE_H_
#include <stdint.h>

typedef enum {
  HANDSHAKE = 0,
  MENSAJE = 1
  // próximos checks: INIT_JOB, EXIT, LOAD_BATCH, etc.
} op_code;

typedef struct {
  uint32_t size;
  void* stream;
} t_buffer;

typedef struct {
  uint8_t codigo_operacion;
  t_buffer* buffer;
} t_paquete;

// Genéricas (para paquetes con varios valores, checks futuros)
t_paquete* crear_paquete(op_code codigo);
void agregar_a_paquete(t_paquete* paquete, void* valor, int tamanio);
void enviar_paquete(t_paquete* paquete, int fd_conexion);
void eliminar_paquete(t_paquete* paquete);

// Recepción de bajo nivel
int recibir_operacion(
    int fd_conexion);  // recv del op_code; devuelve -1 si se desconectó
void* recibir_buffer(int* size,
                     int fd_conexion);  // recv del size + payload completo

// Handshake (un solo string, alcanza para el Check 1)
void enviar_handshake(int fd_conexion, char* nombre_modulo);
char* recibir_handshake(
    int fd_conexion);  // devuelve string (malloc'd, ya null-terminated)

// Paquete de prueba, aparte del Handshake (la consigna los pide como dos pasos
// separados, ver [[09-Protocolo-Comunicacion]])
void enviar_mensaje(int fd_conexion, char* mensaje);
char* recibir_mensaje(
    int fd_conexion);  // mismo mecanismo que el Handshake, opcode distinto

#endif  // UTILS_SRC_UTILS_PAQUETE_H_
