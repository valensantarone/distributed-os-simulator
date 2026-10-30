#define _POSIX_C_SOURCE 200809L
#include "fs.h"

#include <commons/bitarray.h>
#include <commons/collections/list.h>
#include <commons/log.h>
#include <commons/string.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern t_log* logger;

#define FIRMA "FAT32_TRAIN"
#define FAT_LIBRE 0x00000000u
#define FAT_EOF 0x0FFFFFFFu
#define FAT_RESERVADO 0x0FFFFFFEu
#define FAT_MASCARA 0x0FFFFFFFu  // los 4 bits más altos están reservados

// Bloque 0. Sin packed, el compilador agrega 1 byte de relleno después de la
// firma (para alinear el uint32_t) y el struct ocuparía 24 bytes, no 23.
typedef struct __attribute__((packed)) {
  char firma[11];  // "FAT32_TRAIN", sin '\0'
  uint32_t tam_bloque;
  uint32_t cant_bloques;
  uint32_t bloques_directorio;
} t_superbloque;

typedef struct __attribute__((packed)) {
  uint8_t estado;  // 0 libre, 1 ocupada
  char nombre[23];
  uint32_t tamanio;
  uint32_t primer_bloque;
} t_entrada_directorio;

_Static_assert(sizeof(t_superbloque) == 23, "superbloque de 23 bytes");
_Static_assert(sizeof(t_entrada_directorio) == 32, "entrada de 32 bytes");

// Una modificación del volumen. Guardar y eliminar arman una lista de estas y
// después la aplican entera. En el Check 4 esa lista es el asiento del
// journal: se escribe en el journal, se hace commit y recién ahí se aplica.
typedef enum { MOD_DIRECTORIO, MOD_FAT, MOD_BLOQUE } t_tipo_modificacion;

typedef struct {
  t_tipo_modificacion tipo;
  uint32_t indice;  // nro de entrada de directorio, de entrada FAT o de bloque
  uint32_t valor;   // MOD_FAT: el valor nuevo de la entrada
  t_entrada_directorio entrada;  // MOD_DIRECTORIO: cómo queda la entrada
  void* datos;                   // MOD_BLOQUE: tam_bloque bytes
} t_modificacion;

static void* volumen;  // el archivo entero, mapeado con mmap
static size_t tam_volumen;
static uint32_t tam_bloque;
static uint32_t cant_bloques;
static uint32_t bloques_directorio;
static uint32_t bloques_fat;
static uint32_t primer_bloque_directorio;
static uint32_t primer_bloque_datos;
static uint32_t cant_entradas;
static int retardo_bloque;
static pthread_mutex_t mutex_fs = PTHREAD_MUTEX_INITIALIZER;

// Bloques que la operación en curso ya leyó y ya escribió: el retardo se paga
// una vez por bloque distinto, aunque se usen varias entradas del mismo bloque
// y aunque el volumen esté en memoria (issue #5317 del foro)
static t_bitarray* bloques_leidos;
static t_bitarray* bloques_escritos;

static void dormir_ms(int ms) {
  struct timespec espera = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&espera, NULL);
}

// Al empezar cada operación: todavía no se accedió a ningún bloque
static void empezar_operacion(void) {
  memset(bloques_leidos->bitarray, 0, bloques_leidos->size);
  memset(bloques_escritos->bitarray, 0, bloques_escritos->size);
}

static void acceder(t_bitarray* accedidos, uint32_t bloque) {
  if (bitarray_test_bit(accedidos, bloque)) return;  // ya se pagó
  bitarray_set_bit(accedidos, bloque);
  dormir_ms(retardo_bloque);
}

static uint32_t* fat(void) {
  return (uint32_t*)((uint8_t*)volumen + tam_bloque);
}

static t_entrada_directorio* directorio(void) {
  return (t_entrada_directorio*)((uint8_t*)volumen +
                                 primer_bloque_directorio * tam_bloque);
}

static void calcular_regiones(void) {
  bloques_fat = (cant_bloques * sizeof(uint32_t) + tam_bloque - 1) / tam_bloque;
  primer_bloque_directorio = 1 + bloques_fat;
  primer_bloque_datos = primer_bloque_directorio + bloques_directorio;
  cant_entradas =
      bloques_directorio * tam_bloque / sizeof(t_entrada_directorio);
}

