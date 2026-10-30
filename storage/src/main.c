#include <commons/config.h>
#include <commons/log.h>
#include <commons/string.h>
#include <ctype.h>
#include <pthread.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utils/conexion.h>
#include <utils/paquete.h>
#include <utils/servidor.h>

#include "fs.h"

t_log* logger;

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
      default: {
        // Siempre se consume el buffer, si no el socket queda desfasado
        int size;
        free(recibir_buffer(&size, fd_conexion));
        log_warning(logger, "Operación desconocida: %d", operacion);
        break;
      }
    }
    operacion = recibir_operacion(fd_conexion);
  }
  cerrar_conexion(fd_conexion);
}

static int valor_hexa(char caracter) {
  if (isdigit((unsigned char)caracter)) return caracter - '0';
  caracter = toupper((unsigned char)caracter);
  if (caracter >= 'A' && caracter <= 'F') return caracter - 'A' + 10;
  return -1;
}

// WRITE <nombre> <hexa>: hasta 20 bytes, escritos como pares de dígitos
static void comando_write(char* nombre, char* hexa) {
  size_t largo = strlen(hexa);
  if (strlen(nombre) > 22 || largo == 0 || largo % 2 != 0 || largo > 40) {
    printf(
        "Uso: WRITE <nombre de hasta 22 letras> <de 1 a 20 bytes en "
        "hexa, ej. 0A1B2C>\n");
    return;
  }
  uint8_t bytes[20];
  for (size_t i = 0; i < largo / 2; i++) {
    int alto = valor_hexa(hexa[2 * i]);
    int bajo = valor_hexa(hexa[2 * i + 1]);
    if (alto < 0 || bajo < 0) {
      printf("WRITE: '%s' no es hexadecimal\n", hexa);
      return;
    }
    bytes[i] = alto * 16 + bajo;
  }
  if (fs_guardar(nombre, bytes, largo / 2) == FS_OK)
    printf("WRITE: %s guardado (%zu bytes)\n", nombre, largo / 2);
}

static void* consola(void* arg) {
  char* linea;
  while ((linea = readline("")) != NULL) {  // NULL = se cerró stdin
    char** tokens = string_split(linea, " ");
    int cantidad = string_array_size(tokens);
    if (cantidad > 0) string_to_upper(tokens[0]);  // el comando, no el nombre

    if (cantidad == 0 || strlen(tokens[0]) == 0) {
      // línea vacía
    } else if (strcmp(tokens[0], "INFO") == 0 && cantidad == 1) {
      fs_info();
    } else if (strcmp(tokens[0], "LS") == 0 && cantidad == 1) {
      fs_ls();
    } else if (strcmp(tokens[0], "CAT") == 0 && cantidad == 3) {
      fs_cat(tokens[1], atoi(tokens[2]));
    } else if (strcmp(tokens[0], "WRITE") == 0 && cantidad == 3) {
      comando_write(tokens[1], tokens[2]);
    } else if (strcmp(tokens[0], "RM") == 0 && cantidad == 2) {
      if (fs_eliminar(tokens[1]) == FS_OK)
        printf("RM: %s borrado\n", tokens[1]);
      else
        printf("RM: no existe %s\n", tokens[1]);
    } else {
      printf(
          "Comandos: INFO | LS | CAT <nombre> <bytes> | WRITE <nombre> "
          "<hexa> | RM <nombre>\n");
    }
    string_array_destroy(tokens);
    free(linea);
  }
  return NULL;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Uso: %s [Archivo Config]\n", argv[0]);
    return EXIT_FAILURE;
  }
  t_config* config = config_create(argv[1]);
  if (config == NULL) {
    fprintf(stderr, "No se pudo leer el config %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  logger = log_create(
      "storage.log", "Storage", 1,
      log_level_from_string(config_get_string_value(config, "LOG_LEVEL")));

  fs_iniciar(config);  // Check 3: monta o formatea el volumen

  // Sin esto, readline se queda con las señales: Ctrl+C y kill no cortan el
  // proceso mientras la consola espera una línea
  rl_catch_signals = 0;
  pthread_t hilo_consola;
  pthread_create(&hilo_consola, NULL, consola, NULL);
  pthread_detach(hilo_consola);

  int fd_servidor =
      iniciar_servidor(config_get_string_value(config, "PUERTO_ESCUCHA"));
  log_info(logger, "Servidor listo");
  escuchar_conexiones(fd_servidor, atender_conexion);  // no vuelve

  return EXIT_SUCCESS;
}
