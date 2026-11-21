package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Game updates: put the new ManchesterRide-Setup.exe in the updates/ folder and type "update".
// Players on an older build are offered it when they join, download it from this server through the
// same UDP tunnel, check its SHA-256, and the installer updates the game and restarts it.
//
// The installer ends with: [zip][version block 64][magic "MRPAYLD2"][u64 offset][u64 length]
// version block = "MRVER001" u32 build  version string (NUL padded to 52 bytes).

type Update struct {
	Path    string
	Name    string
	Size    int64
	Sha     [32]byte
	Build   uint32
	Version string
	Chunks  uint32
	f       *os.File
}

func readSetupVersion(f *os.File, size int64) (uint32, string, error) {
	if size < 24+64 {
		return 0, "", errors.New("file too small to be the game installer")
	}
	tail := make([]byte, 24+64)
	if _, err := f.ReadAt(tail, size-int64(len(tail))); err != nil {
		return 0, "", err
	}
	magic := string(tail[64 : 64+8])
	if magic == "MRPAYLD1" {
		return 0, "", errors.New("this installer is from before server updates existed (no build number in it); build a new one with Tools\\installer\\make_installer.bat")
	}
	if magic != "MRPAYLD2" || string(tail[0:8]) != "MRVER001" {
		return 0, "", errors.New("not a Manchester Ride installer (ManchesterRide-Setup.exe made by Tools\\installer)")
	}
	build := binary.LittleEndian.Uint32(tail[8:12])
	ver := string(bytes.TrimRight(tail[12:64], "\x00"))
	return build, ver, nil
}

// findUpdateFile picks the newest .exe in dir.
func findUpdateFile(dir string) (string, error) {
	os.MkdirAll(dir, 0755)
	ents, err := os.ReadDir(dir)
	if err != nil {
		return "", err
	}
	type cand struct {
		p string
		t int64
	}
	var cs []cand
	for _, e := range ents {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".exe") {
			continue
		}
		if inf, err := e.Info(); err == nil {
			cs = append(cs, cand{filepath.Join(dir, e.Name()), inf.ModTime().UnixNano()})
		}
	}
	if len(cs) == 0 {
		return "", errors.New("no .exe in " + dir + " - copy ManchesterRide-Setup.exe there first")
	}
	sort.Slice(cs, func(i, j int) bool { return cs[i].t > cs[j].t })
	return cs[0].p, nil
}

// loadUpdate opens and hashes the installer (a few seconds for ~460 MB; runs off the main loop).
func loadUpdate(path string) (*Update, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	st, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	build, ver, err := readSetupVersion(f, st.Size())
	if err != nil {
		f.Close()
		return nil, err
	}
	h := sha256.New()
	if _, err := io.Copy(h, io.NewSectionReader(f, 0, st.Size())); err != nil {
		f.Close()
		return nil, err
	}
	u := &Update{Path: path, Name: filepath.Base(path), Size: st.Size(), Build: build, Version: ver, f: f,
		Chunks: uint32((st.Size() + ChunkSize - 1) / ChunkSize)}
	copy(u.Sha[:], h.Sum(nil))
	return u, nil
}

func (u *Update) Chunk(i uint32, buf []byte) []byte {
	if i >= u.Chunks {
		return nil
	}
	off := int64(i) * ChunkSize
	n := int64(ChunkSize)
	if off+n > u.Size {
		n = u.Size - off
	}
	got, err := u.f.ReadAt(buf[:n], off)
	if err != nil && got != int(n) {
		return nil
	}
	return buf[:n]
}

func (u *Update) Close() {
	if u != nil && u.f != nil {
		u.f.Close()
	}
}
