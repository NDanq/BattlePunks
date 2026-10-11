"""The SteampunkFlyover's director: a script graph that runs the cinematic.

Writes
  Scripts/CinematicDirector.script.nmgraph      the graph (no <gen_code>: the
                                                editor compiles it on open, or
                                                run the `graphs` console command)
  Content/Prefabs/CinematicDirector.nmprefab    an empty entity carrying it
  Content/Levels/Steampunk/SteampunkStreet/<guid>.nmprefabinst  one placed in the level

The graph shows the Sequence library from a script graph:
  On Start          -> subtitles on, start the level's "Cinematic" player
  On Event          "Flyover.Shot" (the sequence's Event track) -> log the cut
  On Sequence Finished (library event) -> log it, emit "Director.Done"
  On Event          "Flyover.Done" (fire-on-skip key) -> log the hand-back

Run make_steampunk_flyover.py first: it writes the "Cinematic" player with
Auto Play off, so this graph is what starts it.
"""

from pathlib import Path

GAME   = Path(__file__).resolve().parents[2]
GRAPH  = GAME / "Scripts" / "CinematicDirector.script.nmgraph"
PREFAB = GAME / "Content" / "Prefabs" / "CinematicDirector.nmprefab"
FOLDER = GAME / "Content" / "Levels" / "Steampunk" / "SteampunkStreet"   # the level's entities and instances
WORLD  = GAME / "Content" / "Levels" / "Steampunk" / "SteampunkStreet.nmworld"
INSTANCE_GUID = "d1ec7041c1e3a0e5a0b0c0d0e0f00001"
SEQUENCE = "Content/Cinematics/SteampunkFlyover.nmseq"


def node_id(n):
    return "4469726563746f72" + f"{n:016x}"   # "Director" + a counter


class Graph:
    def __init__(self):
        self.nodes, self.links, self.comments = [], [], []

    def node(self, n, kind, x, y, consts=None, props=None):
        body = "".join(f'\n      <Const Pin="{k}" Value="{v}" />' for k, v in (consts or {}).items())
        body += "".join(f'\n      <Prop Name="{k}" Value="{v}" />' for k, v in (props or {}).items())
        close = f">{body}\n    </Node>" if body else " />"
        self.nodes.append(f'    <Node Id="{node_id(n)}" Type="{kind}" Version="1" X="{x}" Y="{y}"{close}')
        return n

    def link(self, a, out_pin, b, in_pin):
        self.links.append(f'    <Link From="{node_id(a)}.{out_pin}" To="{node_id(b)}.{in_pin}" />')

    def comment(self, x, y, w, h, text):
        self.comments.append(f'    <Comment X="{x}" Y="{y}" Width="{w}" Height="{h}" Text="{text}" />')

    def xml(self):
        return "\n".join([
            '<?xml version="1.0" encoding="UTF-8"?>',
            '<NodeGraph Version="1" Schema="Script">',
            "  <Nodes>", *self.nodes, "  </Nodes>",
            "  <Links>", *self.links, "  </Links>",
            "  <Comments>", *self.comments, "  </Comments>",
            "</NodeGraph>", ""])


