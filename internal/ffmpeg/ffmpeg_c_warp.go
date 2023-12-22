package ffmpeg

import (
	"fmt"
)

/*
#if defined(CGO_OS_WINDOWS)
#elif defined(CGO_OS_DARWIN)
#elif defined(CGO_OS_LINUX)
#else
#	// error(unknown os)
#endif
*/
// #cgo windows CFLAGS: -DCGO_OS_WINDOWS=1 -g
// #cgo darwin CFLAGS: -DCGO_OS_DARWIN=1
// #cgo linux CFLAGS: -DCGO_OS_LINUX=1
// #cgo CFLAGS: -I D:/dev/av/ffmpeg-6.1-full_build-shared/include
// #cgo LDFLAGS: -L D:/dev/av/ffmpeg-6.0-full_build-shared/lib -lavcodec.dll -lavutil.dll -lswscale.dll
// #cgo windows CPPFLAGS: -I. -ID:/dev/av/ffmpeg-6.1-full_build-shared/include
// #cgo windows CXXFLAGS: -std=c++11
// #cgo windows LDFLAGS: -LD:/dev/av/ffmpeg-6.1-full_build-shared/lib -lavcodec -lavformat -lswscale -lswresample -lavutil -lavfilter
// #cgo windows FFLAGS:
// #include "progress.h"
// #include "metadata.h"
// #include "remux.h"
// #include "transcode_capi.h"
// int RemuxProgressCB(char *, char *, double, double);
import "C"

type cgo_metaData_t C.metaData_t
type cgo_Remuxer C.Remuxer

// remux
func cgo_newRemuxer(in, out string) *cgo_Remuxer {
	cb := C.ProgressCBPtr(C.RemuxProgressCB)

	p := C.newRemuxer(C.CString(in), C.CString(out), cb)
	return (*cgo_Remuxer)(p)
}

func cgo_remux(remuxer *cgo_Remuxer) error {
	rst := C.remux((*C.Remuxer)(remuxer))
	if rst != 0 {
		return fmt.Errorf("C.remux ERR")
	}

	return nil
}

func cgo_getMetadata(fn string) *cgo_metaData_t {
	p := C.get_metadata(C.CString(fn))
	return (*cgo_metaData_t)(p)
}

func cgo_transcode(afn, bfn string) error {
	ret := C.transcode(C.CString(afn), C.CString(bfn))
	if ret < 0 {
		return fmt.Errorf("C.transcode error")
	}

	return nil
}
