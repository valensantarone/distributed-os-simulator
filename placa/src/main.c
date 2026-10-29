#define _POSIX_C_SOURCE 200809L
#include <commons/collections/dictionary.h>
#include <commons/collections/list.h>
#include <commons/config.h>
#include <commons/log.h>
#include <commons/string.h>
#include <pthread.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utils/conexion.h>
#include <utils/paquete.h>
#include <utils/servidor.h>

#include "memoria.h"

t_log* logger;
t_config* config;
char* path_instrucciones;
int retardo_memoria = 0;  // ms que se espera antes de dar cada respuesta

t_dictionary* instrucciones_por_job;
pthread_mutex_t mutex_instrucciones = PTHREAD_MUTEX_INITIALIZER;

// Espera RETARDO_MEMORIA milisegundos (se llama antes de cada respuesta)
static void esperar_retardo_memoria(void) {
  struct timespec espera = {retardo_memoria / 1000,
                            (retardo_memoria % 1000) * 1000000L};
  nanosleep(&espera, NULL);
}

// Destructor para las listas de instrucciones en el diccionario
void destruir_lista_instrucciones(void* elemento) {
  t_list* lista = (t_list*)elemento;
  list_destroy_and_destroy_elements(lista, free);
}

// Helpers locales para responder paquetes usando las primitivas de utils
static void responder_ack(int fd_conexion) {
  t_paquete* paquete = crear_paquete(ACK);
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

static void responder_instruccion(int fd_conexion, const char* instruccion) {
  t_paquete* paquete = crear_paquete(INSTRUCCION);
  paquete->buffer->size = strlen(instruccion) + 1;
  paquete->buffer->stream = malloc(paquete->buffer->size);
  memcpy(paquete->buffer->stream, instruccion, paquete->buffer->size);
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

// ---------- Check 3: pedidos de memoria de usuario ----------

// Respuesta de un solo número (RESPUESTA_TAM_PAGINA, MARCO)
static void responder_numero(int fd_conexion, op_code codigo, uint32_t valor) {
  t_paquete* paquete = crear_paquete(codigo);
  agregar_a_paquete(paquete, &valor, sizeof(uint32_t));
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

// Campos de un pedido armado con agregar_a_paquete. NULL si se cortó la
// conexión o llegaron menos campos de los esperados.
static t_list* recibir_campos(int fd_conexion, int esperados) {
  int size;
  void* buffer = recibir_buffer(&size, fd_conexion);
  if (buffer == NULL) return NULL;
  t_list* campos = deserializar_paquete(buffer, size);
  free(buffer);
  if (list_size(campos) < esperados) {
    list_destroy_and_destroy_elements(campos, free);
    return NULL;
  }
  return campos;
}

static uint32_t campo_u32(t_list* campos, int indice) {
  return *(uint32_t*)list_get(campos, indice);
}

static void atender_tam_pagina(int fd_conexion) {
  int size;
  free(recibir_buffer(&size, fd_conexion));  // vacío, pero se consume igual
  responder_numero(fd_conexion, RESPUESTA_TAM_PAGINA, memoria_tam_pagina());
}

static void atender_obtener_marco(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 2);
  if (campos == NULL) return;
  int32_t marco =
      memoria_obtener_marco(campo_u32(campos, 0), campo_u32(campos, 1));
  list_destroy_and_destroy_elements(campos, free);
  esperar_retardo_memoria();
  responder_numero(fd_conexion, MARCO, (uint32_t)marco);  // -1 = Page Fault
}

static void atender_leer(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 3);
  if (campos == NULL) return;
  uint32_t jid = campo_u32(campos, 0);
  uint32_t dir_fisica = campo_u32(campos, 1);
  uint32_t tamanio = campo_u32(campos, 2);
  list_destroy_and_destroy_elements(campos, free);

  void* datos = calloc(1, tamanio);  // si el pedido es inválido van ceros
  memoria_leer(jid, dir_fisica, tamanio, datos);
  esperar_retardo_memoria();
  t_paquete* paquete = crear_paquete(DATOS_MEMORIA);
  paquete->buffer->size = tamanio;  // crudo: el buffer son los bytes
  paquete->buffer->stream = datos;  // eliminar_paquete lo libera
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

static void atender_escribir(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 4);
  if (campos == NULL) return;
  memoria_escribir(campo_u32(campos, 0), campo_u32(campos, 1),
                   campo_u32(campos, 2), list_get(campos, 3));
  list_destroy_and_destroy_elements(campos, free);
  esperar_retardo_memoria();
  responder_ack(fd_conexion);
}

static void atender_deslockear(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 1);
  if (campos == NULL) return;
  memoria_deslockear(campo_u32(campos, 0));
  list_destroy_and_destroy_elements(campos, free);
  responder_ack(fd_conexion);  // sin retardo: lo aclaró la cátedra (#5323)
}

