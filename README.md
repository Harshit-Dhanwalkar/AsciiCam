# AsciiCam
Ascii video output from webcam in terminal.

<img src="assets/demo.gif" width="325">

## Build and Run
```
git clone https://github.com/Harshit-Dhanwalkar/AsciiCam.git
cd AsciiCam/C/
make

cd build/
./webcam_ascii --help
```


## TODO

- [x] Adjust width and height of capturing frame.
- [x] A producer/consumer thread splitting.
- [ ] Custom ASCII charset via config file
- [x] Brightness/contrast adjustment.
- [x] Reverse video - Invert brightness $\rightarrow$ charset mapping
- [x] Color output - Extract U/V channels, map to ANSI/RGB codes
- [ ] Add feature to record and save it in popular video formats like `.mp4`, `.mov` and `.gif`.
- [x] Dithering effect.
- [x] Sobel edge detection (kernel convolution). Algorithm reference: https://homepages.inf.ed.ac.uk/rbf/HIPR2/sobel.htm

- [ ] Migrate from C to Cpp.