// En qué bloque del volumen está una entrada de la FAT o del directorio
static uint32_t bloque_de_fat(uint32_t entrada) {
  return 1 + entrada * sizeof(uint32_t) / tam_bloque;
}

static uint32_t bloque_de_directorio(uint32_t indice) {
  return primer_bloque_directorio +
         indice * sizeof(t_entrada_directorio) / tam_bloque;
}

// ---------- Accesos al volumen (con retardo y log) ----------

static uint32_t fat_leer(uint32_t entrada) {
  acceder(bloques_leidos, bloque_de_fat(entrada));
  uint32_t valor = fat()[entrada] & FAT_MASCARA;
  log_info(logger, "## Acceso FAT - Entrada: %u - Valor: 0x%08X", entrada,
           valor);
  return valor;
}

static void fat_escribir(uint32_t entrada, uint32_t valor) {
  acceder(bloques_escritos, bloque_de_fat(entrada));
  uint32_t* actual = &fat()[entrada];
  *actual = (*actual & ~FAT_MASCARA) | (valor & FAT_MASCARA);
  log_info(logger, "## Acceso FAT - Entrada: %u - Valor: 0x%08X", entrada,
           valor);
}

static void bloque_leer(uint32_t nro, void* destino, char* checkpoint) {
  acceder(bloques_leidos, nro);
  memcpy(destino, (uint8_t*)volumen + nro * tam_bloque, tam_bloque);
  log_info(logger, "## Acceso Bloque - Checkpoint: %s - Bloque: %u", checkpoint,
           nro);
}

static void bloque_escribir(uint32_t nro, void* origen, char* checkpoint) {
  acceder(bloques_escritos, nro);
  memcpy((uint8_t*)volumen + nro * tam_bloque, origen, tam_bloque);
  log_info(logger, "## Acceso Bloque - Checkpoint: %s - Bloque: %u", checkpoint,
           nro);
}

// Recorre el directorio leyendo sus bloques. buscar_libre = false: la entrada
// ocupada con ese nombre. true: la primera libre. -1 si no hay.
static int buscar_entrada(char* nombre, bool buscar_libre) {
  for (uint32_t i = 0; i < cant_entradas; i++) {
    acceder(bloques_leidos, bloque_de_directorio(i));
    t_entrada_directorio* entrada = &directorio()[i];
    if (buscar_libre && entrada->estado == 0) return i;
    if (!buscar_libre && entrada->estado == 1 &&
        strncmp(entrada->nombre, nombre, sizeof(entrada->nombre)) == 0)
      return i;
  }
  return -1;
}

// Junta en `libres` hasta `cantidad` bloques libres y devuelve cuántos
// encontró. Lee la FAT de a bloques, con retardo, y corta apenas tiene los
// que necesita. No loguea cada entrada que mira (serían cientos de logs):
// "Acceso FAT" sale para las entradas de una cadena y las que se modifican.
static uint32_t buscar_libres(uint32_t cantidad, uint32_t* libres) {
  uint32_t encontrados = 0;
  for (uint32_t b = primer_bloque_datos;
       b < cant_bloques && encontrados < cantidad; b++) {
    acceder(bloques_leidos, bloque_de_fat(b));
    if ((fat()[b] & FAT_MASCARA) == FAT_LIBRE) libres[encontrados++] = b;
  }
  return encontrados;
}

// Los bloques de un Checkpoint, en orden (lee la FAT entrada por entrada)
static t_list* leer_cadena(uint32_t primer_bloque) {
  t_list* cadena = list_create();
  uint32_t actual = primer_bloque;
  while (actual < cant_bloques && list_size(cadena) < (int)cant_bloques) {
    uint32_t* nro = malloc(sizeof(uint32_t));
    *nro = actual;
    list_add(cadena, nro);
    actual = fat_leer(actual);  // FAT_EOF corta el while
  }
  return cadena;
}

// ---------- Formateo y montaje ----------

