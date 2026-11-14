#ifndef PLANIFICADOR_SRC_STORAGE_CLIENTE_H_
#define PLANIFICADOR_SRC_STORAGE_CLIENTE_H_

#include <stdint.h>
#include <utils/paquete.h>  // t_resultado_checkpoint

// Todos los pedidos del Planificador al Storage. Igual que con la Placa, el
// socket es uno solo y lo comparten los hilos: cada pedido toma
// mutex_fd_storage durante el intercambio completo (mandar + respuesta). Si
// el Storage se cayó, el pedido siguiente vuelve a conectarse.

// Conecta y manda el Handshake. Si el Storage no está levantado, avisa y
// sigue: se reintenta en el primer pedido.
void storage_iniciar(char* ip, char* puerto);

t_resultado_checkpoint guardar_checkpoint_en_storage(char* nombre,
                                                     void* contenido,
                                                     uint32_t tamanio);
// Si sale bien, en *contenido queda lo que respondió el Storage (lo libera el
// que llama) y en *tamanio cuántos bytes son
t_resultado_checkpoint cargar_checkpoint_de_storage(char* nombre,
                                                    void** contenido,
                                                    uint32_t* tamanio);
t_resultado_checkpoint eliminar_checkpoint_en_storage(char* nombre);

#endif  // PLANIFICADOR_SRC_STORAGE_CLIENTE_H_
