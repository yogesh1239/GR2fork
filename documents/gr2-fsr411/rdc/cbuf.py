import os, struct, traceback
import renderdoc as rd
OUT=os.environ["RDC_OUT"]; CAP=os.environ["RDC_CAP"]
EVENTS=[int(e) for e in os.environ["RDC_EVENTS"].split(",")]
log=open(OUT,"w")
def p(*a): print(*a,file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctrl=cap.OpenCapture(rd.ReplayOptions(),None)
    bufs={int(b.resourceId):b for b in ctrl.GetBuffers()}
    for ev in EVENTS:
        ctrl.SetFrameEvent(ev, True)
        pipe=ctrl.GetPipelineState()
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel, rd.ShaderStage.Compute):
            refl=pipe.GetShaderReflection(stage)
            if refl is None: continue
            groups=[("cb",pipe.GetConstantBlocks(stage)),("ro",pipe.GetReadOnlyResources(stage)),("rw",pipe.GetReadWriteResources(stage))]
            for kind,lst in groups:
                for i,u in enumerate(lst):
                    d=u.descriptor
                    rid=int(d.resource)
                    if rid not in bufs: continue
                    off=d.byteOffset; size=d.byteSize
                    if size==0 or size>4096: size=min(4096, bufs[rid].length-off)
                    data=ctrl.GetBufferData(d.resource, off, size)
                    n=len(data)//4
                    fl=struct.unpack("<%df"%n, data[:n*4])
                    p("== ev %d stage %s %s[%d] buf %d off %d size %d (buflen %d)"%(ev,str(stage),kind,i,rid,off,d.byteSize,bufs[rid].length))
                    for r in range(0,min(n,256),4):
                        p("  %4d: %s"%(r*4," ".join("%14.7g"%x for x in fl[r:r+4])))
            pcs = None
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p(traceback.format_exc())
log.close(); os._exit(0)
