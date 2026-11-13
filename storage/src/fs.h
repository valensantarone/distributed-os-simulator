#ifndef STORAGE_SRC_FS_H_
#define STORAGE_SRC_FS_H_

#include <commons/collections/list.h>
#include <commons/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/paquete.h>  // t_resultado_checkpoint

// Monta el volumen (si el archivo no existe, lo crea y lo formatea), abre el
// journal y recupera los asientos que quedaron sin aplicar.
void fs_iniciar(t_config* config);

// Las tres operaciones. Guardar y eliminar pasan por el journal. Las usan los
// pedidos del Planificador y la consola (WRITE / RM).
t_resultado_checkpoint fs_guardar(char* nombre, void* datos, uint32_t tamanio);
// En *datos deja una copia del contenido (la libera el que llama)
t_resultado_checkpoint fs_cargar(char* nombre, void** datos, uint32_t* tamanio);
// Si tamanio no es NULL, deja ahí cuántos bytes tenía el Checkpoint
t_resultado_checkpoint fs_eliminar(char* nombre, uint32_t* tamanio);

// Comandos de consola
void fs_info(void);
void fs_ls(void);
void fs_cat(char* nombre, uint32_t cant_bytes);

// ---------- Lo que fs.c comparte con recuperacion.c ----------

typedef struct __attribute__((packed)) {
  uint8_t estado;  // 0 libre, 1 ocupada
  char nombre[23];
  uint32_t tamanio;
  uint32_t primer_bloque;
} t_entrada_directorio;

// Una modificación del volumen. Guardar y eliminar arman una lista de estas
// (el "plan"), la escriben en el journal como un asiento y recién después la
// aplican. Cada una dice cómo queda un lugar del volumen, no cuánto cambia:
// por eso aplicarla dos veces da lo mismo que aplicarla una (idempotente).
typedef enum { MOD_DIRECTORIO, MOD_FAT, MOD_BLOQUE } t_tipo_modificacion;

typedef struct {
  t_tipo_modificacion tipo;
  uint32_t indice;  // nro de entrada de directorio, de entrada FAT o de bloque
  uint32_t valor;   // MOD_FAT: el valor nuevo de la entrada
  t_entrada_directorio entrada;  // MOD_DIRECTORIO: cómo queda la entrada
  void* datos;                   // MOD_BLOQUE: tam_bloque bytes
} t_modificacion;

t_modificacion* nueva_modificacion(t_tipo_modificacion tipo, uint32_t indice);
void destruir_modificacion(void* modificacion);

uint32_t fs_tam_bloque(void);

// true si la modificación cae adentro del volumen (una línea rota del journal
// puede traer cualquier número)
bool modificacion_valida(t_modificacion* modificacion);

// Aplica un asiento con COMMIT que quedó sin aplicar y le agrega la marca
// RECORD_APPLIED. Solo al arrancar, antes de atender pedidos.
void fs_reaplicar(uint32_t id, t_list* plan, char* nombre);

#endif  // STORAGE_SRC_FS_H_