#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "protocolo_core.h"

//Recibe el JID y el contexto enviados por el Planificador
bool recibir_asignacion_job(int fd_planificador, uint32_t* jid, t_contexto* contexto) {
    int size;
    if (recibir_operacion(fd_planificador) != ASIGNAR_JOB) return false;
    void* buffer = recibir_buffer(&size, fd_planificador);
    if (buffer == NULL) return false;

    t_list* campos = deserializar_paquete(buffer, size);

    *jid = *(uint32_t*) list_get(campos, 0);
    t_contexto* ctx_recibido = (t_contexto*) list_get(campos, 1);
    memcpy(contexto, ctx_recibido, sizeof(t_contexto));

    list_destroy_and_destroy_elements(campos, free);
    free(buffer);
    return true;
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
    if (recibir_operacion(fd_placa) != INSTRUCCION) return NULL;
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
int recibir_respuesta_interrupcion(int fd_planificador) {
    if (recibir_operacion(fd_planificador) != RESPUESTA_INTERRUPCION) return -1;
    int size;
    uint8_t* respuesta = (uint8_t*) recibir_buffer(&size, fd_planificador);
    if (respuesta == NULL) return -1;
    int desalojar = *respuesta;
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

//Lee los campos de una respuesta armada con agregar_a_paquete (NULL si se cortó)
static t_list* recibir_campos(int fd, op_code esperado, int cant_campos) {
    if (recibir_operacion(fd) != esperado) return NULL;
    int size;
    void* buffer = recibir_buffer(&size, fd);
    if (buffer == NULL) return NULL;
    t_list* campos = deserializar_paquete(buffer, size);
    free(buffer);
    if (list_size(campos) < cant_campos) {
        list_destroy_and_destroy_elements(campos, free);
        return NULL;
    }
    return campos;
}

uint32_t pedir_tam_pagina(int fd_placa) {
    t_paquete* paquete = crear_paquete(PEDIR_TAM_PAGINA);
    enviar_paquete(paquete, fd_placa);
    eliminar_paquete(paquete);

    t_list* campos = recibir_campos(fd_placa, RESPUESTA_TAM_PAGINA, 1);
    if (campos == NULL) return 0;
    uint32_t tam_pagina = *(uint32_t*) list_get(campos, 0);
    list_destroy_and_destroy_elements(campos, free);
    return tam_pagina;
}

//Pide el marco de una página. Si está, la Placa la deja lockeada
int64_t pedir_marco(int fd_placa, uint32_t jid, uint32_t nro_pagina) {
    t_paquete* paquete = crear_paquete(OBTENER_MARCO);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, &nro_pagina, sizeof(uint32_t));
    enviar_paquete(paquete, fd_placa);
    eliminar_paquete(paquete);

    t_list* campos = recibir_campos(fd_placa, MARCO, 1);
    if (campos == NULL) return -2;
    int32_t marco = *(int32_t*) list_get(campos, 0);
    list_destroy_and_destroy_elements(campos, free);
    return marco;  // -1 = Page Fault
}

//Devuelve el Job por Page Fault: mismo DEVOLVER_JOB, con la página como cuarto campo
void enviar_page_fault(int fd_planificador, uint32_t jid, t_contexto* contexto, uint32_t nro_pagina) {
    uint8_t motivo = PAGE_FAULT;
    t_paquete* paquete = crear_paquete(DEVOLVER_JOB);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, contexto, sizeof(t_contexto));
    agregar_a_paquete(paquete, &motivo, sizeof(uint8_t));
    agregar_a_paquete(paquete, &nro_pagina, sizeof(uint32_t));
    enviar_paquete(paquete, fd_planificador);
    eliminar_paquete(paquete);
}

void enviar_solicitud_alloc(int fd_planificador, uint32_t jid, uint32_t tamanio) {
    t_paquete* paquete = crear_paquete(SOLICITUD_ALLOC);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, &tamanio, sizeof(uint32_t));
    enviar_paquete(paquete, fd_planificador);
    eliminar_paquete(paquete);
}

int recibir_respuesta_alloc(int fd_planificador, uint32_t* dir_logica) {
    t_list* campos = recibir_campos(fd_planificador, RESPUESTA_ALLOC, 2);
    if (campos == NULL) return -1;
    int ok = *(uint8_t*) list_get(campos, 0);
    *dir_logica = *(uint32_t*) list_get(campos, 1);
    list_destroy_and_destroy_elements(campos, free);
    return ok;
}

void enviar_solicitud_free(int fd_planificador, uint32_t jid, uint32_t dir_logica) {
    t_paquete* paquete = crear_paquete(SOLICITUD_FREE);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, &dir_logica, sizeof(uint32_t));
    enviar_paquete(paquete, fd_planificador);
    eliminar_paquete(paquete);
}

//Syscall bloqueante: libera el Core. Lleva el contexto (con el PC ya avanzado)
//y el rango ya traducido: un t_par_fisico por cada página
void enviar_syscall_bloqueante(int fd_planificador, uint32_t jid, t_contexto* contexto, uint8_t tipo, char* nombre, uint32_t tamanio, t_list* pares) {
    uint32_t cant_pares = list_size(pares);
    t_par_fisico* arreglo = malloc(sizeof(t_par_fisico) * (cant_pares + 1));
    for (uint32_t i = 0; i < cant_pares; i++) {
        arreglo[i] = *(t_par_fisico*) list_get(pares, i);
    }

    t_paquete* paquete = crear_paquete(SYSCALL_BLOQUEANTE);
    agregar_a_paquete(paquete, &jid, sizeof(uint32_t));
    agregar_a_paquete(paquete, contexto, sizeof(t_contexto));
    agregar_a_paquete(paquete, &tipo, sizeof(uint8_t));
    agregar_a_paquete(paquete, nombre, strlen(nombre) + 1);
    agregar_a_paquete(paquete, &tamanio, sizeof(uint32_t));
    agregar_a_paquete(paquete, &cant_pares, sizeof(uint32_t));
    agregar_a_paquete(paquete, arreglo, sizeof(t_par_fisico) * cant_pares);
    enviar_paquete(paquete, fd_planificador);
    eliminar_paquete(paquete);
    free(arreglo);
}