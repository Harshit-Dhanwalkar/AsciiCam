#!/usr/bin/env python3
"""
Reference for tests/test_asrnet.c: network written from transformers'
modeling_asrnet_streaming.py with numpy, run on a small model whose
weights come from a PRNG that C test

    python3 tests/gen_asrnet_vectors.py > tests/asrnet_vectors.inc

Also usable as a library: gen_weights(), forward()
"""
import math
import struct
import sys

import numpy as np

# test configuration (the real tiny model is 320/1280/6/8, vocab 32768)
CFG = dict(hid=32, ffn=64, layers=3, heads=2, dhid=32, dffn=48, dlayers=2, dheads=2,
           vocab=50, pos_rows=64, wl=[5, 5, 4], wr=[4, 0, 3], theta=10000.0, partial=0.8,
           start=1, eos=2)

M32 = 0xFFFFFFFF


class Prng:
    def __init__(self, seed):
        self.s = seed & M32

    def next(self):
        s = self.s
        s ^= (s << 13) & M32
        s ^= s >> 17
        s ^= (s << 5) & M32
        self.s = s
        return s / 4294967296.0


def tensor_list(c):
    """(name, shape, scale, kind) in file order; kind: w weight, g gamma, o ones-ish"""
    t = []
    h, f = c["hid"], c["ffn"]
    d, df = c["dhid"], c["dffn"]
    t.append(("model.encoder.embedder.comp.log_k", (1,), 0.0, "logk"))
    t.append(("model.encoder.embedder.linear.weight", (h, 80), 0.25, "w"))
    t.append(("model.encoder.embedder.conv1.weight", (2 * h, h, 5), 0.12, "w"))
    t.append(("model.encoder.embedder.conv1.bias", (2 * h,), 0.1, "w"))
    t.append(("model.encoder.embedder.conv2.weight", (h, 2 * h, 5), 0.08, "w"))
    t.append(("model.encoder.embedder.conv2.bias", (h,), 0.1, "w"))
    for l in range(c["layers"]):
        p = f"model.encoder.layers.{l}."
        for n in "qkvo":
            t.append((p + f"self_attn.{n}_proj.weight", (h, h), 0.3, "w"))
        t.append((p + "input_layernorm.gamma", (h,), 0.3, "g"))
        t.append((p + "post_attention_layernorm.gamma", (h,), 0.3, "g"))
        t.append((p + "mlp.fc1.weight", (f, h), 0.3, "w"))
        t.append((p + "mlp.fc1.bias", (f,), 0.1, "w"))
        t.append((p + "mlp.fc2.weight", (h, f), 0.25, "w"))
        t.append((p + "mlp.fc2.bias", (h,), 0.1, "w"))
    t.append(("model.encoder.final_norm.gamma", (h,), 0.3, "g"))
    t.append(("model.decoder.embed_tokens.weight", (c["vocab"], d), 0.6, "w"))
    t.append(("model.decoder.pos_emb.weight", (c["pos_rows"], h), 0.3, "w"))
    for l in range(c["dlayers"]):
        p = f"model.decoder.layers.{l}."
        for blk in ("self_attn", "encoder_attn"):
            for n in "qkvo":
                t.append((p + f"{blk}.{n}_proj.weight", (d, d), 0.3, "w"))
        for n in ("input_layernorm", "post_attention_layernorm", "final_layernorm"):
            t.append((p + n + ".weight", (d,), 0.3, "o"))
        t.append((p + "mlp.fc1.weight", (2 * df, d), 0.3, "w"))
        t.append((p + "mlp.fc1.bias", (2 * df,), 0.1, "w"))
        t.append((p + "mlp.fc2.weight", (d, df), 0.25, "w"))
        t.append((p + "mlp.fc2.bias", (d,), 0.1, "w"))
    t.append(("model.decoder.norm.weight", (d,), 0.3, "o"))
    t.append(("proj_out.weight", (c["vocab"], d), 0.5, "w"))
    return t


def gen_weights(c, seed=0x1234ABCD):
    rng = Prng(seed)
    out = {}
    for name, shape, scale, kind in tensor_list(c):
        n = int(np.prod(shape))
        if kind == "logk":
            a = np.array([math.log(0.75)], dtype=np.float32)
        else:
            a = np.empty(n, dtype=np.float32)
            for i in range(n):
                v = (rng.next() - 0.5) * scale
                if kind == "o":
                    v += 1.0  # weights of plain LayerNorm sit around 1
                a[i] = np.float32(v)
            a = a.reshape(shape)
        out[name] = a
    return out


