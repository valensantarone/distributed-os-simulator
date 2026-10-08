#define _POSIX_C_SOURCE 200809L
#include <commons/collections/dictionary.h>
#include <commons/collections/list.h>
#include <commons/config.h>
#include <commons/log.h>
#include <commons/string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utils/conexion.h>
#include <utils/paquete.h>
#include <utils/servidor.h>

t_log* logger;
t_config* config;
char* path_instrucciones;

t_dictionary* instrucciones_por_job;
pthread_mutex_t mutex_instrucciones = PTHREAD_MUTEX_INITIALIZER;

// Destructor para las listas de instrucciones en el diccionario
void destruir_lista_instrucciones(void* elemento) {
  t_list* lista = (t_list*)elemento;
  list_destroy_and_destroy_elements(lista, free);
}

// Helpers locales para responder paquetes usando las primitivas existentes de
// utils
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

/**
 * Lee el archivo de pseudocódigo línea por línea.
 * Regla Crítica (0-based): la primera línea se guarda en la posición 0 de la
 * lista.
 */
static t_list* cargar_instrucciones_desde_archivo(const char* nombre_archivo) {
  char* path_completo;
  if (string_ends_with(path_instrucciones, "/")) {
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
    // Quitar saltos de línea al final (\r y \n)
    size_t longitud = strlen(linea);
    while (longitud > 0 &&
           (linea[longitud - 1] == '\n' || linea[longitud - 1] == '\r')) {
      linea[longitud - 1] = '\0';
      longitud--;
    }
    // Primer elemento ingresa en el índice 0 (0-based)
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

        free(jid_str);
        list_destroy_and_destroy_elements(campos, free);
        free(buffer);

        // Log obligatorio de Creación de Job y confirmación con ACK
        log_info(logger, "## JID: %u - Job Creado", jid);
        responder_ack(fd_conexion);
        break;
      }

      case FETCH_INSTRUCCION: {
        int size;
        void* buffer = recibir_buffer(&size, fd_conexion);

        uint32_t jid;
        uint32_t pc;

        // Soporta tanto paquete empaquetado por campos como 2 uint32_t contiguos
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

        // Regla 0-based: pc = 0 busca en lista[0]
        if (lista != NULL && pc < (uint32_t)list_size(lista)) {
          instruccion_a_enviar = strdup((char*)list_get(lista, pc));
        }
        pthread_mutex_unlock(&mutex_instrucciones);

        free(jid_str);

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

      default: {
        // Siempre se consume el buffer, si no el socket queda desfasado
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

  instrucciones_por_job = dictionary_create();

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