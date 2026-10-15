"""Compile the PC launcher's graphics fixes for the prepared UWP guest image."""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import patches


def prepare(out):
    analysis = json.loads((out / 'analysis.json').read_text(encoding='utf-8'))
    sfo = analysis['sfo']
    profile = patches.game_profile(sfo.get('TITLE_ID'), sfo.get('TITLE'))
    if not profile:
        raise ValueError('No graphics patch profile for the prepared game')
    _, xml, version, _ = profile
    elf = (out / 'eboot.elf').read_bytes()
    segments, image = patches.eboot_segments(elf), patches.eboot_image(elf)
    report = {}
    for resolution in ('native', '720p'):
        extra = '' if resolution == 'native' else 'Resolution Patch - 720p'
        names = patches.selected_patches(xml, version, extra)
        skipped = {}
        writes = patches.compile_patches(xml, names, None, segments, image, skipped)
        if skipped:
            raise ValueError(f'{resolution}: required graphics patches unavailable: {skipped}')
        patches.write_patches(out, writes)
        (out / 'patches.bin').replace(out / f'patches-{resolution}.bin')
        dmem, vblank = patches.patch_requirements(xml, names, version)
        report[resolution] = {'applied': names, 'writes': len(writes),
                              'dmem_mb': dmem, 'vblank_hz': vblank or 60}
        print(f'UWP graphics {resolution}: {len(writes)} writes, DMEM {dmem} MiB; {names}')
    (out / 'graphics-patches.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    prepare(parser.parse_args().out)
