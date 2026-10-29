#define _POSIX_C_SOURCE 200809L
#include "memoria.h"

#include <commons/bitarray.h>
#include <commons/collections/dictionary.h>
#include <commons/collections/list.h>
#include <commons/log.h>
#include <commons/string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern t_log* logger;

// Una entrada de la tabla de páginas de un Job
typedef struct {
  bool asignada;           // pertenece a un ALLOC vigente
  bool presente;           // está cargada en un marco
  bool lockeada;           // page locking: no puede ser víctima
  bool uso;                // bit de uso (Clock-M)
  bool modificada;         // bit de modificado: hay que bajarla al Offload
  bool en_offload;         // el Offload tiene una copia (si no, se sube en 0)
  uint32_t marco;          // válido solo si presente
  uint32_t slot_offload;   // lugar reservado en el Offload
  uint64_t ultimo_acceso;  // para LRU
} t_pagina;

typedef struct {
  uint32_t dir_logica;
  uint32_t tamanio;
  uint32_t primera_pagina;
  uint32_t cant_paginas;
} t_asignacion;

typedef struct {
  t_list* paginas;       // t_pagina*: el índice en la lista es el nro de página
  t_list* asignaciones;  // t_asignacion*: una por cada ALLOC vigente
} t_tabla_job;

// Qué hay en cada marco: sirve para recorrer víctimas y para saber de qué
// página es una dirección física
typedef struct {
  bool ocupado;
  uint32_t jid;
  uint32_t nro_pagina;
  t_pagina* pagina;
} t_marco;

static void* memoria;  // la memoria de datos: un solo bloque contiguo
static uint32_t tam_memoria;
static uint32_t tam_pagina;
static uint32_t cant_marcos;
static t_marco* marcos;
static t_dictionary* tablas;  // JID (string) -> t_tabla_job*

static FILE* offload;
static uint32_t cant_slots;
static t_bitarray* slots_offload;  // 1 = slot reservado por alguna página
static int retardo_offload;

static bool usar_lru;  // true = LRU, false = CLOCK-M
static uint32_t puntero_clock = 0;
static uint64_t reloj_accesos = 0;  // contador global de accesos para LRU

// mutex_memoria protege todo lo de arriba. El Offload tiene además el suyo:
// nunca dos lecturas/escrituras a la vez. Orden: memoria -> offload.
static pthread_mutex_t mutex_memoria = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mutex_offload = PTHREAD_MUTEX_INITIALIZER;

static void dormir_ms(int ms) {
  struct timespec espera = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&espera, NULL);
}

void memoria_iniciar(t_config* config) {
  tam_memoria = config_get_int_value(config, "TAM_MEMORIA");
  tam_pagina = config_get_int_value(config, "TAM_PAGINA");
  cant_marcos = tam_memoria / tam_pagina;
  memoria = calloc(1, tam_memoria);
  marcos = calloc(cant_marcos, sizeof(t_marco));
  tablas = dictionary_create();
  usar_lru = strcmp(config_get_string_value(config, "ALGORITMO_REEMPLAZO"),
                    "LRU") == 0;

  uint32_t tam_offload = config_get_int_value(config, "TAM_OFFLOAD");
  retardo_offload = config_get_int_value(config, "RETARDO_OFFLOAD");
  cant_slots = tam_offload / tam_pagina;
  int bytes_bitmap = (cant_slots + 7) / 8;
  slots_offload = bitarray_create_with_mode(calloc(1, bytes_bitmap),
                                            bytes_bitmap, LSB_FIRST);

  char* path_offload = config_get_string_value(config, "PATH_OFFLOAD");
  offload = fopen(path_offload, "w+b");  // lo crea, o lo vacía si ya existía
  if (offload == NULL || ftruncate(fileno(offload), tam_offload) != 0) {
    log_error(logger, "No se pudo crear el Offload en %s", path_offload);
    exit(EXIT_FAILURE);
  }
  log_info(logger, "Memoria: %u marcos de %u bytes - Offload: %u páginas - %s",
           cant_marcos, tam_pagina, cant_slots, usar_lru ? "LRU" : "CLOCK-M");
}