def write_graph():
    g = Graph()

    # 1. Roll it: subtitles on, then start the level's cinematic player.
    g.comment(-960, -80, 1320, 360, "On start: subtitles on, then start the level's Cinematic player (a Sequence Player placed in SteampunkStreet)")
    start    = g.node(1, "Event.OnStart", -900, 0)
    subs     = g.node(2, "Library.Sequence.SetSubtitles", -660, 0, consts={"On": "true"})
    find     = g.node(3, "World.FindEntity", -660, 140, consts={"Name": "Cinematic"})
    play     = g.node(4, "Library.Sequence.Start", -380, 0)
    rolling  = g.node(5, "Debug.Print", -100, 0, consts={"Value": "Director: rolling the flyover"})
    g.link(start, "Then", subs, "Exec")
    g.link(subs, "Then", play, "Exec")
    g.link(find, "Entity", play, "Player")
    g.link(play, "Then", rolling, "Exec")

    # 2. Each cut: the sequence's Event track emits Flyover.Shot with the shot's name.
    g.comment(-960, 340, 1320, 320, "Each cut: the sequence's Event track emits Flyover.Shot (Arg 1 is the shot's name)")
    shot     = g.node(10, "Event.Custom", -900, 420, props={"EventName": "Flyover.Shot"})
    cut_text = g.node(11, "String.Append", -660, 540, consts={"A": "Director: cut to "})
    cut_log  = g.node(12, "Debug.Print", -380, 420)
    g.link(shot, "Then", cut_log, "Exec")
    g.link(shot, "Arg 1", cut_text, "B")
    g.link(cut_text, "Out", cut_log, "Value")

    # 3. The end: the Sequence library's own event, then one of ours for gameplay.
    g.comment(-960, 720, 1320, 320, "The end (or a skip): the library's Sequence Finished event, then Director.Done for gameplay to pick up")
    finished = g.node(20, "Library.Sequence.OnSequenceFinished", -900, 800)
    end_text = g.node(21, "String.Append", -660, 920, consts={"A": "Director: finished "})
    end_log  = g.node(22, "Debug.Print", -380, 800)
    done     = g.node(23, "Events.Emit", -100, 800, consts={"Event": "Director.Done"})
    g.link(finished, "Then", end_log, "Exec")
    g.link(finished, "Sequence", end_text, "B")
    g.link(end_text, "Out", end_log, "Value")
    g.link(end_log, "Then", done, "Exec")

    # 4. The sequence's last key fires even when skipped (fireOnSkip).
    g.comment(-960, 1100, 1320, 220, "Flyover.Done is keyed fire-on-skip, so it arrives whether the cinematic played out or was skipped")
    hand     = g.node(30, "Event.Custom", -900, 1180, props={"EventName": "Flyover.Done"})
    hand_log = g.node(31, "Debug.Print", -380, 1180, consts={"Value": "Director: the airship is back in gameplay's hands"})
    g.link(hand, "Then", hand_log, "Exec")

    GRAPH.parent.mkdir(parents=True, exist_ok=True)
    GRAPH.write_text(g.xml())


def write_prefab():
    PREFAB.write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<Prefab Format="1" Name="CinematicDirector" NextId="2">
  <Script Path="Scripts/CinematicDirector.script.nmgraph" />
  <Tick Mode="Tick" />
  <Entity Id="1" Name="CinematicDirector">
    <Component Type="Transform" Version="1">
      <Position X="0" Y="0" Z="0" />
      <Rotation X="0" Y="0" Z="0" />
      <Scale X="1" Y="1" Z="1" />
    </Component>
  </Entity>
</Prefab>
""")


def write_instance():
    (FOLDER / f"{INSTANCE_GUID}.nmprefabinst").write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<Instance Guid="{INSTANCE_GUID}" Name="CinematicDirector" Prefab="Content/Prefabs/CinematicDirector.nmprefab">
  <Entity Id="1">
    <Component Type="Transform" Version="1">
      <Position X="0" Y="0" Z="0" />
      <Rotation X="0" Y="0" Z="0" />
      <Scale X="1" Y="1" Z="1" />
    </Component>
  </Entity>
</Instance>
""")
    manifest = WORLD.read_text()
    line = f'  <Instance Guid="{INSTANCE_GUID}" />'
    if line not in manifest:
        WORLD.write_text(manifest.replace("</World>", line + "\n</World>"))


if __name__ == "__main__":
    write_graph()
    write_prefab()
    write_instance()
    print(f"wrote {GRAPH.relative_to(GAME)}, {PREFAB.relative_to(GAME)} and an instance in {WORLD.relative_to(GAME)}")
