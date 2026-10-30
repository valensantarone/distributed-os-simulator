#ifndef STORAGE_SRC_FS_H_
#define STORAGE_SRC_FS_H_

#include <commons/config.h>
#include <stdint.h>

typedef enum {
  FS_OK,
  FS_SIN_ESPACIO,
  FS_DIRECTORIO_LLENO,
  FS_NO_EXISTE
} t_resultado_fs;

// Monta el volumen. Si el archivo no existe, lo crea y lo formatea.
void fs_iniciar(t_config* config);

// Guardar (crea o reemplaza) y eliminar un Checkpoint. En el Check 3 las usa
// la consola (WRITE / RM); en el Check 4, los pedidos del Planificador.
t_resultado_fs fs_guardar(char* nombre, void* datos, uint32_t tamanio);
t_resultado_fs fs_eliminar(char* nombre);

// Comandos de consola
void fs_info(void);
void fs_ls(void);
void fs_cat(char* nombre, uint32_t cant_bytes);

#endif  // STORAGE_SRC_FS_H_
