#include "audio.h"
#include "pin_config.h"
#include "i18n.h"
#include "sdmon.h"
#include "voices_ptbr.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include <FS.h>
#include <SD_MMC.h>

// ---------------------------------------------------------------------------
// Audio del TamaPoke: códec ES8311 (DAC -> amplificador PA -> altavoz) por I2S.
// Init del ES8311 portado del driver oficial de Espressif (esp-bsp), fijado a
// MCLK=4.096MHz (256*fs), 16kHz, 16-bit, esclavo I2S. Los efectos son tonos
// cuadrados (estilo Game Boy) + clips de voz hablada en PT-BR sintetizados en
// una tarea aparte para no bloquear el loop de juego.
// ---------------------------------------------------------------------------

#define ES8311_ADDR 0x18
#define SAMPLE_RATE 16000

// Codigos en la cola de audio (uint16_t):
//   0 .. SFX_COUNT-1             -> efecto de tono cuadrado
//   0x0100 | voxId               -> frase hablada en flash (VOX_CLIPS)
//   0x0200 | dexNum (1..151)     -> nombre hablado de especie en SD (/mons/voices.bin)
#define AUDIO_TAG_VOX 0x0100
#define AUDIO_TAG_DEX 0x0200

static I2SClass i2s;
static bool gReady = false;
static bool gOn = true;
static bool gSleeping = false;
static QueueHandle_t gQ = nullptr;

// El NS4150B tarda bastante mas de 8 ms en estabilizarse tras cada apagado, asi
// que encenderlo justo antes de cada efecto se comia los cortos: el jingle de
// arranque (440 ms) se oia y los pitidos de 35 ms no. Se deja encendido mientras
// haya sonido y la mascota este despierta, como en el ejemplo verificado de
// Waveshare, y solo se apaga al dormir o al silenciar.
static void updateAmplifierPower() {
  // gReady evita encenderlo en una placa donde el codec no arranco: ahi el
  // amplificador solo aportaria siseo, porque no va a sonar nada.
  digitalWrite(PA, (gReady && gOn && !gSleeping) ? HIGH : LOW);
}

