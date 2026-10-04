import os, re, traceback
import renderdoc as rd
OUT=os.environ["RDC_OUT"]; CAP=os.environ["RDC_CAP"]
log=open(OUT,"w")
def p(*a): print(*a,file=log); log.flush()
EVENTS=[8797,8849,8964,14355,14371,14395,14443,14811,14900,14956,15028,15037,15044,15052,15081,15094,15105,15113,15120,15128,15138,15148]
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctrl=cap.OpenCapture(rd.ReplayOptions(),None)
    sd=ctrl.GetStructuredFile()
    names={int(r.resourceId):r.name for r in ctrl.GetResources()}
    texs={int(t.resourceId):t for t in ctrl.GetTextures()}
    bufs={int(b.resourceId):b for b in ctrl.GetBuffers()}
    def rn(rid):
        i=int(rid)
        if i in texs:
            t=texs[i]; n=names.get(i,"")
            m=re.match(r"Image (\d+)x(\d+)x\d+ \S+ (\S+) (0x[0-9a-f]+):",n)
            return ("T%d %s@%s %dx%d"%(i,m.group(3),m.group(4)[-5:],t.width,t.height)) if m else "T%d %s"%(i,n[:40])
        if i in bufs: return "B%d(%d bytes)"%(i,bufs[i].length)
        return "R%d %s"%(i,names.get(i,"")[:40])
    disp=[]
    def walk(acts):
        for a in acts:
            if a.flags & rd.ActionFlags.Dispatch: disp.append(a.eventId)
            if a.children: walk(a.children)
    walk(ctrl.GetRootActions())
    for eid in sorted(set(EVENTS+disp)):
        ctrl.SetFrameEvent(eid, True)
        s=ctrl.GetPipelineState()
        stages=[rd.ShaderStage.Compute] if eid in disp else [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
        p("== event %d %s"%(eid, "DISPATCH" if eid in disp else "DRAW"))
        for stg in stages:
            sh=s.GetShader(stg)
            if int(sh)==0: continue
            p("  %s shader: %s"%(str(stg).split('.')[-1], names.get(int(sh),"?")))
            try:
                ro=s.GetReadOnlyResources(stg, True)
                for u in ro:
                    r=u.descriptor.resource
                    if int(r)!=0: p("    read : %s"%rn(r))
            except Exception as e: p("    ro err",e)
            try:
                rw=s.GetReadWriteResources(stg, True)
                for u in rw:
                    r=u.descriptor.resource
                    if int(r)!=0: p("    write: %s"%rn(r))
            except Exception as e: p("    rw err",e)
            try:
                cb=s.GetConstantBlocks(stg, True)
                for u in cb:
                    r=u.descriptor.resource
                    if int(r)!=0: p("    const: %s off=%d size=%d"%(rn(r),u.descriptor.byteOffset,u.descriptor.byteSize))
            except Exception as e: p("    cb err",e)
        if eid not in disp:
            try:
                for o in s.GetOutputTargets():
                    if int(o.resource)!=0: p("    out  : %s"%rn(o.resource))
                d=s.GetDepthTarget()
                if int(d.resource)!=0: p("    depth: %s"%rn(d.resource))
            except Exception as e: p("    out err",e)
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p(traceback.format_exc())
log.close(); os._exit(0)
