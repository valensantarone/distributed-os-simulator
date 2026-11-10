#ifndef STORAGE_SRC_RECUPERACION_H_
#define STORAGE_SRC_RECUPERACION_H_

#include <stdint.h>

//Recorre el journal en orden cronológico: un asiento con COMMIT y sin
//APPLIED se vuelve a aplicar entero; uno sin COMMIT, o con las dos marcas, se
//descarta. Devuelve el id más alto que encontró (0 si el journal está vacío o
//no existe), para que los asientos nuevos sigan numerando desde ahí.
uint32_t recuperar(char* path_journal);

#endif  // STORAGE_SRC_RECUPERACION_H_