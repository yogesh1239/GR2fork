import sys, time
from evdev import UInput, ecodes as e
key = getattr(e, "KEY_" + sys.argv[1].upper()); every = float(sys.argv[2]); count = int(sys.argv[3])
ui = UInput({e.EV_KEY: [key]}, name="claude-virtual-keyboard")
time.sleep(1.0)
for i in range(count):
    ui.write(e.EV_KEY, key, 1); ui.syn(); time.sleep(0.15)
    ui.write(e.EV_KEY, key, 0); ui.syn()
    print(time.strftime("%H:%M:%S"), "press", i + 1, flush=True)
    if i + 1 < count: time.sleep(every)
time.sleep(0.5); ui.close()
