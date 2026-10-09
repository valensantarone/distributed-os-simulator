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

#endif