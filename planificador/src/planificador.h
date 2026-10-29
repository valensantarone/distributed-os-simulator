#ifndef PLANIFICADOR_SRC_PLANIFICADOR_H_
#define PLANIFICADOR_SRC_PLANIFICADOR_H_

#include <commons/log.h>
#include <stdint.h>

// Lo que main.c comparte con placa_cliente.c y servicios.c

extern t_log* logger;

// BLOCKED -> READY y corto plazo. La usan los servicios al terminar y el
// hilo que atiende cada Page Fault. No llamarla con mutex_jobs tomado.
void desbloquear_job(uint32_t jid);

#endif  // PLANIFICADOR_SRC_PLANIFICADOR_H_
