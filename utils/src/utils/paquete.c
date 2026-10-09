#include "paquete.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

t_paquete* crear_paquete(op_code codigo) {
  t_paquete* paquete = malloc(sizeof(t_paquete));
  paquete->codigo_operacion = codigo;
  paquete->buffer = malloc(sizeof(t_buffer));
  paquete->buffer->size = 0;
  paquete->buffer->stream = NULL;
  return paquete;
}

void agregar_a_paquete(t_paquete* paquete, void* valor, int tamanio) {
  paquete->buffer->stream =
      realloc(paquete->buffer->stream,
              paquete->buffer->size + tamanio + sizeof(uint32_t));
  uint32_t tam = (uint32_t)tamanio;
  memcpy(paquete->buffer->stream + paquete->buffer->size, &tam,
         sizeof(uint32_t));
  memcpy(paquete->buffer->stream + paquete->buffer->size + sizeof(uint32_t),
         valor, tamanio);
  paquete->buffer->size += tamanio + sizeof(uint32_t);
}

static void* serializar_paquete(t_paquete* paquete, int* bytes_totales) {
  *bytes_totales = sizeof(uint8_t) + sizeof(uint32_t) + paquete->buffer->size;
  void* magic = malloc(*bytes_totales);
  int desplazamiento = 0;
  memcpy(magic + desplazamiento, &(paquete->codigo_operacion), sizeof(uint8_t));
  desplazamiento += sizeof(uint8_t);
  memcpy(magic + desplazamiento, &(paquete->buffer->size), sizeof(uint32_t));
  desplazamiento += sizeof(uint32_t);
  memcpy(magic + desplazamiento, paquete->buffer->stream,
         paquete->buffer->size);
  return magic;
}

t_list* deserializar_paquete(void* buffer, int size) {
  t_list* campos = list_create();
  int desplazamiento = 0;
  while (desplazamiento < size) {
    uint32_t tam_campo;
    memcpy(&tam_campo, buffer + desplazamiento, sizeof(uint32_t));
    desplazamiento += sizeof(uint32_t);
    void* campo = malloc(tam_campo);
    memcpy(campo, buffer + desplazamiento, tam_campo);
    desplazamiento += tam_campo;
    list_add(campos, campo);
  }
  return campos;
}

void enviar_paquete(t_paquete* paquete, int fd_conexion) {
  int bytes;
  void* a_enviar = serializar_paquete(paquete, &bytes);
  send(fd_conexion, a_enviar, bytes, 0);
  free(a_enviar);
}

void eliminar_paquete(t_paquete* paquete) {
  free(paquete->buffer->stream);
  free(paquete->buffer);
  free(paquete);
}

int recibir_operacion(int fd_conexion) {
  uint8_t codigo;
  if (recv(fd_conexion, &codigo, sizeof(uint8_t), MSG_WAITALL) > 0)
    return codigo;
  return -1;
}

void* recibir_buffer(int* size, int fd_conexion) {
  uint32_t tam;
  *size = 0;
  if (recv(fd_conexion, &tam, sizeof(uint32_t), MSG_WAITALL) <= 0) return NULL;
  void* buffer = malloc(tam);
  if (tam > 0 && recv(fd_conexion, buffer, tam, MSG_WAITALL) <= 0) {
    free(buffer);
    return NULL;
  }
  *size = tam;
  return buffer;
}

static void enviar_string(int fd_conexion, op_code codigo, char* valor) {
  t_paquete* paquete = crear_paquete(codigo);
  paquete->buffer->size = strlen(valor) + 1;
  paquete->buffer->stream = malloc(paquete->buffer->size);
  memcpy(paquete->buffer->stream, valor, paquete->buffer->size);
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

void enviar_handshake(int fd_conexion, char* nombre_modulo) {
  enviar_string(fd_conexion, HANDSHAKE, nombre_modulo);
}

void enviar_mensaje(int fd_conexion, char* mensaje) {
  enviar_string(fd_conexion, MENSAJE, mensaje);
}

char* recibir_handshake(int fd_conexion) {
  int size;
  return recibir_buffer(
      &size, fd_conexion);  // ya viene null-terminated (se mandó con strlen+1)
}

char* recibir_mensaje(int fd_conexion) {
  int size;
  return recibir_buffer(
      &size,
      fd_conexion);  // mismo formato que el Handshake, solo cambia el opcode
}

void enviar_ack(int fd_conexion) {
  t_paquete* paquete = crear_paquete(ACK);
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

bool recibir_ack(int fd_conexion) {
  if (recibir_operacion(fd_conexion) != ACK) return false;
  int size;
  free(recibir_buffer(&size, fd_conexion));
  return true;
}

void enviar_respuesta_interrupcion(int fd_conexion, uint8_t desalojar) {
  t_paquete* paquete = crear_paquete(RESPUESTA_INTERRUPCION);
  paquete->buffer->size = sizeof(uint8_t);
  paquete->buffer->stream = malloc(sizeof(uint8_t));
  memcpy(paquete->buffer->stream, &desalojar, sizeof(uint8_t));
  enviar_paquete(paquete, fd_conexion);
  eliminar_paquete(paquete);
}

void enviar_instruccion(int fd_conexion, char* instruccion) {
  enviar_string(fd_conexion, INSTRUCCION, instruccion);
}
