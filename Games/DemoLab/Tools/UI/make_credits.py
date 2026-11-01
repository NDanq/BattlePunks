#!/usr/bin/env python3
"""
Writes Content/UI/Credits.lua, the credits screen's text, from Docs/Credits.md:
the team lines below, then every sound and music entry the doc lists (its
tables' rows: the asset, its source, author and licence).

    python3 Tools/UI/make_credits.py        (from Games/DemoLab)

Run it whenever Docs/Credits.md changes (Tools/Audio adds rows there).
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCE = os.path.join(ROOT, 'Docs', 'Credits.md')
TARGET = os.path.join(ROOT, 'Content', 'UI', 'Credits.lua')

HEADER = [
    'A NimbleEngine showcase',
    '',
    'DESIGN, CODE AND LEVELS',
    'The NimbleEngine team, with Claude',
    '',
    'GAMEPLAY',
    'Ported from BattlePunks: player, combat, AI, encounters, story mode',
    '',
    'CHARACTERS',
    'The SteamDummy, skinned to the CrashDummy rig',
    '',
    'ENGINE',
    'NimbleEngine: Vulkan renderer, DDGI, Jolt Physics, Recast navigation, Sequencer, UI',
    'Plugins: Time Of Day, Weather',
]


def plain(text):
    """Markdown to plain text: links to their label, no emphasis or code marks."""
    text = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', text)
    return text.replace('**', '').replace('`', '').replace('*', '').strip()


def read_credits():
    lines = []
    if not os.path.exists(SOURCE):
        return lines
    heading = None
    columns = []
    for raw in open(SOURCE, encoding='utf-8'):
        line = raw.rstrip('\n')
        if line.startswith('#'):
            heading = plain(line.lstrip('#'))
            columns = []
            continue
        if not line.startswith('|'):
            continue
        cells = [plain(c) for c in line.strip().strip('|').split('|')]
        if all(set(c) <= set('-: ') for c in cells):
            continue
        if not columns:
            columns = cells
            if heading and heading.lower() not in ('demo lab credits', 'credits'):
                lines += ['', heading.upper()]
            continue
        lines.append('   '.join(c for c in cells if c))
    return lines


def lua_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    body = HEADER + read_credits() + ['', 'Thank you for playing.']
    out = ['-- Demo Lab\'s credits (Credits.nmui\'s Script). Written by Tools/UI/make_credits.py',
           '-- from Docs/Credits.md: edit that, then rerun the script.',
           '',
           'LabCredits = LabCredits or {}',
           '',
           'LabCredits.Text = table.concat({']
    out += [f'    {lua_string(line)},' for line in body]
    out += ['}, "\\n")', '',
            'function LabCredits.Show()',
            '    UI.SetString("Credits.Text", LabCredits.Text)',
            'end', '']
    with open(TARGET, 'w', encoding='ascii', errors='replace') as f:
        f.write('\n'.join(out))
    print(f'Credits.lua: {len(body)} lines')


if __name__ == '__main__':
    main()
