import os, struct, zlib, traceback
import renderdoc as rd
OUT=os.environ["RDC_OUT"]; CAP=os.environ["RDC_CAP"]; D=os.path.dirname(OUT)
log=open(OUT,"w")
def p(*a): print(*a,file=log); log.flush()
def write_png(path,w,h,rows):
    raw=b"".join(b"\x00"+bytes(r) for r in rows)
    def chunk(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
    open(path,"wb").write(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))+chunk(b"IDAT",zlib.compress(raw,6))+chunk(b"IEND",b""))
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctrl=cap.OpenCapture(rd.ReplayOptions(),None)
    texs={int(t.resourceId):t for t in ctrl.GetTextures()}
    def tid(i): return texs[i].resourceId
    sub=rd.Subresource(0,0,0)
    # velocity after the game's camera-motion pass, before post
    ctrl.SetFrameEvent(15058, True)
    data=ctrl.GetTextureData(tid(2223), sub)
    W,H=2560,1440
    p("velocity bytes", len(data), "expected", W*H*4)
    vals=struct.unpack("<%de"%(W*H*2), data[:W*H*4])
    xs=vals[0::2]; ys=vals[1::2]
    import math
    finite=[(x,y) for x,y in zip(xs[::37],ys[::37]) if math.isfinite(x) and math.isfinite(y)]
    ax=sorted(abs(x) for x,_ in finite); ay=sorted(abs(y) for _,y in finite)
    def pct(a,q): return a[min(len(a)-1,int(q*len(a)))]
    p("samples", len(finite))
    p("x: min %.6g max %.6g  |x| p50 %.6g p90 %.6g p99 %.6g" % (min(x for x,_ in finite), max(x for x,_ in finite), pct(ax,.5), pct(ax,.9), pct(ax,.99)))
    p("y: min %.6g max %.6g  |y| p50 %.6g p90 %.6g p99 %.6g" % (min(y for _,y in finite), max(y for _,y in finite), pct(ay,.5), pct(ay,.9), pct(ay,.99)))
    for (px,py) in [(1280,200),(300,300),(2300,300),(1280,720),(1280,1350),(200,1300),(2400,1300),(1000,900),(1600,900),(1270,800)]:
        i=py*W+px; p("vel at (%d,%d): %.6g %.6g" % (px,py,xs[i],ys[i]))
    # visualization 640x360: |v| scaled, R=x G=y
    mx=max(pct(ax,.99),pct(ay,.99),1e-9)
    rows=[]
    for y in range(0,H,4):
        r=bytearray()
        for x in range(0,W,4):
            i=y*W+x; vx=xs[i]; vy=ys[i]
            r+=bytes((max(0,min(255,int(128+127*vx/mx))), max(0,min(255,int(128+127*vy/mx))), 128))
        rows.append(r)
    write_png(os.path.join(D,"velocity_vis.png"),W//4,H//4,rows)
    # velocity right after the G-buffer (objects only, before camera pass)
    ctrl.SetFrameEvent(14355, True)
    d2=ctrl.GetTextureData(tid(2223), sub)
    v2=struct.unpack("<%de"%(W*H*2), d2[:W*H*4])
    rows=[]
    for y in range(0,H,4):
        r=bytearray()
        for x in range(0,W,4):
            i=y*W+x; vx=v2[2*i]; vy=v2[2*i+1]
            r+=bytes((max(0,min(255,int(128+127*vx/mx))) if math.isfinite(vx) else 0, max(0,min(255,int(128+127*vy/mx))) if math.isfinite(vy) else 0, 128))
        rows.append(r)
    write_png(os.path.join(D,"velocity_gbuffer_only.png"),W//4,H//4,rows)
    # depth samples
    ctrl.SetFrameEvent(15058, True)
    for (px,py) in [(1280,40),(100,100),(1280,720),(1280,1400),(980,1100),(200,1300)]:
        v=ctrl.PickPixel(tid(2192), px, py, sub, rd.CompType.Typeless)
        p("depth at (%d,%d): %.7f" % (px,py,v.floatValue[0]))
        v=ctrl.PickPixel(tid(2215), px, py, sub, rd.CompType.Typeless)
        p("  linear R32F at (%d,%d): %.6g" % (px,py,v.floatValue[0]))
    # save LDR images
    def save(i, name, eid):
        ctrl.SetFrameEvent(eid, True)
        ts=rd.TextureSave(); ts.resourceId=tid(i); ts.destType=rd.FileType.PNG; ts.mip=0; ts.alpha=rd.AlphaMapping.Discard
        ok=ctrl.SaveTexture(ts, os.path.join(D,name)); p("save",name,ok)
    save(2366,"tonemapped_before_taa.png",15087)
    save(2374,"taa_output.png",15094)
    save(2155,"final_display.png",15148)
    save(2375,"taa_history_in.png",15087)
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p(traceback.format_exc())
log.close(); os._exit(0)
