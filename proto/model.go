package proto

type GoRemuxProgressCB func(t, fn string, pos, duration float64)

type MetaData struct {
	Width        int
	Height       int
	BitRate      int64
	Duration     int64
	TotleSeconds float64
	FN           string
}