uint32_t memoria_tam_pagina(void) { return tam_pagina; }

// ---------- Helpers (llamar con mutex_memoria tomado) ----------

static t_tabla_job* buscar_tabla(uint32_t jid) {
  char* clave = string_itoa(jid);
  t_tabla_job* tabla = dictionary_get(tablas, clave);
  free(clave);
  return tabla;
}

// NULL si la página no existe o no pertenece a un ALLOC vigente
static t_pagina* buscar_pagina(uint32_t jid, uint32_t nro_pagina) {
  t_tabla_job* tabla = buscar_tabla(jid);
  if (tabla == NULL || nro_pagina >= (uint32_t)list_size(tabla->paginas))
    return NULL;
  t_pagina* pagina = list_get(tabla->paginas, nro_pagina);
  return pagina->asignada ? pagina : NULL;
}

static void registrar_acceso(t_pagina* pagina, bool escritura) {
  pagina->uso = true;
  pagina->ultimo_acceso = ++reloj_accesos;
  if (escritura) pagina->modificada = true;
}

static uint32_t slots_libres(void) {
  uint32_t libres = 0;
  for (uint32_t i = 0; i < cant_slots; i++)
    if (!bitarray_test_bit(slots_offload, i)) libres++;
  return libres;
}

static uint32_t reservar_slot(void) {
  uint32_t i = 0;
  while (bitarray_test_bit(slots_offload, i)) i++;
  bitarray_set_bit(slots_offload, i);
  return i;
}

// Saca la página de su marco y le devuelve el lugar en el Offload
static void liberar_pagina(void* elemento) {
  t_pagina* pagina = elemento;
  if (pagina->presente) {
    marcos[pagina->marco].ocupado = false;
    marcos[pagina->marco].pagina = NULL;
  }
  if (pagina->asignada) bitarray_clean_bit(slots_offload, pagina->slot_offload);
  pagina->presente = false;
  pagina->asignada = false;
  pagina->lockeada = false;
}

// ---------- Offload: un solo acceso a la vez ----------

static void escribir_en_offload(uint32_t slot, void* origen) {
  pthread_mutex_lock(&mutex_offload);
  fseek(offload, slot * tam_pagina, SEEK_SET);
  fwrite(origen, tam_pagina, 1, offload);
  fflush(offload);
  dormir_ms(retardo_offload);
  pthread_mutex_unlock(&mutex_offload);
}

static void leer_de_offload(uint32_t slot, void* destino) {
  pthread_mutex_lock(&mutex_offload);
  fseek(offload, slot * tam_pagina, SEEK_SET);
  if (fread(destino, tam_pagina, 1, offload) != 1)
    memset(destino, 0, tam_pagina);
  dormir_ms(retardo_offload);
  pthread_mutex_unlock(&mutex_offload);
}

// ---------- Creación y finalización ----------

void memoria_crear_job(uint32_t jid) {
  t_tabla_job* tabla = malloc(sizeof(t_tabla_job));
  tabla->paginas = list_create();
  tabla->asignaciones = list_create();
  char* clave = string_itoa(jid);
  pthread_mutex_lock(&mutex_memoria);
  dictionary_put(tablas, clave, tabla);
  pthread_mutex_unlock(&mutex_memoria);
  free(clave);
}

void memoria_finalizar_job(uint32_t jid) {
  char* clave = string_itoa(jid);
  pthread_mutex_lock(&mutex_memoria);
  t_tabla_job* tabla = dictionary_remove(tablas, clave);
  if (tabla != NULL) {
    list_iterate(tabla->paginas, liberar_pagina);
    list_destroy_and_destroy_elements(tabla->paginas, free);
    list_destroy_and_destroy_elements(tabla->asignaciones, free);
    free(tabla);
  }
  pthread_mutex_unlock(&mutex_memoria);
  free(clave);
}

// ---------- Asignación y liberación de espacio ----------

