Download models
```sh
B=https://huggingface.co/moonshine-ai/moonshine-streaming-tiny/resolve/main
curl -L -f -O $B/config.json -O $B/tokenizer.json -O $B/model.safetensors
```

Pytouch installation
```sh
pip install torch --index-url https://download.pytorch.org/whl/cpu
pip install soundfile
pip install "transformers @ git+https://github.com/huggingface/transformers.git"

# verify
python -c "from transformers import MoonshineStreamingForConditionalGeneration; print('OK')"
```
