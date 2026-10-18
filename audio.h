#pragma once
#include <stdint.h>

// Efectos de sonido del juego (cola, no bloqueante). El orden coincide con la
// tabla SFX de audio.cpp.
enum Sfx : uint8_t {
  SFX_TAP = 0,  // tocar / boton
  SFX_EAT,      // comer
  SFX_PLAY,     // punto del minijuego / golpe
  SFX_HEART,    // le gusta / mimo
  SFX_HATCH,    // eclosion
  SFX_EVOLVE,   // evolucion
  SFX_MEDAL,    // medalla / hito
  SFX_DENY,     // accion no permitida
  SFX_BYE,      // despedida
  SFX_LEVEL,    // sube de nivel
  SFX_COUNT
};

// Voces habladas en PT-BR (el orden coincide con VOX_CLIPS en voices_ptbr.h)
enum VoiceId : uint8_t {
  VOX_FEED = 0,
  VOX_BERRY_R,
  VOX_BERRY_B,
  VOX_BERRY_G,
  VOX_CANDY,
  VOX_LIKES,
  VOX_PLAY,
  VOX_SLEEP,
  VOX_WAKE,
  VOX_BATH,
  VOX_TRAIN,
  VOX_PET,
  VOX_HATCH,
  VOX_EVO_READY,
  VOX_EVOLVING,
  VOX_KEEP_FORM,
  VOX_FAREWELL_Q,
  VOX_FAREWELL,
  VOX_STAY,
  VOX_RUNAWAY,
  VOX_RELEASE_Q,
  VOX_GOODBYE,
  VOX_LEVEL_UP,
  VOX_MEDAL,
  VOX_COUNT
};

void audioBegin();          // init ES8311 + I2S + amplificador + tarea de audio
void sfxPlay(uint8_t id);   // encola un efecto (no bloquea el loop)
void voxPlay(uint8_t id);   // encola una frase hablada en PT-BR (no bloquea el loop)
void voxPlayDex(int16_t dexNum);  // encola el nombre del Pokemon desde la SD (voices.bin)
void audioSetEnabled(bool on);
bool audioEnabled();
void audioSetSleeping(bool sleeping);  // dormida: amplificador apagado
