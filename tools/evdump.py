"""Decode event scripts (.bfevfl) and list what they change.

    python evdump.py <flow.bfevfl> [--all]     state-changing actions and queries with their parameters
"""
import sys
import evfl

VISUAL = ("PlayAS", "Camera", "Fade", "LookAt", "DoF", "Lod", "Sound", "Emotion", "Shading", "Foliage",
          "Palette", "ModelBind", "Offstage", "Warp", "Wait", "Comment", "Participate", "Turn", "Voice",
          "Graphics", "Effect", "Rumble", "Light", "ScreenShot", "Blur")


def events(path):
    f = evfl.EventFlow()
    f.read(open(path, "rb").read())
    fc = f.flowchart
    for e in fc.events:
        d = e.data
        kind = type(d).__name__
        actor = getattr(getattr(d, "actor", None), "v", None)
        act = getattr(getattr(d, "actor_action", None), "v", None) or getattr(getattr(d, "actor_query", None), "v", None)
        params = dict(d.params.data) if getattr(d, "params", None) else {}
        sub = getattr(d, "res_flowchart_name", "") + ("/" + d.entry_point_name if getattr(d, "entry_point_name", "") else "")
        yield fc.name, e.name, kind, (str(actor.identifier) if actor else ""), (act.v if act else ""), params, sub


def main():
    every = "--all" in sys.argv
    for p in [a for a in sys.argv[1:] if not a.startswith("--")]:
        for fl, name, kind, actor, act, params, sub in events(p):
            if not every and kind == "ActionEvent" and any(v in act for v in VISUAL):
                continue
            if kind in ("ForkEvent", "JoinEvent"):
                continue
            ps = ", ".join(f"{k}={v}" for k, v in params.items())
            print(f"{fl}:{name:<10} {kind[:-5]:<8} {actor}.{act}({ps}) {sub}".rstrip())


if __name__ == "__main__":
    main()
