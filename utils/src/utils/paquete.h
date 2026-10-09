#ifndef UTILS_SRC_UTILS_PAQUETE_H_
#define UTILS_SRC_UTILS_PAQUETE_H_
#include <stdint.h>
#include <stdbool.h>
#include <commons/collections/list.h>

typedef enum {
  HANDSHAKE = 0,
  MENSAJE = 1,
  CREAR_JOB = 2,              // Planificador -> Placa: JID + nombre de archivo
  ACK = 3,                    // respuesta genérica sin contenido (confirma que se procesó el pedido)
  FETCH_INSTRUCCION = 4,      // Core -> Placa: JID + PC
  INSTRUCCION = 5,            // Placa -> Core: string con la instrucción
  ASIGNAR_JOB = 6,            // Planificador -> Core: JID + contexto
  CONSULTAR_INTERRUPCION = 7, // Core -> Planificador: JID
  RESPUESTA_INTERRUPCION = 8, // Planificador -> Core: 1 byte (0=SEGUIR, 1=DESALOJAR)
  DEVOLVER_JOB = 9,           // Core -> Planificador: JID + contexto + motivo
  SOLICITUD_INIT_JOB = 10     // Core -> Planificador: JID del que pide + nombre del archivo nuevo
} op_code;

typedef struct {
  uint32_t size;
  void* stream;
} t_buffer;

typedef struct {
  uint8_t codigo_operacion;
  t_buffer* buffer;
} t_paquete;

typedef struct {
    uint32_t pc;
    uint32_t p0, p1, p2, p3, p4, p5;
    uint32_t ax, bx, cx, dx;
    uint32_t e1, e2, e3;
} t_contexto;



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


    t_list* deserializar_paquete(void* buffer, int size);


void enviar_ack(int fd_conexion);
bool recibir_ack(int fd_conexion);  // false si no llegó un ACK o se cortó la conexión
void enviar_respuesta_interrupcion(int fd_conexion, uint8_t desalojar);  // 0 o 1
void enviar_instruccion(int fd_conexion, char* instruccion);
void enviar_ack(int fd_conexion);          // ACK: paquete sin buffer


#endif  // UTILS_SRC_UTILS_PAQUETE_H_
