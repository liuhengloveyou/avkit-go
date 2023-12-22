package ffmpeg

import "C"

//export RemuxProgressCB
func RemuxProgressCB(t *C.char, fn *C.char, duration C.double, pos C.double) C.int {

	// var progress float64
	// if pos > 0 && duration > 0 {
	// 	progress = float64(pos) / float64(duration)
	// }
	goRemuxProgressCB(C.GoString(t), C.GoString(fn), float64(pos), float64(duration))
	return 0
}
