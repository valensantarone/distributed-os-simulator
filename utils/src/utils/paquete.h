#ifndef UTILS_SRC_UTILS_PAQUETE_H_
#define UTILS_SRC_UTILS_PAQUETE_H_
#include <commons/collections/list.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  HANDSHAKE = 0,
  MENSAJE = 1,
  CREAR_JOB = 2,  // Planificador -> Placa: JID + nombre de archivo
  ACK = 3,  // respuesta genérica sin contenido (confirma que se procesó pedido)
  FETCH_INSTRUCCION = 4,       // Core -> Placa: JID + PC
  INSTRUCCION = 5,             // Placa -> Core: string con la instrucción
  ASIGNAR_JOB = 6,             // Planificador -> Core: JID + contexto
  CONSULTAR_INTERRUPCION = 7,  // Core -> Planificador: JID
  RESPUESTA_INTERRUPCION = 8,             // Planificador -> Core: 1 byte (0=SEGUIR, 1=DESALOJAR)
  DEVOLVER_JOB = 9,  // Core -> Planificador: JID + contexto + motivo
  SOLICITUD_INIT_JOB =10, // Core -> Planificador: JID del que pide + nombre del archivo nuevo
  PEDIR_TAM_PAGINA = 11,      // Core -> Placa: sin contenido
  RESPUESTA_TAM_PAGINA = 12,  // Placa -> Core: uint32 tam_pagina
  OBTENER_MARCO = 13,         // Core -> Placa: jid + nro_pagina
  MARCO = 14,                 // Placa -> Core: int32 marco (-1 = Page Fault)
  LEER_MEMORIA = 15,          // Core/Planif. -> Placa: jid + dir_fisica + tam
  DATOS_MEMORIA = 16,       // Placa -> Core/Planif.: los bytes leídos, crudos
  ESCRIBIR_MEMORIA = 17,    // Core/Planif. -> Placa: jid + dir + tam + datos
  DESLOCKEAR_PAGINAS = 18,  // Core/Planif. -> Placa: jid. Responde ACK
  ASIGNAR_ESPACIO = 19,     // Planif. -> Placa: jid + tamaño
  LIBERAR_ESPACIO = 20,     // Planif. -> Placa: jid + dir_logica. Resp. ACK
  CARGAR_PAGINA = 21,       // Planif. -> Placa: jid + nro_pagina. Resp. ACK
  FINALIZAR_JOB = 22,       // Planif. -> Placa: jid. Responde ACK
  SOLICITUD_ALLOC = 23,    // Core -> Planif.: jid + tamaño
  RESPUESTA_ALLOC = 24,    // Planif. -> Core y Placa -> Planif.: ok + dir
  SOLICITUD_FREE = 25,     // Core -> Planif.: jid + dir_logica. Resp. ACK
  SYSCALL_BLOQUEANTE = 26  // Core -> Planif.: jid + contexto + parámetros
} op_code;

typedef enum {
  FIN_QUANTUM = 0,
  FIN_EJECUCION = 1,
  PAGE_FAULT = 2,  // DEVOLVER_JOB lleva un campo más: el número de página
  FIN_ERROR = 3    // ALLOC sin lugar en el Offload: el Job se finaliza
} t_motivo_devolucion;

typedef enum {
  SYS_LOAD_BATCH = 0,
  SYS_LABEL = 1,
  SYS_REPORT = 2,
  SYS_SAVE_CHECKPOINT = 3,   // Check 4
  SYS_LOAD_CHECKPOINT = 4,   // Check 4
  SYS_DELETE_CHECKPOINT = 5  // Check 4
} t_syscall_bloqueante;

typedef struct {
  uint32_t dir_fisica;
  uint32_t tamanio;
} t_par_fisico;


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
bool recibir_ack(
    int fd_conexion);  // false si no llegó un ACK o se cortó la conexión

    
void enviar_respuesta_interrupcion(int fd_conexion,
                                   uint8_t desalojar);  // 0 o 1


void enviar_instruccion(int fd_conexion, char* instruccion);


bool placa_leer(int fd_placa, uint32_t jid, uint32_t dir_fisica,
                uint32_t tamanio, void* destino);


bool placa_escribir(int fd_placa, uint32_t jid, uint32_t dir_fisica,
                    uint32_t tamanio, void* origen);


bool placa_deslockear(int fd_placa, uint32_t jid);
#endif  // UTILS_SRC_UTILS_PAQUETE_H_
