#include "asrnet.h"
#include "nolibc.h"

static int pass_n, fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static int same(const an_config_t *a, const an_config_t *b) {
  int ok = a->hid == b->hid && a->ffn == b->ffn && a->layers == b->layers &&
           a->heads == b->heads && a->dhid == b->dhid && a->dffn == b->dffn &&
           a->dlayers == b->dlayers && a->dheads == b->dheads &&
           a->vocab == b->vocab && a->start_tok == b->start_tok &&
           a->eos_tok == b->eos_tok && a->rope_theta == b->rope_theta &&
           a->rope_partial == b->rope_partial;
  for (int i = 0; i < a->layers; i++) {
    ok = ok && a->win_l[i] == b->win_l[i] && a->win_r[i] == b->win_r[i];
  }

  return ok;
}

// File as transformers writes it for tiny model
// A config.json as published for smallest model (keys parser does not read,
// like names of model class, are skipped)
static const char *TINY =
    "{\n \"architectures\": [\"SomeModel\"],\n"
    " \"model_type\": \"some_model\",\n"
    " \"encoder_config\": {\"hidden_size\": 320, \"intermediate_size\": 1280,"
    " \"num_hidden_layers\": 6, \"num_attention_heads\": 8, \"frame_ms\": 5.0,"
    " \"sliding_windows\": [[16, 4], [16, 4], [16, 0], [16, 0], [16, 4],"
    " [16, 4]]},\n"
    " \"vocab_size\": 32768, \"hidden_size\": 320, \"intermediate_size\": 1280,"
    " \"num_hidden_layers\": 6, \"num_attention_heads\": 8,\n"
    " \"bos_token_id\": 1, \"eos_token_id\": 2, \"decoder_start_token_id\": "
    "null,\n"
    " \"rope_parameters\": {\"rope_type\": \"default\", \"rope_theta\": "
    "10000.0, \"partial_rotary_factor\": 0.8},\n \"torch_dtype\": \"float32\"}";

int main(void) {
  an_config_t c;
  an_config_t t;
  char err[96];
  an_config_tiny(&t);
  CHECK(an_config_parse(&c, TINY, nl_strlen(TINY), err, sizeof(err)) == 0,
        "tiny: %s", err);
  CHECK(same(&c, &t), "the tiny file gives the built-in tiny config");

  // Another shape: values come from file, rest stays
  const char *S =
      "{\"encoder_config\":{\"hidden_size\":620,\"intermediate_size\":2480,"
      "\"num_hidden_layers\":2,\"num_attention_heads\":10,"
      "\"sliding_windows\":[[8,2],[8,0]]},\"hidden_size\":512,"
      "\"intermediate_size\":2048,\"num_hidden_layers\":3,"
      "\"num_attention_heads\":8,\"vocab_size\":1000,\"decoder_start_token_id\""
      ":5,\"eos_token_id\":7,\"rope_parameters\":{\"rope_theta\":500.5,"
      "\"partial_rotary_factor\":0.5}}";
  CHECK(an_config_parse(&c, S, nl_strlen(S), err, sizeof(err)) == 0, "S: %s",
        err);
  CHECK(c.hid == 620 && c.ffn == 2480 && c.layers == 2 && c.heads == 10 &&
            c.dhid == 512 && c.dffn == 2048 && c.dlayers == 3 &&
            c.dheads == 8 && c.vocab == 1000 && c.start_tok == 5 &&
            c.eos_tok == 7,
        "dims");
  CHECK(c.win_l[0] == 8 && c.win_r[0] == 2 && c.win_l[1] == 8 &&
            c.win_r[1] == 0,
        "windows");
  CHECK(c.rope_theta == 500.5f && c.rope_partial == 0.5f, "rope");

  // An empty object is tiny model
  CHECK(an_config_parse(&c, "{}", 2, err, sizeof(err)) == 0 && same(&c, &t),
        "empty object");
  // Unknown keys, nested things and strings with braces are skipped
  const char *U =
      "{\"x\":{\"a\":[1,2,{\"b\":\"}\"}]},\"y\":\"\\\"vocab_size\\\""
      ":3\",\"vocab_size\":77}";
  CHECK(an_config_parse(&c, U, nl_strlen(U), err, sizeof(err)) == 0 &&
            c.vocab == 77,
        "skipping: %s vocab %d", err, c.vocab);

  const char *bad[] = {
      "",
      "[]",
      "{",
      "{\"hidden_size\":}",
      "{\"hidden_size\":0}",
      "{\"hidden_size\":-4}",
      "{\"hidden_size\":3.5}",
      "{\"hidden_size\":\"320\"}",
      "{\"hidden_size\":99999999}",
      "{\"hidden_size\":1e3}",
      "{\"num_hidden_layers\":17}",
      "{\"hidden_size\":321}",
      "{\"encoder_config\":{\"sliding_windows\":[[16,4]]}}",
      "{\"encoder_config\":{\"sliding_windows\":[[16,4],[16,4],[16,0],[16,0],"
      "[16,4],[0,4]]}}",
      "{\"encoder_config\":{\"sliding_windows\":[[16,4],[16,4],[16,0],[16,0],"
      "[16,4],[16,99]]}}",
      "{\"encoder_config\":{\"sliding_windows\":5}}",
      "{\"encoder_config\":{\"sliding_windows\":[[1]]}}",
      "{} x",
  };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    err[0] = 0;
    int r = an_config_parse(&c, bad[i], nl_strlen(bad[i]), err, sizeof(err));
    CHECK(r == -1 && err[0], "bad %u refused: %s", i, err);
  }

  // Every truncation of real file is refused
  size_t len = nl_strlen(TINY);
  int early = 0;
  for (size_t k = 0; k < len; k++) {
    early += an_config_parse(&c, TINY, k, err, sizeof(err)) == 0;
  }

  CHECK(early == 0, "%d truncated files accepted", early);

  // Damage never crashes and what is accepted is within limits
  unsigned s = 4242;
  char *buf = nl_malloc(len + 1);
  int accepted = 0;
  for (int it = 0; it < 4000; it++) {
    nl_memcpy(buf, TINY, len + 1);
    for (int k = 0; k < 2; k++) {
      s ^= s << 13u;
      s ^= s >> 17u;
      s ^= s << 5u;
      buf[s % len] = (char)(s >> 8u);
    }

    if (an_config_parse(&c, buf, len, err, sizeof(err)) == 0) {
      accepted++;
      CHECK(c.layers >= 1 && c.layers <= AN_MAX_LAYERS && c.dlayers >= 1 &&
                c.dlayers <= AN_MAX_LAYERS && c.hid % c.heads == 0 &&
                c.dhid % c.dheads == 0,
            "accepted config is sane");
    }
  }

  nl_free(buf);
  CHECK(accepted > 0, "some damaged files still parse (%d)", accepted);
  fprintf(stderr, "asrcfg: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n != 0;
}
