#!/usr/bin/env python3
"""Compares transcription models on known phrases, with no microphone and no speaker.

The phrases are spoken to files by the phone's synthetic voice (Piper), with the noise of a real
room underneath; each model transcribes them, and wrong words and time are counted. The synthetic
voice is easier than a person: this shows whether a variant of a model got worse, not how many
mistakes it will make live. Runs on the phone, with the mangos-parakeet service stopped (the
models do not fit in memory together).

  transcribe_compare.py <model directory>...        (directories under /opt/mangos/asr)
  --noise file.wav   a stretch without speech recorded from the microphone (16 kHz), mixed underneath
"""
import importlib.machinery, importlib.util, json, os, re, sys, time, wave
os.environ["OPENBLAS_NUM_THREADS"] = "1"
sys.path.insert(0, "/opt/mangos/asr/lib")
import numpy as np
import onnxruntime
import onnx_asr

PHRASES = """Ciao PIM, mi senti?
Scatta una foto di quello che sto guardando e dimmi cosa vedi.
Che ore sono?
Chiudi la foto.
Voglio che disegni la tua casa con più stanze, e che la arredi come piace a te.
Abbassa il volume, per favore.
Cerca un ristorante di sushi in zona Isola per stasera alle otto e mezza.
Va bene, grazie.
Quanta batteria è rimasta?
Apri la pagina che hai fatto ieri sui ristoranti.
Metti una lampada vicino alla finestra e dipingi le pareti di verde.
Domani mattina ricordami di chiamare il dentista alle nove.
Non ho capito, puoi ripetere più lentamente?
Scrivi una mail a Giulia e dille che arrivo tra venti minuti.
Com'è il tempo oggi a Milano?
Spegni lo schermo e resta in ascolto.
Fammi un riassunto delle notizie di oggi sull'intelligenza artificiale.
Quanto fa trecentoquarantasette diviso dodici?
Gira la fotocamera e guardami in faccia.
Cancella l'ultima cosa che ti ho chiesto.
Mi serve una lista della spesa con pane, latte, uova e pomodori.
Cerca su internet come si configura il Wi-Fi su Linux.
Alza un po' la voce, non ti sento bene.
Che cosa stavi facendo prima che ti interrompessi?
Controlla la temperatura del processore e dimmi se è troppo alta.
Traduci in inglese questa frase: il gatto dorme sul divano.
Fai partire un timer di cinque minuti.
Secondo te quale di questi due progetti conviene finire per primo?
Salva questa conversazione e ricordamela domani.
Basta così, a dopo.""".splitlines()


def words(t):
    return re.sub(r"[^a-zàèéìòù0-9 ]", " ", t.lower().replace("'", " ")).split()


def wrong_words(good, heard):
    """Word-level edit distance between the expected and the transcribed words."""
    row = list(range(len(heard) + 1))
    for i, a in enumerate(good, 1):
        new = [i]
        for j, b in enumerate(heard, 1):
            new.append(min(row[j] + 1, new[j - 1] + 1, row[j - 1] + (a != b)))
        row = new
    return row[-1]


def synthesize(noise):
    """Every phrase as 16 kHz samples (from -1 to 1), with the noise underneath."""
    loader = importlib.machinery.SourceFileLoader("mangos_speak", "/usr/bin/mangos-speak")
    speak = importlib.util.module_from_spec(importlib.util.spec_from_loader("mangos_speak", loader))
    loader.exec_module(speak)
    synth = speak.Synthesizer()
    waves = []
    for phrase in PHRASES:
        parts = [synth.wave(group) for group in speak.sentences(phrase)]
        audio = np.concatenate(parts)
        n = int(len(audio) * 16000 / synth.sample_rate)
        audio = np.interp(np.arange(n) * synth.sample_rate / 16000, np.arange(len(audio)), audio).astype(np.float32)
        audio *= 0.3 / max(1e-6, np.abs(audio).max())          # the peak of a voice at thirty centimeters
        if noise is not None:
            start = np.random.randint(0, max(1, len(noise) - len(audio)))
            audio[:len(noise) - start] += noise[start:start + len(audio)]
        waves.append(audio)
    return waves


def main():
    args = sys.argv[1:]
    noise = None
    if "--noise" in args:
        i = args.index("--noise")
        with wave.open(args[i + 1]) as f:
            if (f.getframerate(), f.getnchannels(), f.getsampwidth()) != (16000, 1, 2):
                sys.exit("--noise: needs a 16 kHz, mono, 16-bit WAV")
            noise = np.frombuffer(f.readframes(f.getnframes()), dtype="<i2").astype(np.float32) / 32768
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    np.random.seed(1)
    waves = synthesize(noise)
    seconds = sum(len(o) for o in waves) / 16000
    expected = [words(p) for p in PHRASES]
    print("%d phrases, %d words, %.0f seconds" % (len(PHRASES), sum(len(g) for g in expected), seconds), flush=True)
    for directory in args:
        model_type = json.load(open("/opt/mangos/asr/%s/config.json" % directory))["model_type"]
        options = onnxruntime.SessionOptions(); options.intra_op_num_threads = 8
        model = onnx_asr.load_model(model_type, "/opt/mangos/asr/" + directory, quantization="int8", sess_options=options,
                                    providers=["CPUExecutionProvider"])
        errors, started, details = 0, time.time(), []
        for good, phrase, samples in zip(expected, PHRASES, waves):
            text = model.recognize(samples, sample_rate=16000)
            e = wrong_words(good, words(text))
            errors += e
            if e:
                details.append("   %d  %s  ->  %s" % (e, phrase, text))
        t = time.time() - started
        print("%s: %d wrong words, %.1f s (%.2f times the duration)" % (directory, errors, t, t / seconds), flush=True)
        print("\n".join(details), flush=True)
        del model


if __name__ == "__main__":
    main()
