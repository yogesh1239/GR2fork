import os, traceback
import renderdoc as rd
OUT=os.environ["RDC_OUT"]; CAP=os.environ["RDC_CAP"]; D=os.path.dirname(OUT)
log=open(OUT,"w")
def p(*a): print(*a,file=log); log.flush()
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctrl=cap.OpenCapture(rd.ReplayOptions(),None)
    targets=ctrl.GetDisassemblyTargets(True)
    p("targets:", list(targets))
    for eid,name in [(15087,"taa"),(14773,"cammotion"),(15058,"mbcompress"),(15081,"tonemap")]:
        ctrl.SetFrameEvent(eid, True)
        s=ctrl.GetPipelineState()
        stg=rd.ShaderStage.Pixel if name=="tonemap" else rd.ShaderStage.Compute
        refl=s.GetShaderReflection(stg)
        pipe=s.GetComputePipelineObject() if stg==rd.ShaderStage.Compute else s.GetGraphicsPipelineObject()
        for t in targets:
            txt=ctrl.DisassembleShader(pipe, refl, t)
            fn=os.path.join(D,"%s_%s.txt"%(name,t.replace(" ","_").replace("(","").replace(")","").replace("/","_")))
            open(fn,"w").write(txt)
            p(name, t, len(txt))
        # constants
        for cb in s.GetConstantBlocks(stg, True):
            p(name,"cb", cb.access.index if hasattr(cb,'access') else '', cb.descriptor.byteOffset, cb.descriptor.byteSize)
            try:
                vars=ctrl.GetCBufferVariableContents(pipe, refl.resourceId, stg, refl.entryPoint, cb.access.index, cb.descriptor.resource, cb.descriptor.byteOffset, cb.descriptor.byteSize)
                def dump(vs,ind="  "):
                    for v in vs:
                        if v.members: p(ind+v.name); dump(v.members, ind+"  ")
                        else: p(ind+"%s = %s"%(v.name, [v.value.f32v[i] for i in range(v.rows*v.columns)]))
                dump(vars)
            except Exception as e: p("cb read err", e)
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p(traceback.format_exc())
log.close(); os._exit(0)