// ---- I2C del códec ----
static bool esW(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t esR(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ES8311_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

// Secuencia de init VERIFICADA EN ESTA PLACA (proyecto PlaneRadar2.0, misma
// Waveshare 1.75). Clave: reloj DERIVADO DEL BCLK (reg01=0xBF, sin MCLK externo)
// y referencia interna que alimenta el DAC (reg44=0x58); sin esos dos el codec
// respondia por I2C pero no salia audio. 16kHz, 16-bit, esclavo I2S.
static bool es8311Init() {
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) return false;

  // open()
  esW(0x0D, 0xFA); esW(0x44, 0x08); esW(0x44, 0x08);  // power up + quirk de 1a escritura
  esW(0x01, 0x30); esW(0x02, 0x00); esW(0x03, 0x10); esW(0x16, 0x24);
  esW(0x04, 0x10); esW(0x05, 0x00); esW(0x0B, 0x00); esW(0x0C, 0x00);
  esW(0x10, 0x1F); esW(0x11, 0x7F);
  esW(0x00, 0x80); esW(0x00, 0x80);                   // reset clock, esclavo
  esW(0x01, 0xBF);                                    // clk src = BCLK (sin MCLK externo)
  { uint8_t r = esR(0x06); r &= ~0x20; esW(0x06, r); }  // SCLK no invertido
  esW(0x13, 0x10); esW(0x1B, 0x0A); esW(0x1C, 0x6A);
  esW(0x44, 0x58);                                    // referencia interna -> alimenta el DAC

  // config_sample(): BCLK*8 = DIG_MCLK
  esW(0x02, 0x18); esW(0x05, 0x00); esW(0x03, 0x10); esW(0x04, 0x20);
  { uint8_t r = esR(0x07); r &= 0xC0; esW(0x07, r); }
  esW(0x08, 0xFF);
  { uint8_t r = esR(0x06); r &= 0xE0; r |= 0x03; esW(0x06, r); }  // bclk_div=4

  // formato I2S 16-bit
  esW(0x09, 0x0C); esW(0x0A, 0x0C);

  // start() DAC esclavo
  esW(0x00, 0x80); esW(0x01, 0xBF); esW(0x09, 0x0C); esW(0x0A, 0x0C);
  esW(0x17, 0xBF); esW(0x0E, 0x02); esW(0x12, 0x00); esW(0x14, 0x1A);
  esW(0x0D, 0x01); esW(0x15, 0x40); esW(0x37, 0x08); esW(0x45, 0x00);

  // volumen + unmute
  esW(0x32, 0xBF);                                    // volumen DAC ~0 dB
  { uint8_t r = esR(0x31); r &= 0x9F; esW(0x31, r); }  // unmute
  return true;
}

// ---- sintetizador de tono cuadrado ----
struct Note { uint16_t f, ms; };

static const Note N_TAP[]    = {{880, 35}};
static const Note N_EAT[]    = {{660, 45}, {0, 12}, {660, 45}};
static const Note N_PLAY[]   = {{784, 45}, {988, 60}};
static const Note N_HEART[]  = {{1047, 55}, {1319, 90}};
static const Note N_HATCH[]  = {{523, 80}, {659, 80}, {784, 110}, {1047, 170}};
static const Note N_EVOLVE[] = {{523, 80}, {659, 80}, {784, 80}, {1047, 90}, {1319, 230}};
static const Note N_MEDAL[]  = {{784, 70}, {0, 25}, {784, 70}, {0, 25}, {1047, 200}};
static const Note N_DENY[]   = {{300, 110}, {200, 170}};
static const Note N_BYE[]    = {{784, 150}, {659, 150}, {523, 280}};
static const Note N_LEVEL[]  = {{784, 70}, {1047, 130}};

struct SfxDef { const Note *n; uint8_t len; };
static const SfxDef SFX[SFX_COUNT] = {
  {N_TAP, 1}, {N_EAT, 3}, {N_PLAY, 2}, {N_HEART, 2}, {N_HATCH, 4},
  {N_EVOLVE, 5}, {N_MEDAL, 5}, {N_DENY, 2}, {N_BYE, 3}, {N_LEVEL, 2},
};

static int16_t buf[256 * 2];  // estéreo intercalado (L=R)

// reproduce un tono (o silencio si f==0) con rampa de ataque/caida anti-click
static void playTone(uint16_t f, uint16_t ms) {
  int total = SAMPLE_RATE * ms / 1000;
  int half = f ? (SAMPLE_RATE / (2 * f)) : 0;  // medio periodo en muestras
  const int16_t amp = 5000;
  int phase = 0, done = 0;
  bool high = true;
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
      int16_t s = 0;
      if (f) {
        s = high ? amp : -amp;
        int idx = done + i;
        if (idx < 64) s = (int16_t)(s * idx / 64);                 // ataque
        else if (idx > total - 96) s = (int16_t)(s * (total - idx) / 96);  // caida
        if (++phase >= half) { phase = 0; high = !high; }
      }
      buf[i * 2] = s; buf[i * 2 + 1] = s;
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

// reproduce un clip PCM de 8 bits con signo a 16 kHz desde Flash/PSRAM
static void playPcm8(const int8_t *pcm, uint32_t total) {
  uint32_t done = 0;
  while (done < total && gOn && !gSleeping) {
    uint32_t n = total - done;
    if (n > 256) n = 256;
    for (uint32_t i = 0; i < n; i++) {
      int16_t s = (int16_t)pcm[done + i] * 180;  // ganancia limpia sin saturar
      uint32_t idx = done + i;
      if (idx < 64) s = (int16_t)((int32_t)s * idx / 64);
      else if (idx > total - 96) s = (int16_t)((int32_t)s * (total - idx) / 96);
      buf[i * 2] = s;
      buf[i * 2 + 1] = s;
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

// reproduce el nombre de un Pokemon desde /mons/voices.bin en la SD (formato TPVX)
static void playSdDexVoice(uint16_t dexNum) {
  if (!sdReady || dexNum < 1 || dexNum > 151) return;
  File f = SD_MMC.open("/mons/voices.bin", FILE_READ);
  if (!f) return;
  uint8_t hdr[6];
  if (f.read(hdr, 6) != 6 || memcmp(hdr, "TPVX", 4) != 0) {
    f.close();
    return;
  }
  uint16_t count = hdr[4] | (hdr[5] << 8);
  if (dexNum > count) {
    f.close();
    return;
  }
  uint32_t entryOff = 6 + (uint32_t)(dexNum - 1) * 8;
  uint8_t meta[8];
  if (!f.seek(entryOff) || f.read(meta, 8) != 8) {
    f.close();
    return;
  }
  uint32_t off, len;
  memcpy(&off, meta, 4);
  memcpy(&len, meta + 4, 4);
  if (len == 0 || len > 64000 || !f.seek(off)) {
    f.close();
    return;
  }
  int8_t raw[256];
  uint32_t done = 0;
  while (done < len && gOn && !gSleeping) {
    uint32_t n = len - done;
    if (n > 256) n = 256;
    if (f.read((uint8_t *)raw, n) != n) break;
    for (uint32_t i = 0; i < n; i++) {
      int16_t s = (int16_t)raw[i] * 180;
      uint32_t idx = done + i;
      if (idx < 64) s = (int16_t)((int32_t)s * idx / 64);
      else if (idx > len - 96) s = (int16_t)((int32_t)s * (len - idx) / 96);
      buf[i * 2] = s;
      buf[i * 2 + 1] = s;
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
  f.close();
}

static void audioTask(void *) {
  uint16_t cmd;
  for (;;) {
    if (xQueueReceive(gQ, &cmd, portMAX_DELAY) && gOn && !gSleeping && gReady) {
      if (cmd & AUDIO_TAG_VOX) {
        uint8_t vid = cmd & 0xFF;
        if (vid < VOX_COUNT) {
          playTone(0, 25);  // breve pausa tras el efecto retro
          playPcm8(VOX_CLIPS[vid].data, VOX_CLIPS[vid].len);
        }
      } else if (cmd & AUDIO_TAG_DEX) {
        uint16_t dex = cmd & 0xFF;
        playTone(0, 30);
        playSdDexVoice(dex);
      } else if (cmd < SFX_COUNT) {
        const SfxDef &d = SFX[cmd];
        for (uint8_t i = 0; i < d.len; i++) playTone(d.n[i].f, d.n[i].ms);
      }
    }
  }
}

void audioBegin() {
  // I2S primero: arranca el MCLK que necesita el códec para engancharse
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);   // hasta saber si el sonido esta activado

  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S begin fallo");
    return;
  }
  if (!es8311Init()) { Serial.println("ES8311 no responde (audio off)"); return; }

  Preferences p;
  p.begin("tamapoke", true);
  gOn = p.getBool("snd", true);
  p.end();

  gReady = true;
  updateAmplifierPower();
  gQ = xQueueCreate(12, sizeof(uint16_t));
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 1, nullptr, 0);
  sfxPlay(SFX_HATCH);  // jingle de arranque (confirma que suena)
}

void sfxPlay(uint8_t id) {
  if (!gReady || !gOn || !gQ) return;
  uint16_t cmd = id;
  xQueueSend(gQ, &cmd, 0);  // descarta si la cola esta llena
}

void voxPlay(uint8_t id) {
  if (!gReady || !gOn || !gQ || id >= VOX_COUNT) return;
  if (gLang != LANG_PT_BR && gLang != LANG_PT) return;  // voces grabadas en portugues
  uint16_t cmd = AUDIO_TAG_VOX | id;
  xQueueSend(gQ, &cmd, 0);
}

void voxPlayDex(int16_t dexNum) {
  if (!gReady || !gOn || !gQ || dexNum < 1 || dexNum > 151) return;
  uint16_t cmd = AUDIO_TAG_DEX | (uint16_t)dexNum;
  xQueueSend(gQ, &cmd, 0);
}

void audioSetEnabled(bool on) {
  gOn = on;
  updateAmplifierPower();
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("snd", on);
  p.end();
}
bool audioEnabled() { return gOn; }

void audioSetSleeping(bool sleeping) {
  if (gSleeping == sleeping) return;
  gSleeping = sleeping;
  updateAmplifierPower();  // durmiendo no hay efectos: amp apagado, sin consumo
}