static void atender_asignar(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 2);
  if (campos == NULL) return;
  uint32_t dir_logica = 0;
  uint8_t ok =
      memoria_asignar(campo_u32(campos, 0), campo_u32(campos, 1), &dir_logica);
  list_destroy_and_destroy_elements(campos, free);
  esperar_retardo_memoria();
  t_paquete* paquete = crear_paquete(RESPUESTA_ALLOC);
  agregar_a_paquete(paquete, &ok, sizeof(uint8_t));
  agregar_a_paquete(paquete, &dir_logica, sizeof(uint32_t));
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

static void atender_liberar(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 2);
  if (campos == NULL) return;
  memoria_liberar(campo_u32(campos, 0), campo_u32(campos, 1));
  list_destroy_and_destroy_elements(campos, free);
  esperar_retardo_memoria();
  responder_ack(fd_conexion);
}

static void atender_cargar_pagina(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 2);
  if (campos == NULL) return;
  memoria_cargar_pagina(campo_u32(campos, 0), campo_u32(campos, 1));
  list_destroy_and_destroy_elements(campos, free);
  esperar_retardo_memoria();
  responder_ack(fd_conexion);
}

static void atender_finalizar_job(int fd_conexion) {
  t_list* campos = recibir_campos(fd_conexion, 1);
  if (campos == NULL) return;
  uint32_t jid = campo_u32(campos, 0);
  list_destroy_and_destroy_elements(campos, free);

  memoria_finalizar_job(jid);
  char* jid_str = string_itoa(jid);
  pthread_mutex_lock(&mutex_instrucciones);
  t_list* instrucciones = dictionary_remove(instrucciones_por_job, jid_str);
  pthread_mutex_unlock(&mutex_instrucciones);
  if (instrucciones != NULL) destruir_lista_instrucciones(instrucciones);
  free(jid_str);

  log_debug(logger, "JID: %u - Estructuras del Job liberadas", jid);
  esperar_retardo_memoria();
  responder_ack(fd_conexion);
}

// Consola por stdin: INFO y TLS
static void* consola(void* arg) {
  char* linea;
  while ((linea = readline("")) != NULL) {  // NULL = se cerró stdin
    string_trim(&linea);
    string_to_upper(linea);
    if (strcmp(linea, "INFO") == 0) {
      memoria_info();
    } else if (strcmp(linea, "TLS") == 0) {
      memoria_tls();
    } else if (strlen(linea) > 0) {
      printf("Comando desconocido. Disponibles: INFO, TLS\n");
    }
    free(linea);
  }
  return NULL;
}

/**
 * Lee el archivo de pseudocódigo línea por línea.
 * Regla Crítica (0-based): la primera línea se guarda en la posición 0 de la
 * lista.
 */
