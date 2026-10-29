#ifndef PLANIFICADOR_SRC_PLACA_CLIENTE_H_
#define PLANIFICADOR_SRC_PLACA_CLIENTE_H_

#include <stdbool.h>
#include <stdint.h>

// Todos los pedidos del Planificador a la Placa. Cada uno toma
// mutex_fd_placa durante el intercambio completo (mandar + respuesta),
// porque el socket lo comparten todos los hilos. Nunca llamarlas con
// mutex_jobs tomado.

void placa_iniciar(int fd_placa);

void crear_job_en_placa(uint32_t jid, char* archivo);
void finalizar_job_en_placa(uint32_t jid);
bool asignar_espacio_en_placa(uint32_t jid, uint32_t tamanio,
                              uint32_t* dir_logica);
void liberar_espacio_en_placa(uint32_t jid, uint32_t dir_logica);
void cargar_pagina_en_placa(uint32_t jid, uint32_t nro_pagina);

// Con log obligatorio del Planificador
void deslockear_paginas_en_placa(uint32_t jid);
void leer_memoria_job(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                      void* destino);
void escribir_memoria_job(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                          void* origen);

#endif  // PLANIFICADOR_SRC_PLACA_CLIENTE_H_
