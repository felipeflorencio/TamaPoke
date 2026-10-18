#!/usr/bin/env python3
"""Genera las voces en PT-BR para TamaPoke:
1. voices_ptbr.h: clips de voz para las acciones de la UI (PCM 8-bit con signo a 16 kHz, en PROGMEM).
2. tools/sdcard/mons/voices.bin: paquete opcional en la SD con los 151 nombres hablados en PT-BR (formato TPVX).

Uso:
  python3 tools/gen_voices.py          # genera voices_ptbr.h
  python3 tools/gen_voices.py --sd     # genera tambien tools/sdcard/mons/voices.bin
"""
import argparse
import os
import shutil
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_H = os.path.join(ROOT, 'voices_ptbr.h')
OUT_SD = os.path.join(HERE, 'sdcard', 'mons', 'voices.bin')

SAMPLE_RATE = 16000
VOICE = 'Luciana'
RATE_WPM = '210'

# (enum_name, phrase_pt_br)
UI_VOICES = [
    ('VOX_FEED',       'Alimentar!'),
    ('VOX_BERRY_R',    'Fruta vermelha!'),
    ('VOX_BERRY_B',    'Fruta azul!'),
    ('VOX_BERRY_G',    'Fruta verde!'),
    ('VOX_CANDY',      'Doce!'),
    ('VOX_LIKES',      'Ele adorou!'),
    ('VOX_PLAY',       'Brincar!'),
    ('VOX_SLEEP',      'Hora de dormir!'),
    ('VOX_WAKE',       'Acordou!'),
    ('VOX_BATH',       'Hora do banho!'),
    ('VOX_TRAIN',      'Treinar força!'),
    ('VOX_PET',        'Carinho!'),
    ('VOX_HATCH',      'Nasceu!'),
    ('VOX_EVO_READY',  'Pronto para evoluir!'),
    ('VOX_EVOLVING',   'Evoluindo!'),
    ('VOX_KEEP_FORM',  'Manter forma!'),
    ('VOX_FAREWELL_Q', 'Hora da despedida?'),
    ('VOX_FAREWELL',   'Obrigado! Até sempre!'),
    ('VOX_STAY',       'Ficar juntos!'),
    ('VOX_RUNAWAY',    'Ele fugiu...'),
    ('VOX_RELEASE_Q',  'Soltar Pokémon?'),
    ('VOX_GOODBYE',    'Tchau!'),
    ('VOX_LEVEL_UP',   'Subiu de nível!'),
    ('VOX_MEDAL',      'Nova medalha!'),
]


def synth_pcm8(text, tmpdir):
    """Sintetiza `text` en PT-BR y devuelve bytes PCM 8-bit signed @ 16kHz mono."""
    aiff_path = os.path.join(tmpdir, 'tts.aiff')
    raw_path = os.path.join(tmpdir, 'tts.s8')
    subprocess.run(
        ['say', '-v', VOICE, '-r', RATE_WPM, '-o', aiff_path, text],
        check=True,
    )
    af_filter = (
        'silenceremove=start_periods=1:start_silence=0.02:start_threshold=-45dB,'
        'areverse,'
        'silenceremove=start_periods=1:start_silence=0.04:start_threshold=-45dB,'
        'areverse,'
        'dynaudnorm=f=75:g=7:p=0.92'
    )
    subprocess.run(
        [
            'ffmpeg', '-y', '-loglevel', 'error',
            '-i', aiff_path,
            '-af', af_filter,
            '-ar', str(SAMPLE_RATE),
            '-ac', '1',
            '-f', 's8',
            raw_path,
        ],
        check=True,
    )
    with open(raw_path, 'rb') as fh:
        return fh.read()


