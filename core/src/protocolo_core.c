#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "protocolo_core.h"

//Recibe el JID y el contexto enviados por el Planificador
void recibir_asignacion_job(int fd_planificador, uint32_t* jid, t_contexto* contexto) {
    int size;
    recibir_operacion(fd_planificador);
    void* buffer = recibir_buffer(&size, fd_planificador);

    t_list* campos = deserializar_paquete(buffer, size);

    *jid = *(uint32_t*) list_get(campos, 0);
    t_contexto* ctx_recibido = (t_contexto*) list_get(campos, 1);
    memcpy(contexto, ctx_recibido, sizeof(t_contexto));

    list_destroy_and_destroy_elements(campos, free);
    free(buffer);
}

//Solicita a la Placa la instrucción asociada al PC actual
void enviar_fetch(int fd_placa, uint32_t jid, uint32_t pc) {
    t_paquete* paquete = crear_paquete(FETCH_INSTRUCCION);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, &pc, sizeof(uint32_t));
    enviar_paquete(paquete, fd_placa);
    eliminar_paquete(paquete);
}

//Recibe el string de la instrucción desde la Placa
char* recibir_instruccion(int fd_placa) {
    recibir_operacion(fd_placa);
    int size;
    return (char*) recibir_buffer(&size, fd_placa);
}

//Consulta al Planificador si hubo una interrupción por fin de quantum
void enviar_consultar_interrupcion(int fd_planificador, uint32_t jid) {
    t_paquete* paquete = crear_paquete(CONSULTAR_INTERRUPCION);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    enviar_paquete(paquete, fd_planificador);
    eliminar_paquete(paquete);
}

//Recibe la respuesta del Planificador (0 = seguir, 1 = desalojar
uint8_t recibir_respuesta_interrupcion(int fd_planificador) {
    recibir_operacion(fd_planificador);
    int size;
    uint8_t* respuesta = (uint8_t*) recibir_buffer(&size, fd_planificador);
    uint8_t desalojar = *respuesta;
    free(respuesta);
    return desalojar;
}

//Devuelve el Job al Planificador (motivo 0 = FIN_QUANTUM, 1 = FIN_EJECUCION)
void enviar_devolver_job(int fd_planificador, uint32_t jid, t_contexto* contexto, uint8_t motivo) {
    t_paquete* paquete = crear_paquete(DEVOLVER_JOB);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, contexto, sizeof(t_contexto));
    agregar_a_paquete(paquete, &motivo, sizeof(uint8_t));
    enviar_paquete(paquete, fd_planificador);
    eliminar_paquete(paquete);
}

//Notifica la syscall INIT_JOB al Planificador con el archivo a cargar
void enviar_solicitud_init_job(int fd_planificador, uint32_t jid, char* archivo) {
    t_paquete* paquete = crear_paquete(SOLICITUD_INIT_JOB);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, archivo, strlen(archivo) + 1); // +1 para incluir el null terminator
    enviar_paquete(paquete, fd_planificador);
    eliminar_paquete(paquete);
}

//Espera la confirmación (ACK) del Planificador tras la syscall
void recibir_ack(int fd_planificador) {
    recibir_operacion(fd_planificador);
}