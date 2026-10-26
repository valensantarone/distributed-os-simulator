#ifndef PROTOCOLO_CORE_H_
#define PROTOCOLO_CORE_H_

#include <stdint.h>
#include <stdbool.h>
#include "utils/paquete.h"

//Asignación y Fetch
bool recibir_asignacion_job(int fd_planificador, uint32_t* jid, t_contexto* contexto);
void enviar_fetch(int fd_placa, uint32_t jid, uint32_t pc);
char* recibir_instruccion(int fd_placa);

//Interrupciones y Devolución
void enviar_consultar_interrupcion(int fd_planificador, uint32_t jid);
int recibir_respuesta_interrupcion(int fd_planificador);
void enviar_devolver_job(int fd_planificador, uint32_t jid, t_contexto* contexto, uint8_t motivo);

//Syscalls
void enviar_solicitud_init_job(int fd_planificador, uint32_t jid, char* archivo);

//MMU (Core <-> Placa)
uint32_t pedir_tam_pagina(int fd_placa); //0 si se cortó la conexión
int64_t pedir_marco(int fd_placa, uint32_t jid, uint32_t nro_pagina); //-1 = Page Fault, -2 = sin conexión

//Page Fault y syscalls de memoria (Core <-> Planificador)
void enviar_page_fault(int fd_planificador, uint32_t jid, t_contexto* contexto, uint32_t nro_pagina);
void enviar_solicitud_alloc(int fd_planificador, uint32_t jid, uint32_t tamanio);
int recibir_respuesta_alloc(int fd_planificador, uint32_t* dir_logica);  // 1 ok, 0 sin espacio, -1 sin conexión
void enviar_solicitud_free(int fd_planificador, uint32_t jid, uint32_t dir_logica);
void enviar_syscall_bloqueante(int fd_planificador, uint32_t jid, t_contexto* contexto, uint8_t tipo, char* nombre, uint32_t tamanio, t_list* pares);

#endif