static t_list* cargar_instrucciones_desde_archivo(const char* nombre_archivo) {
  char* path_completo;
  if (string_starts_with((char*)nombre_archivo, "/")) {
    path_completo = string_duplicate((char*)nombre_archivo);
  } else if (string_ends_with(path_instrucciones, "/")) {
    path_completo =
        string_from_format("%s%s", path_instrucciones, nombre_archivo);
  } else {
    path_completo =
        string_from_format("%s/%s", path_instrucciones, nombre_archivo);
  }

  FILE* archivo = fopen(path_completo, "r");
  if (archivo == NULL) {
    log_error(logger, "No se pudo abrir el archivo de pseudocódigo en: %s",
              path_completo);
    free(path_completo);
    return NULL;
  }

  t_list* lista_instrucciones = list_create();
  char* linea = NULL;
  size_t len = 0;
  ssize_t leidos;

  while ((leidos = getline(&linea, &len, archivo)) != -1) {
    size_t longitud = strlen(linea);
    while (longitud > 0 &&
           (linea[longitud - 1] == '\n' || linea[longitud - 1] == '\r')) {
      linea[longitud - 1] = '\0';
      longitud--;
    }
    list_add(lista_instrucciones, strdup(linea));
  }

  free(linea);
  fclose(archivo);

  log_info(logger, "Archivo cargado exitosamente: %s (%d instrucciones)",
           path_completo, list_size(lista_instrucciones));
  free(path_completo);

  return lista_instrucciones;
}

void atender_conexion(int fd_conexion) {
  int operacion = recibir_operacion(fd_conexion);

  while (operacion != -1) {
    switch (operacion) {
      case HANDSHAKE: {
        char* nombre_modulo = recibir_handshake(fd_conexion);
        log_info(logger, "## Módulo: %s", nombre_modulo);
        free(nombre_modulo);
        break;
      }

      case MENSAJE: {
        char* mensaje = recibir_mensaje(fd_conexion);
        log_info(logger, "Mensaje de prueba recibido: %s", mensaje);
        free(mensaje);
        break;
      }

      case CREAR_JOB: {
        int size;
        void* buffer = recibir_buffer(&size, fd_conexion);
        t_list* campos = deserializar_paquete(buffer, size);

        uint32_t jid = *(uint32_t*)list_get(campos, 0);
        char* nombre_archivo = (char*)list_get(campos, 1);

        t_list* instrucciones =
            cargar_instrucciones_desde_archivo(nombre_archivo);
        if (instrucciones == NULL) {
          instrucciones = list_create();
        }

        char* jid_str = string_itoa(jid);

        pthread_mutex_lock(&mutex_instrucciones);
        if (dictionary_has_key(instrucciones_por_job, jid_str)) {
          t_list* lista_anterior =
              dictionary_remove(instrucciones_por_job, jid_str);
          list_destroy_and_destroy_elements(lista_anterior, free);
        }
        dictionary_put(instrucciones_por_job, jid_str, instrucciones);
        pthread_mutex_unlock(&mutex_instrucciones);
        memoria_crear_job(jid);  // Check 3: tabla de páginas vacía

        free(jid_str);
        list_destroy_and_destroy_elements(campos, free);
        free(buffer);

        log_info(logger, "## JID: %u - Job Creado", jid);
        esperar_retardo_memoria();
        responder_ack(fd_conexion);
        break;
      }

      case FETCH_INSTRUCCION: {
        int size;
        void* buffer = recibir_buffer(&size, fd_conexion);

        uint32_t jid;
        uint32_t pc;

        if (size == sizeof(uint32_t) * 2) {
          memcpy(&jid, buffer, sizeof(uint32_t));
          memcpy(&pc, buffer + sizeof(uint32_t), sizeof(uint32_t));
        } else {
          t_list* campos = deserializar_paquete(buffer, size);
          jid = *(uint32_t*)list_get(campos, 0);
          pc = *(uint32_t*)list_get(campos, 1);
          list_destroy_and_destroy_elements(campos, free);
        }
        free(buffer);

        char* jid_str = string_itoa(jid);
        char* instruccion_a_enviar = NULL;

        pthread_mutex_lock(&mutex_instrucciones);
        t_list* lista = dictionary_get(instrucciones_por_job, jid_str);

        if (lista != NULL && pc < (uint32_t)list_size(lista)) {
          instruccion_a_enviar = strdup((char*)list_get(lista, pc));
        }
        pthread_mutex_unlock(&mutex_instrucciones);

        free(jid_str);

        esperar_retardo_memoria();

        if (instruccion_a_enviar != NULL) {
          log_info(logger,
                   "## JID: %u - Obtener instrucción: %u - Instrucción: %s",
                   jid, pc, instruccion_a_enviar);
          responder_instruccion(fd_conexion, instruccion_a_enviar);
          free(instruccion_a_enviar);
        } else {
          log_warning(logger,
                      "## No se encontró instrucción para JID: %u con PC: %u",
                      jid, pc);
          responder_instruccion(fd_conexion, "");
        }
        break;
      }

      case PEDIR_TAM_PAGINA:
        atender_tam_pagina(fd_conexion);
        break;
      case OBTENER_MARCO:
        atender_obtener_marco(fd_conexion);
        break;
      case LEER_MEMORIA:
        atender_leer(fd_conexion);
        break;
      case ESCRIBIR_MEMORIA:
        atender_escribir(fd_conexion);
        break;
      case DESLOCKEAR_PAGINAS:
        atender_deslockear(fd_conexion);
        break;
      case ASIGNAR_ESPACIO:
        atender_asignar(fd_conexion);
        break;
      case LIBERAR_ESPACIO:
        atender_liberar(fd_conexion);
        break;
      case CARGAR_PAGINA:
        atender_cargar_pagina(fd_conexion);
        break;
      case FINALIZAR_JOB:
        atender_finalizar_job(fd_conexion);
        break;

      default: {
        int size;
        free(recibir_buffer(&size, fd_conexion));
        log_warning(logger, "Operación no reconocida recibida: %d", operacion);
        break;
      }
    }

    operacion = recibir_operacion(fd_conexion);
  }

  log_info(logger, "Cliente desconectado (FD: %d)", fd_conexion);
  cerrar_conexion(fd_conexion);
}

