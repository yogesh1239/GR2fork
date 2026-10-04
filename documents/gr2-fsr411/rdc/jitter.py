import os, struct, traceback
import renderdoc as rd
OUT=os.environ["RDC_OUT"]; CAPS=os.environ["RDC_CAPS"].split(",")
log=open(OUT,"w")
def p(*a): print(*a,file=log); log.flush()
def inv(m):
    n=4; a=[list(m[i])+[1.0 if i==j else 0.0 for j in range(n)] for i in range(n)]
    for c in range(n):
        piv=max(range(c,n),key=lambda r:abs(a[r][c])); a[c],a[piv]=a[piv],a[c]
        d=a[c][c]; a[c]=[x/d for x in a[c]]
        for r in range(n):
            if r!=c:
                f=a[r][c]; a[r]=[x-f*y for x,y in zip(a[r],a[c])]
    return [row[n:] for row in a]
def mul(a,b): return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def mat(f,o): return [f[o+4*r:o+4*r+4] for r in range(4)]
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
for CAP in CAPS:
    try:
        cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
        st,ctrl=cap.OpenCapture(rd.ReplayOptions(),None)
        if st!=rd.ResultCode.Succeeded: p(CAP,"replay failed",st); continue
        draws=[]
        def walk(acts):
            for a in acts:
                if a.flags & rd.ActionFlags.Drawcall and len([o for o in a.outputs if int(o)!=0])>=3 and int(a.depthOut)!=0: draws.append(a.eventId)
                if a.children: walk(a.children)
        walk(ctrl.GetRootActions())
        found=0
        for ev in draws[::7]:
            ctrl.SetFrameEvent(ev, False)
            pipe=ctrl.GetPipelineState()
            for u in pipe.GetReadWriteResources(rd.ShaderStage.Vertex):
                d=u.descriptor
                if d.byteSize<192 or d.byteSize>4096: continue
                data=ctrl.GetBufferData(d.resource,d.byteOffset,192)
                f=struct.unpack("<48f",data)
                WV=mat(f,0); WVP=mat(f,16)
                if abs(WV[3][3]-1)>1e-4 or abs(WVP[2][3])<0.01: continue
                try: P=mul(inv(WV),WVP)
                except ZeroDivisionError: continue
                if abs(P[2][3]+1)>1e-3 or abs(P[0][0])<0.5: continue
                p("%s ev %d P00 %.6f P11 %.6f  jitter_px x %.6f y %.6f" % (os.path.basename(CAP), ev, P[0][0], P[1][1], P[2][0]*1280, P[2][1]*720))
                found+=1
                break
            if found>=3: break
        if not found: p(os.path.basename(CAP),"no matrix draw found among",len(draws))
        ctrl.Shutdown(); cap.Shutdown()
    except Exception:
        p(CAP, traceback.format_exc())
log.close(); os._exit(0)
