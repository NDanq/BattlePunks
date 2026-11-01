#!/usr/bin/env python3
"""
fetch_cc0.py -- downloads Demo Lab's CC0 audio sources (music, nature
ambience, some foley) into an empty directory, after checking each source page
still says CC0. Nothing downloaded is ever executed: the files are read as
audio by build_audio.py (through ffmpeg) and nothing else.

    python Games/DemoLab/Tools/Audio/fetch_cc0.py <empty-dir>
    python Games/DemoLab/Tools/Audio/build_audio.py --cc0 <that-dir>

Every source is cited in Docs/Credits.md. Kenney packs are zips (unpacked
with the stdlib's zipfile, audio members only); OpenGameArt files are fetched
directly. No account or sign-in is needed for any of them.
"""

import hashlib
import io
import json
import re
import sys
import urllib.request
import zipfile
from pathlib import Path

UA = {"User-Agent": "Mozilla/5.0 (DemoLab audio fetch)"}

# id -> (source page, file URL or kenney pack slug, author, title)
OGA = "https://opengameart.org/content/"
OGF = "https://opengameart.org/sites/default/files/"
SOURCES = {
    # Music
    "MusicMenu":        (OGA + "unexplored-expansion", OGF + "tad_-_unexplored_-_expanded_1_by_bo_jingles.mp3",
                         "Bo Jingles (from TAD's 'Unexplored - short')", "Unexplored Expansion"),
    "MusicExplore":     (OGA + "factory-ambiance", OGF + "Factory.ogg", "yd", "Factory ambiance"),
    "MusicCombat":      (OGA + "determined-pursuit-epic-orchestra-loop", OGF + "determined_pursuit_loop.wav",
                         "Emma_MA", "Determined Pursuit (epic orchestra loop)"),
    "MusicExtraction":  (OGA + "epic-endgame-cinematic", OGF + "ChoirChordsBassMaster.wav", "cynicmusic",
                         "Epic Endgame Cinematic"),
    # Nature
    "Wave01": (OGA + "beach-ocean-waves", OGF + "wave_01_cc0-18363__jasinski__alkaibeach.flac", "jasinski (extracted by qubodup)", "Beach Ocean Waves"),
    "Wave02": (OGA + "beach-ocean-waves", OGF + "wave_02_cc0-18363__jasinski__alkaibeach.flac", "jasinski (extracted by qubodup)", "Beach Ocean Waves"),
    "Wave03": (OGA + "beach-ocean-waves", OGF + "wave_03_cc0-18363__jasinski__alkaibeach.flac", "jasinski (extracted by qubodup)", "Beach Ocean Waves"),
    "Wave04": (OGA + "beach-ocean-waves", OGF + "wave_04_cc0-18363__jasinski__alkaibeach.flac", "jasinski (extracted by qubodup)", "Beach Ocean Waves"),
    "ParkWind":  (OGA + "park-ambiences", OGF + "park_ambience_wind.wav", "Thimras", "Park ambiences (wind)"),
    "ParkBirds": (OGA + "park-ambiences", OGF + "park_ambience_birds.wav", "Thimras", "Park ambiences (birds)"),
    "TreeCreak": (OGA + "tree-creaking", OGF + "tree_creak.flac", "Department64 (cleaned by AntumDeluge)", "Tree Creaking"),
    # Foley packs
    "KenneyImpact":    ("https://kenney.nl/assets/impact-sounds", "kenney:impact-sounds", "Kenney", "Impact Sounds"),
    "KenneyInterface": ("https://kenney.nl/assets/interface-sounds", "kenney:interface-sounds", "Kenney", "Interface Sounds"),
    "KenneyRpg":       ("https://kenney.nl/assets/rpg-audio", "kenney:rpg-audio", "Kenney", "RPG Audio"),
}


def get(url, headers=None, cookies=None):
    req = urllib.request.Request(url, headers={**UA, **(headers or {})})
    if cookies:
        req.add_header("Cookie", cookies)
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read(), r.headers


def check_cc0(page_html, page_url):
    """The licence field of the page must name CC0 and nothing else."""
    if "kenney.nl" in page_url:
        if "Creative Commons CC0" not in page_html:
            raise SystemExit(f"not CC0: {page_url}")
        return "CC0 (Kenney licence field: Creative Commons CC0)"
    m = re.search(r"field-name-field-art-licenses.{0,700}", page_html, re.S)
    text = re.sub(r"\s+", " ", re.sub("<[^>]+>", " ", m.group(0))) if m else ""
    lic = re.search(r"License\(s\):&nbsp;(.*?)(Collections|Uploader|$)", text)
    lic = lic.group(1).strip() if lic else ""
    if lic != "CC0":
        raise SystemExit(f"licence field is '{lic}', not CC0: {page_url}")
    return "CC0 (OpenGameArt licence field)"


def kenney_zip(page_url):
    raw, hdrs = get(page_url)
    cookies = "; ".join(c.split(";")[0] for c in hdrs.get_all("Set-Cookie") or [])
    html = raw.decode("utf8", "ignore")
    check_cc0(html, page_url)
    dl_id = re.search(r"data-download-id='([0-9a-f]+)'", html).group(1)
    link, _ = get("https://kenney.nl/download-link/" + dl_id, {"Referer": page_url}, cookies)
    data, _ = get(json.loads(link)["url"], {"Referer": page_url}, cookies)
    return data


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    manifest = {}
    pages = {}
    for key, (page, url, author, title) in SOURCES.items():
        if page not in pages:
            pages[page] = get(page)[0].decode("utf8", "ignore")
        licence = check_cc0(pages[page], page)
        if url.startswith("kenney:"):
            data = kenney_zip(page)
            dst = out / key
            dst.mkdir(exist_ok=True)
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                for name in z.namelist():
                    if name.lower().endswith((".ogg", ".wav", ".flac")) and ".." not in name:
                        (dst / Path(name).name).write_bytes(z.read(name))
        else:
            data, _ = get(url)
            (out / (key + Path(url).suffix.lower())).write_bytes(data)
        manifest[key] = {"page": page, "file": url, "author": author, "title": title, "licence": licence,
                         "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
        print(f"  {key:16s} {len(data) / 1e6:6.1f} MB  {licence}")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