bool memoria_asignar(uint32_t jid, uint32_t tamanio, uint32_t* dir_logica) {
  uint32_t cant_paginas = (tamanio + tam_pagina - 1) / tam_pagina;
  pthread_mutex_lock(&mutex_memoria);
  t_tabla_job* tabla = buscar_tabla(jid);
  if (tabla == NULL || slots_libres() < cant_paginas) {
    pthread_mutex_unlock(&mutex_memoria);
    log_warning(logger, "JID: %u - Sin lugar en el Offload para %u bytes", jid,
                tamanio);
    return false;
  }
  // Las páginas nuevas van al final de la tabla: así la asignación arranca
  // al principio de una página. No ocupan marcos todavía (bajo demanda).
  t_asignacion* asignacion = malloc(sizeof(t_asignacion));
  asignacion->primera_pagina = list_size(tabla->paginas);
  asignacion->cant_paginas = cant_paginas;
  asignacion->tamanio = tamanio;
  asignacion->dir_logica = asignacion->primera_pagina * tam_pagina;
  for (uint32_t i = 0; i < cant_paginas; i++) {
    t_pagina* pagina = calloc(1, sizeof(t_pagina));
    pagina->asignada = true;
    pagina->slot_offload = reservar_slot();
    list_add(tabla->paginas, pagina);
  }
  list_add(tabla->asignaciones, asignacion);
  *dir_logica = asignacion->dir_logica;
  log_info(logger,
           "## JID: %u - Asignación - Dirección Lógica: %u - Tamaño: %u", jid,
           *dir_logica, tamanio);
  pthread_mutex_unlock(&mutex_memoria);
  return true;
}

void memoria_liberar(uint32_t jid, uint32_t dir_logica) {
  pthread_mutex_lock(&mutex_memoria);
  t_tabla_job* tabla = buscar_tabla(jid);
  t_asignacion* asignacion = NULL;
  for (int i = 0; tabla != NULL && i < list_size(tabla->asignaciones); i++) {
    t_asignacion* actual = list_get(tabla->asignaciones, i);
    if (actual->dir_logica == dir_logica) {
      asignacion = list_remove(tabla->asignaciones, i);
      break;
    }
  }
  if (asignacion == NULL) {
    pthread_mutex_unlock(&mutex_memoria);
    log_warning(logger, "JID: %u - FREE de una dirección no asignada: %u", jid,
                dir_logica);
    return;
  }
  // Las entradas quedan en la tabla (marcadas libres) para no correr la
  // numeración de las páginas que siguen
  for (uint32_t i = 0; i < asignacion->cant_paginas; i++)
    liberar_pagina(list_get(tabla->paginas, asignacion->primera_pagina + i));
  log_info(logger,
           "## JID: %u - Liberación - Dirección Lógica: %u - Tamaño: %u", jid,
           asignacion->dir_logica, asignacion->tamanio);
  free(asignacion);
  pthread_mutex_unlock(&mutex_memoria);
}

// ---------- Obtención de marco y carga de página ----------

int32_t memoria_obtener_marco(uint32_t jid, uint32_t nro_pagina) {
  int32_t marco = -1;
  pthread_mutex_lock(&mutex_memoria);
  t_pagina* pagina = buscar_pagina(jid, nro_pagina);
  if (pagina == NULL) {
    log_error(logger, "JID: %u - La página %u no está asignada", jid,
              nro_pagina);
  } else if (pagina->presente) {
    pagina->lockeada = true;
    registrar_acceso(pagina, false);
    marco = pagina->marco;
  }
  pthread_mutex_unlock(&mutex_memoria);
  return marco;
}

static int32_t buscar_marco_libre(void) {
  for (uint32_t m = 0; m < cant_marcos; m++)
    if (!marcos[m].ocupado) return m;
  return -1;
}

static bool es_candidata(uint32_t marco) {
  return marcos[marco].ocupado && !marcos[marco].pagina->lockeada;
}

// LRU: la página sin lockear con el acceso más viejo
static int32_t victima_lru(void) {
  int32_t victima = -1;
  for (uint32_t m = 0; m < cant_marcos; m++) {
    if (!es_candidata(m)) continue;
    if (victima == -1 ||
        marcos[m].pagina->ultimo_acceso < marcos[victima].pagina->ultimo_acceso)
      victima = m;
  }
  return victima;
}