static void formatear(void) {
  // Todo en 0: la FAT queda libre y el directorio con todas sus entradas
  // libres. Después: superbloque y bloques reservados.
  memset(volumen, 0, tam_volumen);
  t_superbloque superbloque;
  memcpy(superbloque.firma, FIRMA, sizeof(superbloque.firma));
  superbloque.tam_bloque = tam_bloque;
  superbloque.cant_bloques = cant_bloques;
  superbloque.bloques_directorio = bloques_directorio;
  memcpy(volumen, &superbloque, sizeof(t_superbloque));
  for (uint32_t b = 0; b < primer_bloque_datos; b++)
    fat()[b] = FAT_RESERVADO;  // superbloque + FAT + directorio
  msync(volumen, tam_volumen, MS_SYNC);
  log_info(logger,
           "Volumen formateado: %u bloques de %u bytes - FAT: %u bloques - "
           "Directorio: %u bloques (%u entradas) - Datos desde el bloque %u",
           cant_bloques, tam_bloque, bloques_fat, bloques_directorio,
           cant_entradas, primer_bloque_datos);
}

void fs_iniciar(t_config* config) {
  char* path_storage = config_get_string_value(config, "PATH_STORAGE");
  retardo_bloque = config_get_int_value(config, "RETARDO_ACCESO_BLOQUE");
  mkdir(path_storage, 0755);  // si ya existe, no pasa nada
  char* path = string_ends_with(path_storage, "/")
                   ? string_from_format("%sfat32_train.dat", path_storage)
                   : string_from_format("%s/fat32_train.dat", path_storage);

  bool existe = access(path, F_OK) == 0;
  int fd = open(path, O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    log_error(logger, "No se pudo abrir el volumen %s", path);
    exit(EXIT_FAILURE);
  }
  if (existe) {  // la geometría la dice el superbloque, no el config
    t_superbloque superbloque;
    if (pread(fd, &superbloque, sizeof(superbloque), 0) !=
            sizeof(superbloque) ||
        memcmp(superbloque.firma, FIRMA, sizeof(superbloque.firma)) != 0) {
      log_error(logger, "%s no es un volumen FAT32_TRAIN", path);
      exit(EXIT_FAILURE);
    }
    tam_bloque = superbloque.tam_bloque;
    cant_bloques = superbloque.cant_bloques;
    bloques_directorio = superbloque.bloques_directorio;
  } else {
    tam_bloque = config_get_int_value(config, "TAM_BLOQUE");
    cant_bloques = config_get_int_value(config, "CANT_BLOQUES");
    bloques_directorio = config_get_int_value(config, "BLOQUES_DIRECTORIO");
  }
  calcular_regiones();
  size_t bytes_bitmap = (cant_bloques + 7) / 8;
  bloques_leidos = bitarray_create_with_mode(calloc(1, bytes_bitmap),
                                             bytes_bitmap, LSB_FIRST);
  bloques_escritos = bitarray_create_with_mode(calloc(1, bytes_bitmap),
                                               bytes_bitmap, LSB_FIRST);
  tam_volumen = (size_t)cant_bloques * tam_bloque;
  if (!existe && ftruncate(fd, tam_volumen) != 0) {
    log_error(logger, "No se pudo dimensionar el volumen %s", path);
    exit(EXIT_FAILURE);
  }
  volumen = mmap(NULL, tam_volumen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);  // el mapeo sigue vivo sin el fd
  if (volumen == MAP_FAILED) {
    log_error(logger, "No se pudo mapear el volumen %s", path);
    exit(EXIT_FAILURE);
  }
  if (existe)
    log_info(logger, "Volumen %s montado: %u bloques de %u bytes", path,
             cant_bloques, tam_bloque);
  else
    formatear();
  free(path);
}

// ---------- Guardar y eliminar ----------

static t_modificacion* nueva_modificacion(t_tipo_modificacion tipo,
                                          uint32_t indice) {
  t_modificacion* modificacion = calloc(1, sizeof(t_modificacion));
  modificacion->tipo = tipo;
  modificacion->indice = indice;
  return modificacion;
}

static void agregar_fat(t_list* plan, uint32_t entrada, uint32_t valor) {
  t_modificacion* modificacion = nueva_modificacion(MOD_FAT, entrada);
  modificacion->valor = valor;
  list_add(plan, modificacion);
}

static void agregar_directorio(t_list* plan, uint32_t indice,
                               t_entrada_directorio entrada) {
  t_modificacion* modificacion = nueva_modificacion(MOD_DIRECTORIO, indice);
  modificacion->entrada = entrada;
  list_add(plan, modificacion);
}

