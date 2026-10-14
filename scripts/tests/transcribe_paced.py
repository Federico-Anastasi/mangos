#!/usr/bin/env python3
"""Feeds mangos-transcribe a recording at speech pace, as the microphone would, and for every
phrase prints the texts fixed along the way with their time, the final text and how long it waited
after the last word. Runs on the phone; does not open the microphone and sends nothing to the entity.

  transcribe_paced.py <recording.wav>       (16 kHz, mono, 16 bit)
"""
import importlib.machinery, importlib.util, sys, time, wave

if len(sys.argv) != 2:
    sys.exit(__doc__)
loader = importlib.machinery.SourceFileLoader("mangos_transcribe", "/usr/bin/mangos-transcribe")
transcribe = importlib.util.module_from_spec(importlib.util.spec_from_loader("mangos_transcribe", loader))
loader.exec_module(transcribe)


class Paced:
    """An input that yields samples no faster than the microphone would."""

    def __init__(self, data):
        self.data, self.pos, self.start, self.from_pos = data, 0, time.time(), 0

    def resume(self):
        self.start, self.from_pos = time.time(), self.pos

    def read(self, count):
        self.pos += count
        wait = self.start + (self.pos - self.from_pos) / 2 / transcribe.SAMPLE_RATE - time.time()
        if wait > 0:
            time.sleep(wait)
        return self.data[self.pos - count:self.pos]

    def seconds(self):
        return self.pos / 2 / transcribe.SAMPLE_RATE


with wave.open(sys.argv[1]) as f:
    source = Paced(f.readframes(f.getnframes()))
waits = []
while len(source.data) - source.pos >= 2 * transcribe.WINDOW:      # a window is left to read
    begin = time.time()
    source.resume()
    transcription = transcribe.ChunkedTranscription(lambda text: print("   %5.1f s  %s" % (time.time() - begin, text), flush=True))
    samples = transcribe.cut_phrase(source, transcription)
    if samples is None:
        break
    end = time.time()
    text = transcription.finish()
    # the phrase ends END_AFTER windows before mangos-transcribe notices: the real wait starts there
    waits.append(time.time() - end + transcribe.END_AFTER * transcribe.WINDOW / transcribe.SAMPLE_RATE)
    print("%.1f s of phrase (up to %.1f s of the recording), wait %.1f s: %s" % (len(samples) / transcribe.SAMPLE_RATE, source.seconds(), waits[-1], text), flush=True)
if waits:
    print("phrases: %d, average wait %.1f s, maximum %.1f s" % (len(waits), sum(waits) / len(waits), max(waits)))
