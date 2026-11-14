#ifndef PLANIFICADOR_SRC_PLANIFICADOR_H_
#define PLANIFICADOR_SRC_PLANIFICADOR_H_

#include <commons/log.h>
#include <stdint.h>

// Lo que main.c comparte con placa_cliente.c, servicios.c, checkpoints.c y
// storage_cliente.c

extern t_log* logger;

// BLOCKED -> READY y corto plazo. La usan los servicios al terminar y el
// hilo que atiende cada Page Fault. No llamarla con mutex_jobs tomado.
void desbloquear_job(uint32_t jid);

// Check 4: BLOCKED -> EXIT. La usa checkpoints.c cuando el Storage no pudo
// guardar o cargar. No llamarla con mutex_jobs tomado.
void finalizar_job_bloqueado(uint32_t jid, char* motivo);

#endif  // PLANIFICADOR_SRC_PLANIFICADOR_H_
