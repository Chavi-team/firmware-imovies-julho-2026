#ifndef _INTERFACE_H
#define _INTERFACE_H

/*
 * Enumeração dos possíveis estados que serão mostrados nos leds.
 */
enum Interface {
    UNSET = 0x00,                 // Nenhum estado definido.
    FADEIN_FADEOUT = 0x01,        // Fade in, fade out.
    ON_SOUND_TONESEED_OFF = 0x02, // Acende, som da interface, bip (timeToToneSeedSetup), apaga.
    ON_SOUND_TONEDEFAULT = 0x03,  // Acende, som da interface, bip (timeToToneDefault), apaga.
    ON = 0x04,                    // Acende.
    OFF = 0x05,                   // Apaga.
    ON_DELAY_OFF = 0x06,          // Acende, delay(500), apaga.
    ON_TONESEED_OFF = 0x07,       // Acende, bip(timeToToneSeedSetup), apaga.
    ON_TONESEED = 0x08            // Acende, bip(timeToToneSeedSetup).
};

#endif