static void agregar_bloque(t_list* plan, uint32_t nro, void* datos,
                           uint32_t tamanio) {
  t_modificacion* modificacion = nueva_modificacion(MOD_BLOQUE, nro);
  modificacion->datos = calloc(1, tam_bloque);  // el resto queda en 0
  memcpy(modificacion->datos, datos, tamanio);
  list_add(plan, modificacion);
}

static void destruir_modificacion(void* elemento) {
  t_modificacion* modificacion = elemento;
  free(modificacion->datos);
  free(modificacion);
}

static void aplicar(t_list* plan, char* nombre) {
  for (int i = 0; i < list_size(plan); i++) {
    t_modificacion* modificacion = list_get(plan, i);
    switch (modificacion->tipo) {
      case MOD_DIRECTORIO:
        acceder(bloques_escritos, bloque_de_directorio(modificacion->indice));
        directorio()[modificacion->indice] = modificacion->entrada;
        break;
      case MOD_FAT:
        fat_escribir(modificacion->indice, modificacion->valor);
        break;
      case MOD_BLOQUE:
        bloque_escribir(modificacion->indice, modificacion->datos, nombre);
        break;
    }
  }
  msync(volumen, tam_volumen, MS_SYNC);
}

t_resultado_fs fs_guardar(char* nombre, void* datos, uint32_t tamanio) {
  pthread_mutex_lock(&mutex_fs);
  empezar_operacion();
  int indice = buscar_entrada(nombre, false);
  t_list* cadena_vieja = indice >= 0
                             ? leer_cadena(directorio()[indice].primer_bloque)
                             : list_create();
  if (indice < 0) indice = buscar_entrada(nombre, true);

  uint32_t necesarios =
      tamanio == 0 ? 1 : (tamanio + tam_bloque - 1) / tam_bloque;
  uint32_t reusados = (uint32_t)list_size(cadena_vieja) < necesarios
                          ? (uint32_t)list_size(cadena_vieja)
                          : necesarios;
  uint32_t faltan = necesarios - reusados;
  uint32_t* nuevos = calloc(faltan + 1, sizeof(uint32_t));
  t_resultado_fs resultado = FS_OK;
  if (indice < 0) {
    log_error(logger, "## ERROR - Directorio lleno al intentar guardar: %s",
              nombre);
    resultado = FS_DIRECTORIO_LLENO;
  } else if (buscar_libres(faltan, nuevos) < faltan) {
    log_error(logger,
              "## ERROR - Falta de espacio en FAT al intentar guardar: %s",
              nombre);
    resultado = FS_SIN_ESPACIO;
  }
  if (resultado != FS_OK) {  // no se tocó nada del volumen
    free(nuevos);
    list_destroy_and_destroy_elements(cadena_vieja, free);
    pthread_mutex_unlock(&mutex_fs);
    return resultado;
  }

  // La cadena nueva reusa los primeros bloques de la vieja y agrega los
  // libres que hagan falta
  uint32_t* cadena = calloc(necesarios, sizeof(uint32_t));
  for (uint32_t i = 0; i < necesarios; i++)
    cadena[i] = i < reusados ? *(uint32_t*)list_get(cadena_vieja, i)
                             : nuevos[i - reusados];

  t_list* plan = list_create();
  t_entrada_directorio entrada = {0};
  entrada.estado = 1;
  strncpy(entrada.nombre, nombre, sizeof(entrada.nombre) - 1);
  entrada.tamanio = tamanio;
  entrada.primer_bloque = cadena[0];
  agregar_directorio(plan, indice, entrada);
  for (uint32_t i = 0; i < necesarios; i++)
    agregar_fat(plan, cadena[i], i + 1 < necesarios ? cadena[i + 1] : FAT_EOF);
  for (int i = reusados; i < list_size(cadena_vieja); i++)  // sobrantes
    agregar_fat(plan, *(uint32_t*)list_get(cadena_vieja, i), FAT_LIBRE);
  for (uint32_t i = 0; i < necesarios; i++) {
    uint32_t inicio = i * tam_bloque;
    uint32_t cuanto =
        tamanio - inicio < tam_bloque ? tamanio - inicio : tam_bloque;
    agregar_bloque(plan, cadena[i], (uint8_t*)datos + inicio, cuanto);
  }
  aplicar(plan, nombre);

  list_destroy_and_destroy_elements(plan, destruir_modificacion);
  list_destroy_and_destroy_elements(cadena_vieja, free);
  free(cadena);
  free(nuevos);
  pthread_mutex_unlock(&mutex_fs);
  return FS_OK;
}

