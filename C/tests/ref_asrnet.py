#!/usr/bin/env python3
"""Reference transcript from reference code, to compare asciicam with

    pip install torch transformers soundfile
    python3 tests/ref_asrnet.py <model directory or id> speech.wav
    asciicam --listen speech.wav --model <dir>/model.safetensors   # same text?

Prints generation config's start/eos ids (C engine assumes 1 and 2), token ids and the text
Pass model directory downloaded to use same files asciicam reads
Audio must be 16 kHz mono
"""
import sys

import soundfile as sf
import torch
from transformers import AutoModelForSpeechSeq2Seq, AutoProcessor

repo, wav = sys.argv[1], sys.argv[2]
model = AutoModelForSpeechSeq2Seq.from_pretrained(repo).eval()
proc = AutoProcessor.from_pretrained(repo)
audio, rate = sf.read(wav, dtype="float32")
assert rate == 16000 and audio.ndim == 1, "need 16 kHz mono"
print("decoder_start_token_id:", model.generation_config.decoder_start_token_id,
      "eos:", model.generation_config.eos_token_id)
inputs = proc(audio, sampling_rate=16000, return_tensors="pt")
with torch.no_grad():
    ids = model.generate(**inputs, max_new_tokens=int(len(audio) / 16000 * 6.5) + 1)
print("tokens:", ids[0].tolist())
print("text:", proc.batch_decode(ids, skip_special_tokens=True)[0])
