// Manchester Ride dedicated server.
//
// One small program with no dependencies: accounts, text chat, voice chat relay, ride-state relay,
// a Minecraft-style console, and game updates served to players - all over one UDP port, so it
// works through a Playit.gg UDP tunnel. Build: Tools/server/build_server.bat (Linux + Windows).
package main

import (
	"bufio"
	"flag"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

func main() {
	dirFlag := flag.String("dir", "", "server folder (default: the folder this program is in)")
	portFlag := flag.Int("port", 0, "UDP port (overrides server-port in server.properties)")
	flag.Parse()

	dir := *dirFlag
	if dir == "" {
		exe, err := os.Executable()
		if err == nil {
			dir = filepath.Dir(exe)
		} else {
			dir = "."
		}
	}
	dir, _ = filepath.Abs(dir)
	os.MkdirAll(filepath.Join(dir, "updates"), 0755)
	openLog(filepath.Join(dir, "logs"))
	Info("Starting Manchester Ride server %s in %s", serverVersion, dir)

	cfg, err := loadConfig(filepath.Join(dir, "server.properties"))
	if err != nil {
		Error("server.properties: %v", err)
		os.Exit(1)
	}
	store, err := loadStore(dir)
	if err != nil {
		Error("Could not read the account/ban lists: %v", err)
		os.Exit(1)
	}
	store.SaveLists() // creates empty ops.json etc. so they're easy to find
	id, created, err := loadOrCreateIdentity(filepath.Join(dir, "server-identity.key"))
	if err != nil {
		Error("server-identity.key: %v", err)
		os.Exit(1)
	}
	if created {
		Info("Created a new server identity key (server-identity.key). Keep it if you move the server.")
	}
	pub := id.PublicKey().Bytes()

	port := cfg.Int("server-port")
	if *portFlag > 0 {
		port = *portFlag
	}
	addr := &net.UDPAddr{IP: net.ParseIP(cfg.Str("server-ip")), Port: port}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		Error("Could not open UDP port %d: %v", port, err)
		Error("Is another copy of the server already running?")
		os.Exit(1)
	}
	conn.SetReadBuffer(4 << 20)
	conn.SetWriteBuffer(4 << 20)

	s := &Server{dir: dir, cfg: cfg, store: store, conn: conn, identity: id, pub: pub,
		sessions: map[uint32]*Session{}, players: map[uint16]*Player{},
		inbox: make(chan packet, 8192), events: make(chan func(), 1024), quit: make(chan struct{}),
		started: time.Now(), weather: -1, hours: -1, fails: map[string][]time.Time{}, port: port}

	Info("Listening on UDP %s (point your Playit.gg UDP tunnel at port %d)", conn.LocalAddr(), port)
	Info("Server key fingerprint: %s", fingerprint(pub))
	if len(store.Ops) == 0 {
		Info("No operators yet. Join from the game to make your account, then type: op <yourname>")
	}
	if path, err := findUpdateFile(filepath.Join(dir, "updates")); err == nil {
		if u, err := loadUpdate(path); err == nil {
			s.update = u
			Info("Offering game update %s (build %d) from %s", u.Version, u.Build, u.Name)
		} else {
			Warn("updates/%s: %v", filepath.Base(path), err)
		}
	}
	Info("Done! Type \"help\" for commands.")

	sig := make(chan os.Signal, 2)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sig
		s.post(s.shutdown)
	}()

	go s.readLoop()
	go func() {
		in := bufio.NewScanner(os.Stdin)
		for in.Scan() {
			line := strings.TrimSpace(strings.TrimPrefix(in.Text(), "\xef\xbb\xbf"))
			if line != "" {
				s.post(func() { s.runCommand(nil, line) })
			}
		}
		// stdin closed (e.g. running as a service): keep serving without a console.
	}()
	s.loop()
}
