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

done = []

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
        try:
            w.getParam("frameRange").set(2)      # manual
            w.getParam("firstFrame").set(1)
            w.getParam("lastFrame").set(3)
        except Exception:
            pass
        app.render(w, 1, 3)
        log("WROTE", name, path, lab)
        done.append(w)
    except Exception:
        log("FAILED", name, traceback.format_exc().replace("\n", " | "))

PHASE = os.environ.get("SOLVFX_RENDERTEST_PHASE", "gen")
if PHASE == "gen":
    write("mp4_h264", ".mp4", src, codec="libx264")
    write("prores_422_lt", ".mov", src, codec="apcs")
    write("prores_422_hq", ".mov", src, codec="apch")
    write("prores_4444_alpha", ".mov", rad, codec="ap4h", alpha=True)
    write("prores_4444_noalpha", ".mov", src, codec="ap4h", alpha=False)
    write("prores_4444xq_alpha", ".mov", rad, codec="ap4x", alpha=True)
    write("prores_4444xq_noalpha", ".mov", src, codec="ap4x", alpha=False)
    write("png_seq.####", ".png", rad)
    write("exr_seq.####", ".exr", rad)
    # premultiplied soft alpha into codecs without alpha
    write("prores_422_hq_softalpha", ".mov", rad, codec="apch")
    write("mp4_softalpha", ".mp4", rad, codec="libx264")
else:
    # second run: a Read of the EXR sequence written by the first run (like real comps)
    try:
        rd = app.createNode("fr.inria.built-in.Read")
        rd.getParam("filename").set(os.path.join(OUT, "exr_seq.####.exr"))
        for k in ("firstFrame", "lastFrame", "outputComponents", "outputLayer", "outputPremult"):
            try:
                prm = rd.getParam(k)
                v = prm.getValue()
                try:
                    v = "%s (%s)" % (v, prm.getOption(v))
                except Exception:
                    pass
                log("READ", k, v)
            except Exception:
                pass
        write("prores_422_hq_fromread", ".mov", rd, codec="apch")
        write("mp4_fromread", ".mp4", rd, codec="libx264")
        write("prores_4444_fromread", ".mov", rd, codec="ap4h", alpha=True)
        write("exr_fromread.####", ".exr", rd)
    except Exception:
        log("FAILED read", traceback.format_exc().replace("\n", " | "))
# the renderer renders every Write left in the script again at the end: remove them
for w in done:
    try:
        w.destroy()
    except Exception:
        try:
            w.getParam("disableNode").set(True)
        except Exception:
            pass
for n in app.getChildren():
    try:
        if n.getPluginID() == "fr.inria.built-in.Write":
            n.destroy()
    except Exception:
        pass
log("DONE")
