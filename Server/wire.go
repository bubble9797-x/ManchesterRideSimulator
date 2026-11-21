package main

import (
	"encoding/binary"
	"math"
	"unicode/utf8"
)

// Little-endian message writer/reader shared by every message. Strings are u16 byte length + UTF-8.

type W struct{ b []byte }

func (w *W) U8(v uint8)    { w.b = append(w.b, v) }
func (w *W) U16(v uint16)  { w.b = binary.LittleEndian.AppendUint16(w.b, v) }
func (w *W) U32(v uint32)  { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *W) U64(v uint64)  { w.b = binary.LittleEndian.AppendUint64(w.b, v) }
func (w *W) F32(v float32) { w.U32(math.Float32bits(v)) }
func (w *W) F64(v float64) { w.U64(math.Float64bits(v)) }
func (w *W) Bytes(p []byte) { w.b = append(w.b, p...) }
func (w *W) Str(s string) {
	if len(s) > 4000 {
		s = s[:4000]
		for !utf8.ValidString(s) {
			s = s[:len(s)-1]
		}
	}
	w.U16(uint16(len(s)))
	w.b = append(w.b, s...)
}

type R struct {
	b   []byte
	p   int
	Bad bool
}

func (r *R) need(n int) bool {
	if r.Bad || r.p+n > len(r.b) {
		r.Bad = true
		return false
	}
	return true
}
func (r *R) U8() uint8 {
	if !r.need(1) {
		return 0
	}
	v := r.b[r.p]
	r.p++
	return v
}
func (r *R) U16() uint16 {
	if !r.need(2) {
		return 0
	}
	v := binary.LittleEndian.Uint16(r.b[r.p:])
	r.p += 2
	return v
}
func (r *R) U32() uint32 {
	if !r.need(4) {
		return 0
	}
	v := binary.LittleEndian.Uint32(r.b[r.p:])
	r.p += 4
	return v
}
func (r *R) U64() uint64 {
	if !r.need(8) {
		return 0
	}
	v := binary.LittleEndian.Uint64(r.b[r.p:])
	r.p += 8
	return v
}
func (r *R) F32() float32 { return math.Float32frombits(r.U32()) }
func (r *R) Raw(n int) []byte {
	if !r.need(n) {
		return nil
	}
	v := r.b[r.p : r.p+n]
	r.p += n
	return v
}
func (r *R) Rest() []byte { v := r.b[r.p:]; r.p = len(r.b); return v }
func (r *R) Str() string {
	n := int(r.U16())
	b := r.Raw(n)
	if b == nil || !utf8.Valid(b) {
		r.Bad = true
		return ""
	}
	return string(b)
}
