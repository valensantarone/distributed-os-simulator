#ifndef CORE_SRC_EJECUCION_H_
#define CORE_SRC_EJECUCION_H_

#include <stdint.h>
#include <utils/paquete.h>

//Cómo terminó una instrucción que usa memoria o al Planificador (Check 3)
typedef enum {
  EJEC_OK,            // sigue el ciclo normal: PC + 1 y Check Interrupt
  EJEC_PAGE_FAULT,    // devolver el Job sin tocar el PC
  EJEC_JOB_DEVUELTO,  // el Job ya se le devolvió al Planificador
  EJEC_SIN_CONEXION   // se cayó la Placa o el Planificador
} t_resultado_ejecucion;

//FORWARD / BACKWARD / UPDATE
t_resultado_ejecucion ejecutar_entrenamiento(char* instruccion, uint32_t jid, t_contexto* contexto, uint32_t* pagina_faltante);

//Syscalls que no bloquean
t_resultado_ejecucion ejecutar_alloc(char* instruccion, uint32_t jid, t_contexto* contexto, int fd_planificador);
t_resultado_ejecucion ejecutar_free(char* instruccion, uint32_t jid, t_contexto* contexto, int fd_planificador);

// LOAD_BATCH, LABEL, REPORT (y los Checkpoints, que atiende el Check 4)
t_resultado_ejecucion ejecutar_syscall_bloqueante(
    char* instruccion,
    uint32_t jid,
    t_contexto* contexto,
    int fd_planificador,
    uint32_t* pagina_faltante);

#endif  // CORE_SRC_EJECUCION_H_