def emit_header():
    if not shutil.which('say') or not shutil.which('ffmpeg'):
        raise SystemExit('Se necesita macOS `say` y `ffmpeg` para regenerar las voces.')
    tmpdir = tempfile.mkdtemp(prefix='tamapoke-vox-')
    try:
        clips = []
        total_bytes = 0
        for enum_name, phrase in UI_VOICES:
            pcm = synth_pcm8(phrase, tmpdir)
            clips.append((enum_name, phrase, pcm))
            total_bytes += len(pcm)
            print(f'  {enum_name:16s} "{phrase}" -> {len(pcm):5d} B ({len(pcm) / SAMPLE_RATE:.2f}s)')

        lines = [
            '#pragma once',
            '#include <stdint.h>',
            '#include <Arduino.h>',
            '#ifndef PROGMEM',
            '#define PROGMEM',
            '#endif',
            '',
            '// GENERADO por tools/gen_voices.py - no editar a mano',
            f'// {len(clips)} clips de voz PT-BR @ {SAMPLE_RATE} Hz, 8-bit signed PCM ({total_bytes // 1024} KB total)',
            '',
            'struct VoxClip {',
            '  const int8_t *data;',
            '  uint32_t len;',
            '};',
            '',
        ]

        for enum_name, phrase, pcm in clips:
            arr_name = f'DATA_{enum_name}'
            lines.append(f'// "{phrase}" ({len(pcm)} bytes)')
            lines.append(f'static const int8_t {arr_name}[{len(pcm)}] PROGMEM = {{')
            # 24 samples per line
            signed_vals = [struct.unpack('b', bytes([b]))[0] for b in pcm]
            for i in range(0, len(signed_vals), 24):
                chunk = ','.join(str(v) for v in signed_vals[i:i + 24])
                lines.append(f'  {chunk},')
            lines.append('};')
            lines.append('')

        lines.append(f'static const VoxClip VOX_CLIPS[{len(clips)}] = {{')
        for enum_name, _phrase, pcm in clips:
            lines.append(f'  {{ DATA_{enum_name}, {len(pcm)} }},')
        lines.append('};')
        lines.append('')

        with open(OUT_H, 'w', encoding='utf-8') as out:
            out.write('\n'.join(lines))
        print(f'OK -> {OUT_H} ({total_bytes / 1024:.1f} KB)')
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def emit_sd_bundle():
    """Genera tools/sdcard/mons/voices.bin (TPVX) con los 151 nombres hablados."""
    from dex_data import DEX
    tmpdir = tempfile.mkdtemp(prefix='tamapoke-sdvox-')
    try:
        blobs = []
        for row in DEX:
            num, _slug, display = row[0], row[1], row[2]
            spoken = display.title().replace('Nidoran H', 'Nidoran fêmea').replace('Nidoran M', 'Nidoran macho').replace('Mr. Mime', 'Mister Mime').replace('Farfetchd', 'Farfetch')
            pcm = synth_pcm8(spoken, tmpdir)
            blobs.append(pcm)
            if num % 25 == 0 or num == 151:
                print(f'  SD voz #{num:03d} {spoken} ({len(pcm)} B)')

        count = len(blobs)
        header_size = 6 + count * 8  # "TPVX" (4) + count (2) + count * (offset uint32, len uint32)
        offsets = []
        pos = header_size
        for b in blobs:
            offsets.append((pos, len(b)))
            pos += len(b)

        os.makedirs(os.path.dirname(OUT_SD), exist_ok=True)
        with open(OUT_SD, 'wb') as f:
            f.write(b'TPVX')
            f.write(struct.pack('<H', count))
            for off, length in offsets:
                f.write(struct.pack('<II', off, length))
            for b in blobs:
                f.write(b)
        print(f'OK -> {OUT_SD} ({pos / 1024:.1f} KB, {count} especies)')
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sd', action='store_true', help='Genera tambien tools/sdcard/mons/voices.bin (151 especies)')
    args = ap.parse_args()
    emit_header()
    if args.sd:
        emit_sd_bundle()


if __name__ == '__main__':
    main()
