#ifndef INSTRUCCIONES_H_
#define INSTRUCCIONES_H_

#include <stdbool.h>
#include <stdint.h>
#include "utils/paquete.h"

//Tipos de instrucciones
typedef enum {
    INST_LENGUAJE_COMUN,
    INST_INIT_JOB,
    INST_EXIT
} t_tipo_instruccion;

//Decodificación y ejecución local
t_tipo_instruccion decodificar_y_ejecutar(char* instruccion_str, t_contexto* contexto, bool* pc_modificado);

//Helpers de registros y parseo de parámetros
uint32_t* obtener_registro(t_contexto* contexto, char* nombre_registro);
char* obtener_parametro_archivo(char* instruccion_str);

#endif