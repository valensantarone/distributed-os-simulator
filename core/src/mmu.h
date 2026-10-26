#ifndef CORE_SRC_MMU_H_
#define CORE_SRC_MMU_H_

#include <commons/collections/list.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MMU_OK,
    MMU_PAGE_FAULT,
    MMU_SIN_CONEXION
} t_resultado_mmu;

//Pide el tamaño de página a la Placa. Una vez, después de conectarse.
bool mmu_iniciar(int fd_placa);

// Traduce [dir_logica, dir_logica + tamanio): agrega a `pares` un
// t_par_fisico* por cada página que toca (la Placa las deja lockeadas).
// Si una no está cargada devuelve MMU_PAGE_FAULT con su número en *pagina_faltante.
t_resultado_mmu mmu_traducir(uint32_t jid, uint32_t dir_logica, uint32_t tamanio, t_list* pares, uint32_t* pagina_faltante);

//Leen/escriben un rango ya traducido, pedazo por pedazo
bool mmu_leer(uint32_t jid, t_list* pares, void* destino);
bool mmu_escribir(uint32_t jid, t_list* pares, void* origen);

//Deslockea las páginas del Job si en este ciclo se pidió algún marco
bool mmu_deslockear(uint32_t jid);

//Para las syscalls de memoria: las páginas las deslockea el Planificador
void mmu_olvidar_lockeos(void);

#endif  //CORE_SRC_MMU_H_