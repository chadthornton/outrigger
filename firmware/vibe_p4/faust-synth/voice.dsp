// voice.dsp: one voice of the Faust bench patch. The Vibe (faust_synth.c) runs 8 of them, like example-synth:
// a band-limited saw through a resonant low-pass, an ADSR, velocity.
declare name "faust-voice";
declare license "MIT";
import("stdfaust.lib");
freq = hslider("freq", 440, 20, 20000, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.001);
gate = button("gate");
cutoff = hslider("cutoff", 1800, 40, 12000, 1);
q = hslider("q", 1.2, 0.5, 8, 0.01);
env = en.adsr(0.005, 0.3, 0.7, 0.4, gate);
process = os.sawtooth(freq) : fi.resonlp(cutoff, q, 1) : *(env * gain);