// Clock modificado: 1ra vuelta busca (u=0, m=0) sin tocar nada; 2da vuelta
// busca (u=0, m=1) bajando el bit de uso de las que saltea; si no, repite.
// El puntero queda en el marco siguiente a la víctima.
static int32_t victima_clock_m(void) {
  bool hay_candidata = false;
  for (uint32_t m = 0; m < cant_marcos; m++)
    if (es_candidata(m)) hay_candidata = true;
  if (!hay_candidata) return -1;

  while (true) {
    for (uint32_t i = 0; i < cant_marcos; i++) {
      uint32_t m = puntero_clock;
      puntero_clock = (puntero_clock + 1) % cant_marcos;
      if (!es_candidata(m)) continue;
      t_pagina* p = marcos[m].pagina;
      if (!p->uso && !p->modificada) return m;
    }
    for (uint32_t i = 0; i < cant_marcos; i++) {
      uint32_t m = puntero_clock;
      puntero_clock = (puntero_clock + 1) % cant_marcos;
      if (!es_candidata(m)) continue;
      t_pagina* p = marcos[m].pagina;
      if (!p->uso && p->modificada) return m;
      p->uso = false;
    }
  }
}

// Saca la víctima de su marco. Solo se escribe en el Offload si cambió.
static void desalojar(uint32_t marco) {
  t_marco* m = &marcos[marco];
  t_pagina* victima = m->pagina;
  log_info(logger,
           "## Reemplazo - Marco: %u - JID Víctima: %u - Página "
           "Víctima: %u",
           marco, m->jid, m->nro_pagina);
  if (victima->modificada) {
    escribir_en_offload(victima->slot_offload, memoria + marco * tam_pagina);
    victima->en_offload = true;
    log_info(logger, "## JID: %u - Bajada al Offload - Página: %u", m->jid,
             m->nro_pagina);
  }
  victima->presente = false;
  victima->modificada = false;
  victima->uso = false;
  m->ocupado = false;
  m->pagina = NULL;
}

void memoria_cargar_pagina(uint32_t jid, uint32_t nro_pagina) {
  pthread_mutex_lock(&mutex_memoria);
  t_pagina* pagina = buscar_pagina(jid, nro_pagina);
  if (pagina == NULL || pagina->presente) {  // inválida, o ya la cargaron
    pthread_mutex_unlock(&mutex_memoria);
    return;
  }
  int32_t marco = buscar_marco_libre();
  if (marco == -1) {
    marco = usar_lru ? victima_lru() : victima_clock_m();
    if (marco == -1) {
      // Todo lockeado: no se carga. El Job vuelve a READY y, al reintentar la
      // instrucción, da Page Fault de nuevo.
      pthread_mutex_unlock(&mutex_memoria);
      log_error(logger, "JID: %u - No hay marcos sin lockear para la página %u",
                jid, nro_pagina);
      return;
    }
    desalojar(marco);
  }
  void* destino = memoria + marco * tam_pagina;
  if (pagina->en_offload)
    leer_de_offload(pagina->slot_offload, destino);
  else
    memset(destino, 0, tam_pagina);  // nunca se escribió: arranca en ceros
  pagina->presente = true;
  pagina->marco = marco;
  pagina->modificada = false;
  registrar_acceso(pagina, false);
  marcos[marco] = (t_marco){true, jid, nro_pagina, pagina};
  log_info(logger, "## JID: %u - Subida a Memoria - Página: %u - Marco: %d",
           jid, nro_pagina, marco);
  pthread_mutex_unlock(&mutex_memoria);
}

// ---------- Lectura, escritura y deslockeo ----------

