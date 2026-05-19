# AsciiCam
Ascii video output from your webcam in your terminal.

<img src="assets/demo.gif" width="325">

## TODO

- [x] Adjust width and height of capturing frame.
- [ ] Custom ASCII charset via config file
- [x] Brightness/contrast adjustment.
- [x] Reverse video - Invert brightness $\rightarrow$ charset mapping
- [x] Color output - Extract U/V channels, map to ANSI/RGB codes
- [ ] Add feature to record and save it in popular video formats like `.mp4`, `.mov` and `.gif`.
- [x] Dithering effect.
- [ ] A producer/consumer split with pthread_mutex + pthread_cond and a double-buffer swap would decouple them: one thread talks exclusively to V4L2, the other does ASCII conversion and writes.
- [ ] Migrate from C to Cpp.