def gen_audio(n, seed=777):
    rng = Prng(seed)
    a = np.empty(n, dtype=np.float32)
    for i in range(n):
        # a few bursts of different loudness so that the frame normalisation works
        env = 0.05 + 0.9 * (((i // 700) % 5) / 4.0)
        a[i] = np.float32((rng.next() - 0.5) * 2.0 * env)
    return a


def safetensors_bytes(tensors):
    header = {}
    off = 0
    blobs = []
    for name, a in tensors.items():
        b = a.astype("<f4").tobytes()
        header[name] = {"dtype": "F32", "shape": list(a.shape), "data_offsets": [off, off + len(b)]}
        off += len(b)
        blobs.append(b)
    import json
    hj = json.dumps(header, separators=(",", ":")).encode()
    while len(hj) % 8:
        hj += b" "
    return struct.pack("<Q", len(hj)) + hj + b"".join(blobs)


def ln(x, eps=1e-5):
    m = x.mean(-1, keepdims=True)
    v = ((x - m) ** 2).mean(-1, keepdims=True)
    return (x - m) / np.sqrt(v + eps)


def gelu(x):
    return 0.5 * x * (1.0 + np.vectorize(math.erf)(x / math.sqrt(2.0)))


def silu(x):
    return x / (1.0 + np.exp(-x))


def softmax(x):
    x = x - x.max(-1, keepdims=True)
    e = np.exp(x)
    return e / e.sum(-1, keepdims=True)


def causal_conv(x, w, b, stride):
    # x [T, Cin], w [Cout, Cin, K] -> [(T-1)//stride+1, Cout]
    cout, cin, k = w.shape
    xp = np.concatenate([np.zeros((k - 1, cin)), x], 0)
    n = (x.shape[0] - 1) // stride + 1
    out = np.empty((n, cout))
    for j in range(n):
        win = xp[j * stride: j * stride + k]  # [K, Cin]
        out[j] = np.einsum("kc,ock->o", win, w) + b
    return out


def sliding_mask(T, window):
    left, right = window
    m = np.zeros((T, T), dtype=bool)
    for q in range(T):
        for kv in range(T):
            dist = q - kv
            m[q, kv] = (dist >= 0 and dist < left) or (dist < 0 and -dist < right)
    return m


def encoder(w, c, audio):
    g = {k: v.astype(np.float64) for k, v in w.items()}
    h, nh = c["hid"], c["heads"]
    F = len(audio) // 80
    x = audio[: F * 80].astype(np.float64).reshape(F, 80)
    cen = x - x.mean(-1, keepdims=True)
    rms = np.sqrt((cen ** 2).mean(-1, keepdims=True) + 1e-6)
    x = cen / rms
    x = np.arcsinh(np.exp(g["model.encoder.embedder.comp.log_k"][0]) * x)
    x = silu(x @ g["model.encoder.embedder.linear.weight"].T)
    x = silu(causal_conv(x, g["model.encoder.embedder.conv1.weight"], g["model.encoder.embedder.conv1.bias"], 2))
    x = causal_conv(x, g["model.encoder.embedder.conv2.weight"], g["model.encoder.embedder.conv2.bias"], 2)
    T = x.shape[0]
    hd = h // nh
    for l in range(c["layers"]):
        p = f"model.encoder.layers.{l}."
        mask = sliding_mask(T, (c["wl"][l], c["wr"][l]))
        res = x
        y = ln(x) * (g[p + "input_layernorm.gamma"] + 1.0)
        q = (y @ g[p + "self_attn.q_proj.weight"].T).reshape(T, nh, hd).transpose(1, 0, 2)
        k = (y @ g[p + "self_attn.k_proj.weight"].T).reshape(T, nh, hd).transpose(1, 0, 2)
        v = (y @ g[p + "self_attn.v_proj.weight"].T).reshape(T, nh, hd).transpose(1, 0, 2)
        s = q @ k.transpose(0, 2, 1) * hd ** -0.5
        s = np.where(mask[None], s, -np.inf)
        o = (softmax(s) @ v).transpose(1, 0, 2).reshape(T, h)
        x = res + o @ g[p + "self_attn.o_proj.weight"].T
        res = x
        y = ln(x) * (g[p + "post_attention_layernorm.gamma"] + 1.0)
        y = gelu(y @ g[p + "mlp.fc1.weight"].T + g[p + "mlp.fc1.bias"])
        x = res + y @ g[p + "mlp.fc2.weight"].T + g[p + "mlp.fc2.bias"]
    return ln(x) * (g["model.encoder.final_norm.gamma"] + 1.0)


def rope(x, pos, c, hd):
    # x [heads, hd]; HF: partial rotary on the first `rot` dims, interleaved pairs
    rot = int(hd * c["partial"])
    inv = 1.0 / (c["theta"] ** (np.arange(0, rot, 2, dtype=np.float64) / rot))
    ang = pos * inv
    cos, sin = np.cos(ang), np.sin(ang)
    y = x.copy()
    x1, x2 = x[:, 0:rot:2], x[:, 1:rot:2]
    y[:, 0:rot:2] = x1 * cos - x2 * sin
    y[:, 1:rot:2] = x2 * cos + x1 * sin
    return y


def decoder_logits(w, c, enc, tokens):
    """teacher forced: logits after each token (rows)"""
    g = {k: v.astype(np.float64) for k, v in w.items()}
    d, nh = c["dhid"], c["dheads"]
    hd = d // nh
    T = enc.shape[0]
    e = enc + g["model.decoder.pos_emb.weight"][:T]
    n = len(tokens)
    x = g["model.decoder.embed_tokens.weight"][tokens]
    for l in range(c["dlayers"]):
        p = f"model.decoder.layers.{l}."

        def lnw(z, name):
            return ln(z) * g[p + name + ".weight"]

        res = x
        y = lnw(x, "input_layernorm")
        q = (y @ g[p + "self_attn.q_proj.weight"].T).reshape(n, nh, hd)
        k = (y @ g[p + "self_attn.k_proj.weight"].T).reshape(n, nh, hd)
        v = (y @ g[p + "self_attn.v_proj.weight"].T).reshape(n, nh, hd)
        q = np.stack([rope(q[i], i, c, hd) for i in range(n)])
        k = np.stack([rope(k[i], i, c, hd) for i in range(n)])
        qh, kh, vh = q.transpose(1, 0, 2), k.transpose(1, 0, 2), v.transpose(1, 0, 2)
        s = qh @ kh.transpose(0, 2, 1) * hd ** -0.5
        s = np.where(np.tril(np.ones((n, n), dtype=bool))[None], s, -np.inf)
        o = (softmax(s) @ vh).transpose(1, 0, 2).reshape(n, d)
        x = res + o @ g[p + "self_attn.o_proj.weight"].T
        res = x
        y = lnw(x, "post_attention_layernorm")
        q = (y @ g[p + "encoder_attn.q_proj.weight"].T).reshape(n, nh, hd).transpose(1, 0, 2)
        k = (e @ g[p + "encoder_attn.k_proj.weight"].T).reshape(T, nh, hd).transpose(1, 0, 2)
        v = (e @ g[p + "encoder_attn.v_proj.weight"].T).reshape(T, nh, hd).transpose(1, 0, 2)
        s = q @ k.transpose(0, 2, 1) * hd ** -0.5
        o = (softmax(s) @ v).transpose(1, 0, 2).reshape(n, d)
        x = res + o @ g[p + "encoder_attn.o_proj.weight"].T
        res = x
        y = lnw(x, "final_layernorm")
        f = y @ g[p + "mlp.fc1.weight"].T + g[p + "mlp.fc1.bias"]
        hid, gate = f[:, : c["dffn"]], f[:, c["dffn"]:]
        y = (silu(gate) * hid) @ g[p + "mlp.fc2.weight"].T + g[p + "mlp.fc2.bias"]
        x = res + y
    x = ln(x) * g["model.decoder.norm.weight"]
    return x @ g["proj_out.weight"].T


def greedy(w, c, enc, max_new):
    toks = [c["start"]]
    out = []
    for _ in range(max_new):
        lg = decoder_logits(w, c, enc, toks)[-1]
        t = int(np.argmax(lg))
        if t == c["eos"]:
            break
        out.append(t)
        toks.append(t)
    return out


def main():
    c = CFG
    w = gen_weights(c)
    audio = gen_audio(80 * 4 * 50 + 123)  # 50 encoder frames plus a partial frame
    enc = encoder(w, c, audio)
    T = enc.shape[0]
    prefix = [1, 7, 3]
    lg = decoder_logits(w, c, enc, prefix)
    gen = greedy(w, c, enc, 6)
    print("// Generated by tests/gen_asrnet_vectors.py; do not edit")
    print(f"#define MV_AUDIO {len(audio)}")
    print(f"#define MV_FRAMES {T}")
    print("static const int MV_PREFIX[3] = {%s};" % ", ".join(map(str, prefix)))
    for fr in (0, 1, T // 2, T - 1):
        print("// encoder frame %d" % fr)
    frames = [0, 1, 9, T // 2, T - 5, T - 1]
    print("static const int MV_NFRAMES = %d;" % len(frames))
    print("static const int MV_FRAME_IDX[%d] = {%s};" % (len(frames), ", ".join(map(str, frames))))
    print("static const double MV_ENC[%d][8] = {" % len(frames))
    for fr in frames:
        print("  {%s}," % ", ".join(repr(float(v)) for v in enc[fr][:8]))
    print("};")
    print("static const double MV_ENC_SUM[%d] = {%s};" % (len(frames), ", ".join(repr(float(enc[fr].sum())) for fr in frames)))
    print("static const double MV_LOGITS[3][8] = {")
    for i in range(3):
        print("  {%s}," % ", ".join(repr(float(v)) for v in lg[i][:8]))
    print("};")
    print("static const double MV_LOGIT_SUM[3] = {%s};" % ", ".join(repr(float(lg[i].sum())) for i in range(3)))
    print("static const int MV_GEN_N = %d;" % len(gen))
    print("static const int MV_GEN[%d] = {%s};" % (max(1, len(gen)), ", ".join(map(str, gen)) or "0"))


if __name__ == "__main__":
    main()

