#ifndef PLANIFICADOR_SRC_SERVICIOS_H_
#define PLANIFICADOR_SRC_SERVICIOS_H_

#include <commons/config.h>
#include <stdint.h>
#include <utils/paquete.h>

// Un pedido a un servicio: lo arma el hilo que atiende al Core con lo que llegó en SYSCALL_BLOQUEANTE
typedef struct {
  uint32_t jid;
  uint8_t tipo;      // t_syscall_bloqueante (SYS_LOAD_BATCH, SYS_LABEL, SYS_REPORT)
  char* nombre;      // archivo del Loader ("" si no aplica)
  uint32_t tamanio;  // bytes totales
  uint32_t cant_pares;
  t_par_fisico* pares;  // el rango ya traducido, un par por página
} t_pedido_servicio;

// Lee PATH_DATOS, RETARDO_LOADER y PATH_REPORTES, y lanza los 3 hilos
void servicios_iniciar(t_config* config);

// Encola en Loader (LOAD_BATCH), Labeler (LABEL) o Logger (REPORT)
void encolar_pedido_servicio(t_pedido_servicio* pedido);

void destruir_pedido_servicio(t_pedido_servicio* pedido);

#endif  // PLANIFICADOR_SRC_SERVICIOS_H_