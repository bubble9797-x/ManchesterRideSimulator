package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// Minecraft-style log lines: [14:03:22 INFO]: message  (console + logs/latest.log)

var logMu sync.Mutex
var logFile io.WriteCloser

func openLog(dir string) {
	os.MkdirAll(dir, 0755)
	latest := filepath.Join(dir, "latest.log")
	if st, err := os.Stat(latest); err == nil {
		base := st.ModTime().Format("2006-01-02")
		for i := 1; ; i++ {
			p := filepath.Join(dir, fmt.Sprintf("%s-%d.log", base, i))
			if _, err := os.Stat(p); os.IsNotExist(err) {
				os.Rename(latest, p)
				break
			}
		}
	}
	if f, err := os.Create(latest); err == nil {
		logFile = f
	}
}

func logLine(level, format string, args ...any) {
	line := fmt.Sprintf("[%s %s]: %s\n", time.Now().Format("15:04:05"), level, fmt.Sprintf(format, args...))
	logMu.Lock()
	os.Stdout.WriteString(line)
	if logFile != nil {
		logFile.Write([]byte(line))
	}
	logMu.Unlock()
}

func Info(f string, a ...any)  { logLine("INFO", f, a...) }
func Warn(f string, a ...any)  { logLine("WARN", f, a...) }
func Error(f string, a ...any) { logLine("ERROR", f, a...) }