t_resultado_fs fs_eliminar(char* nombre) {
  pthread_mutex_lock(&mutex_fs);
  empezar_operacion();
  int indice = buscar_entrada(nombre, false);
  if (indice < 0) {
    pthread_mutex_unlock(&mutex_fs);
    log_warning(logger, "No existe el Checkpoint %s", nombre);
    return FS_NO_EXISTE;
  }
  t_list* cadena = leer_cadena(directorio()[indice].primer_bloque);
  t_list* plan = list_create();
  t_entrada_directorio libre = {0};
  agregar_directorio(plan, indice, libre);
  for (int i = 0; i < list_size(cadena); i++)
    agregar_fat(plan, *(uint32_t*)list_get(cadena, i), FAT_LIBRE);
  aplicar(plan, nombre);

  list_destroy_and_destroy_elements(plan, destruir_modificacion);
  list_destroy_and_destroy_elements(cadena, free);
  pthread_mutex_unlock(&mutex_fs);
  return FS_OK;
}

// ---------- Comandos de consola ----------

void fs_info(void) {
  pthread_mutex_lock(&mutex_fs);
  empezar_operacion();
  acceder(bloques_leidos, 0);
  t_superbloque superbloque;
  memcpy(&superbloque, volumen, sizeof(t_superbloque));
  pthread_mutex_unlock(&mutex_fs);

  printf("Firma: %.11s\n", superbloque.firma);
  printf("Tamaño de bloque: %u bytes\n", superbloque.tam_bloque);
  printf("Cantidad de bloques: %u\n", superbloque.cant_bloques);
  printf("Bloques del directorio: %u\n", superbloque.bloques_directorio);
  printf(
      "(FAT: %u bloques - Directorio desde el bloque %u - Datos desde el "
      "bloque %u)\n",
      bloques_fat, primer_bloque_directorio, primer_bloque_datos);
}

// Todas las entradas, libres y ocupadas, con todos sus atributos (#5307)
void fs_ls(void) {
  pthread_mutex_lock(&mutex_fs);
  empezar_operacion();
  uint32_t ocupadas = 0;
  for (uint32_t i = 0; i < cant_entradas; i++) {
    acceder(bloques_leidos, bloque_de_directorio(i));
    t_entrada_directorio* entrada = &directorio()[i];
    if (entrada->estado == 1) ocupadas++;
    printf(
        "Entrada %u - Estado: %u (%s) - Nombre: \"%.23s\" - Tamaño: %u bytes - "
        "Primer bloque: 0x%08X\n",
        i, entrada->estado, entrada->estado == 1 ? "ocupada" : "libre",
        entrada->nombre, entrada->tamanio, entrada->primer_bloque);
  }
  printf("%u entradas ocupadas, %u libres\n", ocupadas,
         cant_entradas - ocupadas);
  pthread_mutex_unlock(&mutex_fs);
}

void fs_cat(char* nombre, uint32_t cant_bytes) {
  pthread_mutex_lock(&mutex_fs);
  empezar_operacion();
  int indice = buscar_entrada(nombre, false);
  if (indice < 0) {
    pthread_mutex_unlock(&mutex_fs);
    printf("No existe el Checkpoint %s\n", nombre);
    return;
  }
  t_entrada_directorio entrada = directorio()[indice];
  uint32_t a_mostrar =
      cant_bytes < entrada.tamanio ? cant_bytes : entrada.tamanio;
  uint8_t* contenido = malloc(tam_bloque);
  char* salida = string_new();
  uint32_t bloque = entrada.primer_bloque;
  for (uint32_t leidos = 0; leidos < a_mostrar && bloque < cant_bloques;) {
    bloque_leer(bloque, contenido, nombre);
    for (uint32_t j = 0; j < tam_bloque && leidos < a_mostrar; j++, leidos++)
      string_append_with_format(&salida, "%02X ", contenido[j]);
    if (leidos < a_mostrar) bloque = fat_leer(bloque);  // siguiente bloque
  }
  pthread_mutex_unlock(&mutex_fs);
  printf("%s (%u de %u bytes): %s\n", nombre, a_mostrar, entrada.tamanio,
         salida);
  free(salida);
  free(contenido);
}
