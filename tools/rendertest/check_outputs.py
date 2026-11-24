# Checks the render test files: decodes the first frame and reports mean colour / alpha.
import glob, os, subprocess, sys
import numpy as np
import imageio_ffmpeg

out = sys.argv[1]
ff = imageio_ffmpeg.get_ffmpeg_exe()
bad = 0
for f in sorted(glob.glob(os.path.join(out, "*"))):
    name = os.path.basename(f)
    if not (name.endswith(".mov") or name.endswith(".mp4") or name.endswith(".0001.png") or name.endswith(".0001.exr") or name.endswith("1.png") or name.endswith("1.exr")):
        continue
    probe = subprocess.run([ff, "-hide_banner", "-i", f], capture_output=True, text=True).stderr
    stream = [l.strip() for l in probe.splitlines() if "Video:" in l]
    try:
        raw = subprocess.run([ff, "-v", "error", "-i", f, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgba64le", "-"],
                             capture_output=True, check=True).stdout
        a = np.frombuffer(raw, dtype=np.uint16).astype(np.float64) / 65535.0
        a = a.reshape(-1, 4)
        m = a.mean(axis=0)
        status = "BLACK" if m[:3].max() < 0.01 else "OK"
        if status != "OK":
            bad += 1
        print("CHECK %-28s %-6s size %9d  mean rgba %.3f %.3f %.3f %.3f  %s" % (name, status, os.path.getsize(f), m[0], m[1], m[2], m[3], stream[0] if stream else ""))
    except Exception as e:
        bad += 1
        print("CHECK %-28s ERROR %s %s" % (name, e, stream[0] if stream else probe[-300:]))
print("CHECK done, problems:", bad)
