import sys, os, json, traceback
import renderdoc as rd
OUT = os.environ["RDC_OUT"]
CAP = os.environ["RDC_CAP"]
log = open(OUT, "w")
def p(*a):
    print(*a, file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAP, "", None)
    if res != rd.ResultCode.Succeeded:
        p("open failed", res); raise SystemExit
    status, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        p("replay failed", status); raise SystemExit
    sd = ctrl.GetStructuredFile()
    texs = {int(t.resourceId): t for t in ctrl.GetTextures()}
    names = {int(r.resourceId): r.name for r in ctrl.GetResources()}
    def tdesc(rid):
        t = texs.get(int(rid))
        if not t: return "?"
        return "%s %dx%d %s%s" % (names.get(int(rid), ""), t.width, t.height, t.format.Name(), (" ms%d" % t.msSamp) if t.msSamp > 1 else "")
    def walk(actions, depth=0):
        for a in actions:
            f = a.flags
            kind = "DRAW" if f & rd.ActionFlags.Drawcall else "DISP" if f & rd.ActionFlags.Dispatch else "CLR" if f & rd.ActionFlags.Clear else "COPY" if f & (rd.ActionFlags.Copy | rd.ActionFlags.Resolve) else "PASS" if f & (rd.ActionFlags.BeginPass | rd.ActionFlags.EndPass) else "MARK" if a.children else "OTHER"
            line = "%6d %s%s %s" % (a.eventId, "  " * depth, kind, a.GetName(sd))
            if kind == "DRAW":
                line += " n=%d inst=%d" % (a.numIndices, a.numInstances)
            if kind == "DISP":
                line += " grp=%s" % (tuple(a.dispatchDimension),)
            if kind in ("DRAW", "CLR", "DISP"):
                outs = [tdesc(o) for o in a.outputs if int(o) != 0]
                if outs: line += " -> " + " | ".join(outs)
                if int(a.depthOut) != 0: line += " depth=" + tdesc(a.depthOut)
            p(line)
            if a.children: walk(a.children, depth + 1)
    walk(ctrl.GetRootActions())
    p("==== textures")
    for rid, t in sorted(texs.items(), key=lambda kv: -kv[1].width * kv[1].height):
        p(rid, tdesc(t.resourceId), "mips=%d arr=%d" % (t.mips, t.arraysize))
    ctrl.Shutdown(); cap.Shutdown()
except SystemExit:
    pass
except Exception:
    p(traceback.format_exc())
log.close()
os._exit(0)
