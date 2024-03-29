package avkitgo

import (
	"github.com/liuhengloveyou/avkit-go/internal/ffmpeg"
	"github.com/liuhengloveyou/avkit-go/proto"
)

type AVKit struct {
	Remuxing bool
}

func NewAVKit() *AVKit {
	return &AVKit{}
}

func (p *AVKit) GetMetadata(fn string) *proto.MetaData {
	return ffmpeg.GetMetadata(fn)
}

func (p *AVKit) Remux(in, out string, progressCB proto.GoRemuxProgressCB) error {
	return ffmpeg.Remux(in, out, progressCB)
}

func (p *AVKit) Transcode(in, out string) error {
	return ffmpeg.Transcode(in, out)
}
