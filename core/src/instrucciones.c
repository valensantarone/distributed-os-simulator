#include "instrucciones.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <commons/string.h>

//Mapeo de registro a puntero del contexto
uint32_t* obtener_registro(t_contexto* contexto, char* nombre_registro) {
    if (strcmp(nombre_registro, "PC") == 0) return &(contexto->pc);
    if (strcmp(nombre_registro, "P0") == 0) return &(contexto->p0);
    if (strcmp(nombre_registro, "P1") == 0) return &(contexto->p1);
    if (strcmp(nombre_registro, "P2") == 0) return &(contexto->p2);
    if (strcmp(nombre_registro, "P3") == 0) return &(contexto->p3);
    if (strcmp(nombre_registro, "P4") == 0) return &(contexto->p4);
    if (strcmp(nombre_registro, "P5") == 0) return &(contexto->p5);
    if (strcmp(nombre_registro, "AX") == 0) return &(contexto->ax);
    if (strcmp(nombre_registro, "BX") == 0) return &(contexto->bx);
    if (strcmp(nombre_registro, "CX") == 0) return &(contexto->cx);
    if (strcmp(nombre_registro, "DX") == 0) return &(contexto->dx);
    if (strcmp(nombre_registro, "E1") == 0) return &(contexto->e1);
    if (strcmp(nombre_registro, "E2") == 0) return &(contexto->e2);
    if (strcmp(nombre_registro, "E3") == 0) return &(contexto->e3);
    return NULL;
}

//Devuelve el valor de un operando: si es un registro, su contenido; si no, el número
static uint32_t obtener_valor(t_contexto* contexto, char* token) {
    uint32_t* registro = obtener_registro(contexto, token);
    if (registro) return *registro;
    return (uint32_t) atoi(token);
}

//Parseo y ejecución de instrucciones
t_tipo_instruccion decodificar_y_ejecutar(char* instruccion_str, t_contexto* contexto, bool* pc_modificado) {
    *pc_modificado = false;

    char** tokens = string_split(instruccion_str, " ");
    int cantidad = string_array_size(tokens);
    char* operacion = cantidad > 0 ? tokens[0] : "";
    t_tipo_instruccion resultado = INST_LENGUAJE_COMUN;

    if (strcmp(operacion, "NOOP") == 0) {
    }
    else if (strcmp(operacion, "SET") == 0 && cantidad == 3) {
        uint32_t* registro = obtener_registro(contexto, tokens[1]);
        if (registro) *registro = obtener_valor(contexto, tokens[2]);
    }
    else if (strcmp(operacion, "SUM") == 0 && cantidad == 3) {
        uint32_t* registro = obtener_registro(contexto, tokens[1]);
        if (registro) *registro += obtener_valor(contexto, tokens[2]);
    }
    else if (strcmp(operacion, "SUB") == 0 && cantidad == 3) {
        uint32_t* registro = obtener_registro(contexto, tokens[1]);
        if (registro) *registro -= obtener_valor(contexto, tokens[2]);
    }
    else if (strcmp(operacion, "JNZ") == 0 && cantidad == 3) {
        uint32_t* registro = obtener_registro(contexto, tokens[1]);
        if (registro && *registro != 0) {
            contexto->pc = (uint32_t) atoi(tokens[2]);
            *pc_modificado = true;
        }
    }
    else if (strcmp(operacion, "INIT_JOB") == 0 && cantidad == 2) {
        resultado = INST_INIT_JOB;
    }
    else if (strcmp(operacion, "EXIT") == 0) {
        resultado = INST_EXIT;
    }
    else {
        resultado = INST_INVALIDA;
    }

    string_array_destroy(tokens);
    return resultado;
}

char* obtener_parametro_archivo(char* instruccion_str) {
    char** tokens = string_split(instruccion_str, " ");
    char* archivo = strdup(tokens[1]);
    string_array_destroy(tokens);
    return archivo;
}