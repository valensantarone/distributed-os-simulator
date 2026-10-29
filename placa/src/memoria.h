#ifndef PLACA_SRC_MEMORIA_H_
#define PLACA_SRC_MEMORIA_H_

#include <commons/config.h>
#include <stdbool.h>
#include <stdint.h>

// Memoria de usuario de la Placa (Check 3): paginación bajo demanda, Offload
// y reemplazo. Todas las funciones son thread-safe (usan mutex_memoria).

void memoria_iniciar(t_config* config);
uint32_t memoria_tam_pagina(void);

// Estructuras del Job: tabla de páginas vacía (arranca sin marcos)
void memoria_crear_job(uint32_t jid);
// Libera marcos, lugar en el Offload y la tabla de páginas
void memoria_finalizar_job(uint32_t jid);

// ALLOC: false si no hay lugar en el Offload
bool memoria_asignar(uint32_t jid, uint32_t tamanio, uint32_t* dir_logica);
// FREE: dir_logica es la que devolvió memoria_asignar
void memoria_liberar(uint32_t jid, uint32_t dir_logica);

// MMU: marco de la página (y la deja lockeada) o -1 si no está (Page Fault)
int32_t memoria_obtener_marco(uint32_t jid, uint32_t nro_pagina);
// Después de un Page Fault: trae la página a un marco (reemplaza si hace falta)
void memoria_cargar_pagina(uint32_t jid, uint32_t nro_pagina);

// Lectura/escritura en una dirección física. El rango no cruza de página.
bool memoria_leer(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                  void* destino);
bool memoria_escribir(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                      void* origen);

// Deslockea todas las páginas del Job
void memoria_deslockear(uint32_t jid);

// Comandos de consola
void memoria_info(void);
void memoria_tls(void);

#endif  // PLACA_SRC_MEMORIA_H_