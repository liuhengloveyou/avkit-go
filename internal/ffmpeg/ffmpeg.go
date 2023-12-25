package ffmpeg

import "C"
import (
	"github.com/liuhengloveyou/avkit-go/proto"
)

var goRemuxProgressCB proto.GoRemuxProgressCB

func GetMetadata(fn string) *proto.MetaData {
	p := cgo_getMetadata(fn)
	if p == nil {
		return nil
	}

	return &proto.MetaData{
		Width:        int(p.width),
		Height:       int(p.height),
		BitRate:      int64(p.bit_rate),
		Duration:     int64(p.duration),
		TotleSeconds: float64(p.totle_seconds),
		FN:           C.GoString(&p.fn[0]),
	}
}

func Remux(in, out string, progressCB proto.GoRemuxProgressCB) error {
	goRemuxProgressCB = progressCB
	remuxer := cgo_newRemuxer(in, out)
	return cgo_remux(remuxer)
}

func Transcode(in, out string) error {
	return cgo_transcode(in, out)
}
