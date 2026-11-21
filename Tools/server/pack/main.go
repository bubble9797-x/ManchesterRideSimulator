// Packs the server folder into a .tar.gz with Linux file permissions (Windows' tar can't set the executable bit).
//   go run . <folder> <out.tar.gz> <top-level name>
package main

import (
	"archive/tar"
	"compress/gzip"
	"io"
	"os"
	"path/filepath"
	"strings"
)

func main() {
	src, out, top := os.Args[1], os.Args[2], os.Args[3]
	f, err := os.Create(out)
	if err != nil {
		panic(err)
	}
	gz, _ := gzip.NewWriterLevel(f, gzip.BestCompression)
	tw := tar.NewWriter(gz)
	err = filepath.Walk(src, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		name := top
		if rel != "." {
			name = top + "/" + filepath.ToSlash(rel)
		}
		h, _ := tar.FileInfoHeader(info, "")
		h.Name = name
		h.Uid, h.Gid, h.Uname, h.Gname = 0, 0, "", ""
		switch {
		case info.IsDir():
			h.Name += "/"
			h.Mode = 0755
		case strings.HasSuffix(name, ".sh") || strings.HasPrefix(filepath.Base(name), "manchesterride-server") && filepath.Ext(name) == "":
			h.Mode = 0755
		default:
			h.Mode = 0644
		}
		if err := tw.WriteHeader(h); err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		r, err := os.Open(p)
		if err != nil {
			return err
		}
		defer r.Close()
		_, err = io.Copy(tw, r)
		return err
	})
	if err != nil {
		panic(err)
	}
	tw.Close()
	gz.Close()
	f.Close()
}
