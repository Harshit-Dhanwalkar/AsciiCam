# AsciiCam
Ascii video output from webcam in terminal.

## Demo
<img src="assets/demo2.gif" width="325">

### Edge Detection and invert (brightness) threshold change

<img src="assets/demo-edgedetection.gif" width="325">

## Build and Run
```
git clone https://github.com/Harshit-Dhanwalkar/AsciiCam.git
cd AsciiCam/C/
make

cd build/
./webcam_ascii --help
```

Run with all or selected plugins (currently 3)
```
./build/webcam_ascii -p build/invert.so -p build/threshold.so -p build/edge_detect.so
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
- [x] SIMD for YUYV to grayscale coversion.
- [x] Hot-reloading plugin system
- [ ] Analyzing frames for what changed (inter-frame compression)
- [ ] Temporal Compression
- [ ] Lookup Table (LUT) Cache Optimization


- [ ] Migrate from C to Cpp after, I consider, I have done enough optimisation in C.

---

Project is under [CC BY-NC LICENCE] by [Creative Commons licence](https://creativecommons.org/cc-licenses)
For commercial use, please contact me at *harshitpd1729@gmail.com* to purchase a commercial license."