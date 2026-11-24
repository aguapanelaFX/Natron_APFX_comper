# SOLVFX render test (run with SOLVFXRenderer.exe <this script>): writes a few frames in every
# format the user relies on, so CI can check that the files are not black.
import os
import sys
import traceback

try:
    app = app1
except NameError:
    import NatronEngine
    app = NatronEngine.natron.getActiveInstance()

OUT = os.environ.get("SOLVFX_RENDERTEST_OUT", os.path.join(os.getcwd(), "rendertest_out"))
if not os.path.isdir(OUT):
    os.makedirs(OUT)

def log(*a):
    sys.stdout.write("RENDERTEST " + " ".join(str(x) for x in a) + "\n")
    sys.stdout.flush()

# small format: fast
try:
    app.getProjectParam("outputFormat").set("HD 1920x1080")
except Exception:
    pass

src = app.createNode("net.sf.openfx.CheckerBoardPlugin")
rad = app.createNode("net.sf.openfx.Radial")   # soft alpha for the alpha tests
for n in (src, rad):
    try:
        n.getParam("extent").set(1)    # format
    except Exception:
        pass

def pick_codec(w, key):
    p = w.getParam("codec")
    n = p.getNumOptions()
    for i in range(n):
        lab = p.getOption(i)
        if key.lower() in lab.lower():
            p.set(i)
            return lab
    raise RuntimeError("codec %s not found in %s" % (key, [p.getOption(i) for i in range(n)]))

def write(name, ext, inp, codec=None, alpha=None, extra=None):
    path = os.path.join(OUT, name + ext)
    try:
        w = app.createNode("fr.inria.built-in.Write")
        w.connectInput(0, inp)
        w.getParam("filename").set(path)
        lab = ""
        if codec:
            lab = pick_codec(w, codec)
        if alpha is not None:
            p = w.getParam("enableAlpha")
            if p:
                p.set(alpha)
        try:
            w.getParam("overwrite").set(True)
        except Exception:
            pass
        for k, v in (extra or {}).items():
            w.getParam(k).set(v)
        app.render(w, 1, 3)
        log("WROTE", name, path, lab)
    except Exception:
        log("FAILED", name, traceback.format_exc().replace("\n", " | "))

write("mp4_h264", ".mp4", src, codec="libx264")
write("prores_422_lt", ".mov", src, codec="apcs")
write("prores_422_hq", ".mov", src, codec="apch")
write("prores_4444_alpha", ".mov", rad, codec="ap4h", alpha=True)
write("prores_4444_noalpha", ".mov", src, codec="ap4h", alpha=False)
write("prores_4444xq_alpha", ".mov", rad, codec="ap4x", alpha=True)
write("prores_4444xq_noalpha", ".mov", src, codec="ap4x", alpha=False)
write("png_seq.####", ".png", rad)
write("exr_seq.####", ".exr", rad)
log("DONE")
