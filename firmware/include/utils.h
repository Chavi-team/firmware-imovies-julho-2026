#ifndef UTILS_H
#define UTILS_H

#include <Arduino.h>


constexpr uint16_t RESET_INTERVAL = 20;

/// @brief Função de delay para ser usada dentro de interrupts.
/// Somente para manter compatibilidade.
/// Não deve ser usada em novas implementações.
/// @param ms tempo em milissegundos.
void delayInterrupt(uint16_t ms) {
    for (uint16_t i = 0; i < ms; i++) {
        delayMicroseconds(1000);
    }
}

#endif UTILS_H