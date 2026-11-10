#define _POSIX_C_SOURCE 200809L
#include "recuperacion.h"

#include <commons/collections/list.h>
#include <commons/log.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

extern t_log* logger;

typedef struct {
  uint32_t id;
  t_list* plan;  //t_modificacion*
  bool commit;
  bool applied;
  bool corrupto;  //alguna línea no se pudo leer
} t_asiento;

static int valor_hexa(char caracter) {
  if (isdigit((unsigned char)caracter)) return caracter - '0';
  caracter = toupper((unsigned char)caracter);
  if (caracter >= 'A' && caracter <= 'F') return caracter - 'A' + 10;
  return -1;
}

//Una línea bien escrita puede traer un número que no entra en el volumen
static t_modificacion* validada(t_modificacion* modificacion) {
  if (modificacion_valida(modificacion)) return modificacion;
  destruir_modificacion(modificacion);
  return NULL;
}

//NULL si la línea no es una modificación válida: por ejemplo, una que quedó cortada a la mitad cuando
//mataron al Storage. `fin` (%n) asegura que el formato coincida con la línea entera.
static t_modificacion* texto_a_modificacion(char* linea) {
  uint32_t indice, valor, tamanio, primer_bloque;
  char nombre[23] = {0};
  int fin = -1;
  if (sscanf(linea, "OP_FAT: Entrada 0x%X -> Valor 0x%X%n", &indice, &valor, &fin) == 2 && fin > 0 && linea[fin] == '\0') {
    t_modificacion* modificacion = nueva_modificacion(MOD_FAT, indice);
    modificacion->valor = valor;
    return validada(modificacion);
  }
  fin = -1;
  if (sscanf(linea, "OP_DIR: Directorio %u - Libre%n", &indice, &fin) == 1 && fin > 0 && linea[fin] == '\0')  //entrada en 0: libre
    return validada(nueva_modificacion(MOD_DIRECTORIO, indice));
  fin = -1;
  if (sscanf(linea,
             "OP_DIR: Directorio %u - Checkpoint \"%22[^\"]\" - Tamaño %u - " "Bloque Inicial 0x%X%n",
             &indice, nombre, &tamanio, &primer_bloque, &fin) == 4 && fin > 0 && linea[fin] == '\0') {
    t_modificacion* modificacion = nueva_modificacion(MOD_DIRECTORIO, indice);
    modificacion->entrada.estado = 1;
    memcpy(modificacion->entrada.nombre, nombre, sizeof(nombre));
    modificacion->entrada.tamanio = tamanio;
    modificacion->entrada.primer_bloque = primer_bloque;
    return validada(modificacion);
  }
  fin = -1;
  uint32_t tam_bloque = fs_tam_bloque();
  if (sscanf(linea, "OP_WRITE: Bloque 0x%X - Contenido: %n", &indice, &fin) == 1 &&
      fin > 0 && strlen(linea + fin) == 2 * tam_bloque) {
    uint8_t* datos = malloc(tam_bloque);
    for (uint32_t i = 0; i < tam_bloque; i++) {
      int alto = valor_hexa(linea[fin + 2 * i]);
      int bajo = valor_hexa(linea[fin + 2 * i + 1]);
      if (alto < 0 || bajo < 0) {
        free(datos);
        return NULL;
      }
      datos[i] = alto * 16 + bajo;
    }
    t_modificacion* modificacion = nueva_modificacion(MOD_BLOQUE, indice);
    modificacion->datos = datos;
    return validada(modificacion);
  }
  return NULL;
}

static void destruir_asiento(void* elemento) {
  t_asiento* asiento = elemento;
  list_destroy_and_destroy_elements(asiento->plan, destruir_modificacion);
  free(asiento);
}

static t_asiento* buscar_asiento(t_list* asientos, uint32_t id) {
  for (int i = 0; i < list_size(asientos); i++) {
    t_asiento* asiento = list_get(asientos, i);
    if (asiento->id == id) return asiento;
  }
  return NULL;
}

//true si la línea es exactamente una marca ("RECORD_COMMITED 3"...)
static bool es_marca(char* linea, char* formato, uint32_t* id) {
  int fin = -1;
  return sscanf(linea, formato, id, &fin) == 1 && fin > 0 && linea[fin] == '\0';
}

//El Checkpoint de un plan de guardado (para el log de bloques)
static char* nombre_del_plan(t_list* plan) {
  for (int i = 0; i < list_size(plan); i++) {
    t_modificacion* modificacion = list_get(plan, i);
    if (modificacion->tipo == MOD_DIRECTORIO &&
        modificacion->entrada.estado == 1)
      return modificacion->entrada.nombre;
  }
  return "";
}

uint32_t recuperar(char* path_journal) {
  FILE* archivo = fopen(path_journal, "r");
  if (archivo == NULL) return 0;
  t_list* asientos = list_create();
  t_asiento* actual = NULL;
  uint32_t ultimo_id = 0;
  char* linea = NULL;
  size_t capacidad = 0;
  ssize_t largo;
  while ((largo = getline(&linea, &capacidad, archivo)) != -1) {
    if (largo > 0 && linea[largo - 1] == '\n') linea[largo - 1] = '\0';
    uint32_t id;
    if (es_marca(linea, "RECORD_STARTED: %u%n", &id)) {
      actual = calloc(1, sizeof(t_asiento));
      actual->id = id;
      actual->plan = list_create();
      list_add(asientos, actual);
      if (id > ultimo_id) ultimo_id = id;
    } else if (es_marca(linea, "RECORD_COMMITED %u%n", &id)) {
      t_asiento* asiento = buscar_asiento(asientos, id);
      if (asiento != NULL) asiento->commit = true;
    } else if (es_marca(linea, "RECORD_APPLIED %u%n", &id)) {
      t_asiento* asiento = buscar_asiento(asientos, id);
      if (asiento != NULL) asiento->applied = true;
    } else if (actual != NULL && !actual->commit) {
      t_modificacion* modificacion = texto_a_modificacion(linea);
      if (modificacion == NULL)
        actual->corrupto = true;
      else
        list_add(actual->plan, modificacion);
    }
  }
  free(linea);
  fclose(archivo);

  for (int i = 0; i < list_size(asientos); i++) {
    t_asiento* asiento = list_get(asientos, i);
    if (asiento->commit && !asiento->applied && !asiento->corrupto) {
      fs_reaplicar(asiento->id, asiento->plan, nombre_del_plan(asiento->plan));
      log_info(logger, "## Recuperación - Asiento: %u - APLICADO", asiento->id);
    } else {
      log_info(logger, "## Recuperación - Asiento: %u - DESCARTADO", asiento->id);
    }
  }
  list_destroy_and_destroy_elements(asientos, destruir_asiento);
  return ultimo_id;
}