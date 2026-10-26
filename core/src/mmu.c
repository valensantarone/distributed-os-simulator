#include "mmu.h"

#include <commons/log.h>
#include <stdlib.h>
#include <utils/paquete.h>

#include "protocolo_core.h"

extern t_log* logger;

static int fd_placa;
static uint32_t tam_pagina;
static bool hay_paginas_lockeadas = false;

bool mmu_iniciar(int fd) {
  fd_placa = fd;
  tam_pagina = pedir_tam_pagina(fd_placa);
  return tam_pagina > 0;
}

t_resultado_mmu mmu_traducir(uint32_t jid, uint32_t dir_logica, uint32_t tamanio, t_list* pares, uint32_t* pagina_faltante) {
    uint32_t dir = dir_logica;
  uint32_t restante = tamanio;
  while (restante > 0) {
    uint32_t nro_pagina = dir / tam_pagina;
    uint32_t desplazamiento = dir % tam_pagina;
    uint32_t tam_pedazo = tam_pagina - desplazamiento;  //hasta fin de página
    if (tam_pedazo > restante) tam_pedazo = restante;

    int64_t marco = pedir_marco(fd_placa, jid, nro_pagina);
    if (marco == -2) return MMU_SIN_CONEXION;
    if (marco == -1) {
      log_info(logger, "## JID: %u - Page Fault - Página: %u", jid, nro_pagina);
      *pagina_faltante = nro_pagina;
      return MMU_PAGE_FAULT;
    }
    hay_paginas_lockeadas = true;

    t_par_fisico* par = malloc(sizeof(t_par_fisico));
    par->dir_fisica = marco * tam_pagina + desplazamiento;
    par->tamanio = tam_pedazo;
    list_add(pares, par);

    dir += tam_pedazo;
    restante -= tam_pedazo;
  }
  return MMU_OK;
}
bool mmu_leer(uint32_t jid, t_list* pares, void* destino) {
  uint8_t* cursor = destino;
  for (int i = 0; i < list_size(pares); i++) {
    t_par_fisico* par = list_get(pares, i);
    if (!placa_leer(fd_placa, jid, par->dir_fisica, par->tamanio, cursor))
      return false;
    log_info(
        logger, 
        "## JID: %u - Acción: Lectura - Dirección Física: %u - Tamaño: %u", 
        jid, par->dir_fisica, par->tamanio);
    cursor += par->tamanio;
  }
  return true;
}

bool mmu_escribir(uint32_t jid, t_list* pares, void* origen) {
  uint8_t* cursor = origen;
  for (int i = 0; i < list_size(pares); i++) {
    t_par_fisico* par = list_get(pares, i);
    if (!placa_escribir(fd_placa, jid, par->dir_fisica, par->tamanio, cursor))
      return false;
    log_info(
        logger,
        "## JID: %u - Acción: Escritura - Dirección Física: %u - Tamaño: %u",
        jid, par->dir_fisica, par->tamanio);
    cursor += par->tamanio;
  }
  return true;
}

bool mmu_deslockear(uint32_t jid) {
  if (!hay_paginas_lockeadas) return true;  // no se pidió ningún marco
  hay_paginas_lockeadas = false;
  return placa_deslockear(fd_placa, jid);
}

void mmu_olvidar_lockeos(void) { hay_paginas_lockeadas = false; }