#ifndef PLANIFICADOR_SRC_CHECKPOINTS_H_
#define PLANIFICADOR_SRC_CHECKPOINTS_H_

#include "servicios.h"  // t_pedido_servicio

// SAVE_CHECKPOINT, LOAD_CHECKPOINT y DELETE_CHECKPOINT. El Job ya está
// BLOCKED; lanza un hilo que habla con la Placa y con el Storage y, al
// terminar, lo desbloquea o lo finaliza. Se queda con el pedido y lo libera.
void atender_checkpoint(t_pedido_servicio* pedido);

#endif  // PLANIFICADOR_SRC_CHECKPOINTS_H_