int main(int argc, char* argv[]) {
  char* config_path = (argc > 1) ? argv[1] : "placa.config";
  config = config_create(config_path);
  if (config == NULL) {
    fprintf(stderr, "Error: no se pudo cargar la configuración desde %s\n",
            config_path);
    return EXIT_FAILURE;
  }

  char* log_level_str = config_get_string_value(config, "LOG_LEVEL");
  t_log_level level =
      log_level_from_string(log_level_str ? log_level_str : "INFO");
  logger = log_create("placa.log", "Placa", 1, level);

  path_instrucciones = config_get_string_value(config, "PATH_INSTRUCCIONES");
  if (path_instrucciones == NULL) {
    log_warning(
        logger,
        "PATH_INSTRUCCIONES no configurado en %s, usando './' por defecto",
        config_path);
    path_instrucciones = "./";
  }

  if (config_has_property(config, "RETARDO_MEMORIA")) {
    retardo_memoria = config_get_int_value(config, "RETARDO_MEMORIA");
  }

  instrucciones_por_job = dictionary_create();
  memoria_iniciar(config);  // Check 3: memoria de usuario + Offload

  // Sin esto, readline se queda con las señales: Ctrl+C y kill no cortan el
  // proceso mientras la consola espera una línea
  rl_catch_signals = 0;
  pthread_t hilo_consola;
  pthread_create(&hilo_consola, NULL, consola, NULL);
  pthread_detach(hilo_consola);

  char* puerto_escucha = config_get_string_value(config, "PUERTO_ESCUCHA");
  int fd_servidor = iniciar_servidor(puerto_escucha);

  log_info(
      logger,
      "Servidor Placa iniciado en puerto %s. Esperando Planificador y Cores...",
      puerto_escucha);

  escuchar_conexiones(fd_servidor, atender_conexion);

  dictionary_destroy_and_destroy_elements(instrucciones_por_job,
                                          destruir_lista_instrucciones);
  pthread_mutex_destroy(&mutex_instrucciones);
  config_destroy(config);
  log_destroy(logger);

  return EXIT_SUCCESS;
}