// La página dueña de [dir_fisica, dir_fisica + tamanio), si es del Job y el
// rango no se sale del marco
static t_pagina* pagina_de_direccion(uint32_t jid, uint32_t dir_fisica,
                                     uint32_t tamanio) {
  if (dir_fisica >= tam_memoria) return NULL;
  if (dir_fisica % tam_pagina + tamanio > tam_pagina) return NULL;
  t_marco* m = &marcos[dir_fisica / tam_pagina];
  if (!m->ocupado || m->jid != jid) return NULL;
  return m->pagina;
}

bool memoria_leer(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                  void* destino) {
  pthread_mutex_lock(&mutex_memoria);
  t_pagina* pagina = pagina_de_direccion(jid, dir_fisica, tamanio);
  if (pagina != NULL) {
    memcpy(destino, memoria + dir_fisica, tamanio);
    registrar_acceso(pagina, false);
    log_info(logger, "## JID: %u - Lectura - Dir. Física: %u - Tamaño: %u", jid,
             dir_fisica, tamanio);
  }
  pthread_mutex_unlock(&mutex_memoria);
  if (pagina == NULL)
    log_error(logger, "JID: %u - Lectura inválida en %u (%u bytes)", jid,
              dir_fisica, tamanio);
  return pagina != NULL;
}

bool memoria_escribir(uint32_t jid, uint32_t dir_fisica, uint32_t tamanio,
                      void* origen) {
  pthread_mutex_lock(&mutex_memoria);
  t_pagina* pagina = pagina_de_direccion(jid, dir_fisica, tamanio);
  if (pagina != NULL) {
    memcpy(memoria + dir_fisica, origen, tamanio);
    registrar_acceso(pagina, true);
    log_info(logger, "## JID: %u - Escritura - Dir. Física: %u - Tamaño: %u",
             jid, dir_fisica, tamanio);
  }
  pthread_mutex_unlock(&mutex_memoria);
  if (pagina == NULL)
    log_error(logger, "JID: %u - Escritura inválida en %u (%u bytes)", jid,
              dir_fisica, tamanio);
  return pagina != NULL;
}

void memoria_deslockear(uint32_t jid) {
  pthread_mutex_lock(&mutex_memoria);
  t_tabla_job* tabla = buscar_tabla(jid);
  for (int i = 0; tabla != NULL && i < list_size(tabla->paginas); i++)
    ((t_pagina*)list_get(tabla->paginas, i))->lockeada = false;
  pthread_mutex_unlock(&mutex_memoria);
}

// ---------- Consola ----------

void memoria_info(void) {
  pthread_mutex_lock(&mutex_memoria);
  uint32_t ocupados = 0, lockeados = 0;
  for (uint32_t m = 0; m < cant_marcos; m++) {
    if (!marcos[m].ocupado) continue;
    ocupados++;
    if (marcos[m].pagina->lockeada) lockeados++;
  }
  uint32_t slots_usados = cant_slots - slots_libres();
  printf("Memoria ocupada: %.2f%% (%u de %u marcos)\n",
         100.0 * ocupados / cant_marcos, ocupados, cant_marcos);
  printf("Offload ocupado: %.2f%% (%u de %u páginas)\n",
         100.0 * slots_usados / cant_slots, slots_usados, cant_slots);
  printf("Marcos: %u libres - %u lockeados - %u totales\n",
         cant_marcos - ocupados, lockeados, cant_marcos);
  printf("Jobs actuales: %d\n", dictionary_size(tablas));
  pthread_mutex_unlock(&mutex_memoria);
}

static void imprimir_job(char* jid, void* elemento) {
  t_tabla_job* tabla = elemento;
  int residentes = 0, totales = 0;
  for (int i = 0; i < list_size(tabla->paginas); i++) {
    t_pagina* pagina = list_get(tabla->paginas, i);
    if (!pagina->asignada) continue;
    totales++;
    if (pagina->presente) residentes++;
  }
  printf("JID: %s - Páginas residentes: %d - Páginas totales: %d\n", jid,
         residentes, totales);
}

void memoria_tls(void) {
  pthread_mutex_lock(&mutex_memoria);
  if (dictionary_size(tablas) == 0) printf("No hay Jobs\n");
  dictionary_iterator(tablas, imprimir_job);
  pthread_mutex_unlock(&mutex_memoria);
}