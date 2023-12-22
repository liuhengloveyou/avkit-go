package avkitgo

import (
	"avkitgo/internal/ffmpeg"
	"avkitgo/proto"
)

type AVKit struct {
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

func (p *AVKit) Transcode(fn string) error {
	return ffmpeg.Transcode(fn)
}
