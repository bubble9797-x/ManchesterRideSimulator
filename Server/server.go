package main

import (
	"bytes"
	"crypto/ecdh"
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"math"
	"net"
	"strings"
	"time"
)

type packet struct {
	data []byte
	addr *net.UDPAddr
}

type Player struct {
	id      uint16
	sess    *Session
	acc     *Account
	name    string
	op      bool
	vehicle string
	paint   uint32
	jacket  uint32
	helmet  uint32
	look    string
	state   [StateSize]byte
	hasPos  bool
	dirty   bool
	chatLog []time.Time
	voiceN  int
	voiceAt time.Time
	replyTo string
	joined  time.Time
}

type Session struct {
	link       SecureLink
	addr       *net.UDPAddr
	realAddr   string // from a PROXY protocol header, when the tunnel sends one
	ch         *Channel
	created    time.Time
	active     time.Time // last login attempt or update download request
	lastRecv   time.Time
	player     *Player
	loginBusy  bool
	loginTimes []time.Time
	build      uint32
	version    string
	closing    bool
	closeAt    time.Time
	bucket     float64
	bucketAt   time.Time
	chunksSent uint64
}

type Server struct {
	dir       string
	cfg       *Config
	store     *Store
	conn      *net.UDPConn
	identity  *ecdh.PrivateKey
	pub       []byte
	sessions  map[uint32]*Session
	players   map[uint16]*Player
	nextID    uint16
	inbox     chan packet
	events    chan func()
	quit      chan struct{}
	update    *Update
	updBusy   bool
	started   time.Time
	weather   int
	hours     float32
	fails     map[string][]time.Time
	stopping  bool
	bytesIn   uint64
	bytesOut  uint64
	sawProxy  bool
	chunkBuf  []byte
	lastPings time.Time
	port      int
}

func (s *Server) post(f func()) { s.events <- f }

func (s *Server) loop() {
	tick := time.NewTicker(time.Second / 30)
	defer tick.Stop()
	for {
		select {
		case p := <-s.inbox:
			s.handlePacket(p.data, p.addr)
		case f := <-s.events:
			f()
		case <-tick.C:
			s.tick()
		case <-s.quit:
			return
		}
	}
}

func (s *Server) readLoop() {
	buf := make([]byte, 2048)
	for {
		n, addr, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			if s.stopping {
				return
			}
			continue
		}
		if n == 0 {
			continue
		}
		d := make([]byte, n)
		copy(d, buf[:n])
		select {
		case s.inbox <- packet{d, addr}:
		default: // overloaded: drop rather than block the socket
		}
	}
}

func (s *Server) sendRaw(sess *Session, pkt []byte) {
	s.bytesOut += uint64(len(pkt))
	s.conn.WriteToUDP(pkt, sess.addr)
}

func (s *Server) sendPlain(sess *Session, plain []byte) { s.sendRaw(sess, sess.link.Seal(plain)) }

// ---------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------

var proxySig = []byte{0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A}

// stripProxy removes a PROXY protocol v2 header (Playit can add one when "Proxy Protocol" is switched on).
// Not needed: the server works the same with it off. With it on, the console shows players' real addresses.
func stripProxy(d []byte) ([]byte, string) {
	if len(d) < 16 || !bytes.Equal(d[:12], proxySig) {
		return d, ""
	}
	l := int(binary.BigEndian.Uint16(d[14:16]))
	if len(d) < 16+l {
		return nil, ""
	}
	real := ""
	fam := d[13] >> 4
	a := d[16 : 16+l]
	if fam == 1 && len(a) >= 12 { // IPv4
		real = fmt.Sprintf("%s:%d", net.IP(a[0:4]).String(), binary.BigEndian.Uint16(a[8:10]))
	} else if fam == 2 && len(a) >= 36 { // IPv6
		real = fmt.Sprintf("[%s]:%d", net.IP(a[0:16]).String(), binary.BigEndian.Uint16(a[32:34]))
	}
	return d[16+l:], real
}

func (s *Server) handlePacket(d []byte, addr *net.UDPAddr) {
	s.bytesIn += uint64(len(d))
	d, real := stripProxy(d)
	if len(d) == 0 {
		return
	}
	if real != "" && !s.sawProxy {
		s.sawProxy = true
		Info("PROXY protocol headers detected: real player addresses will be shown")
	}
	switch d[0] {
	case PktHello:
		s.handleHello(d, addr, real)
	case PktData:
		if len(d) < 13+16 {
			return
		}
		sess := s.sessions[binary.LittleEndian.Uint32(d[1:5])]
		if sess == nil {
			return
		}
		plain, ok := sess.link.Open(d)
		if !ok {
			return
		}
		now := time.Now()
		sess.addr = addr // follow the client if its port changes (NAT rebinding)
		if real != "" {
			sess.realAddr = real
		}
		sess.lastRecv = now
		msgs, ok := sess.ch.Receive(plain, now)
		for _, m := range msgs {
			if s.sessions[sess.link.id] != sess {
				return // dropped while handling
			}
			s.handleMsg(sess, m)
		}
		if !ok {
			s.drop(sess, "sent a broken packet")
		}
	}
}

func (s *Server) reject(addr *net.UDPAddr, reason uint8, msg string) {
	w := &W{}
	w.U8(PktReject)
	w.U8(reason)
	w.Str(msg)
	s.bytesOut += uint64(len(w.b))
	s.conn.WriteToUDP(w.b, addr)
}

func (s *Server) handleHello(d []byte, addr *net.UDPAddr, real string) {
	if len(d) < HelloSize || string(d[1:5]) != "MRS1" {
		return
	}
	if v := binary.LittleEndian.Uint16(d[5:7]); v != ProtoVersion {
		s.reject(addr, 1, fmt.Sprintf("This server uses network protocol %d and your game uses %d. Install the latest game.", ProtoVersion, v))
		return
	}
	pending := 0
	for _, x := range s.sessions {
		if x.player == nil {
			pending++
		}
	}
	if pending > 128 {
		s.reject(addr, 2, "The server is busy. Try again in a moment.")
		return
	}
	cPub, err := ecdh.P256().NewPublicKey(d[7:72])
	if err != nil {
		return
	}
	cNonce := d[72:88]
	eph, err := ecdh.P256().GenerateKey(rand.Reader)
	if err != nil {
		return
	}
	ss1, err1 := eph.ECDH(cPub)
	ss2, err2 := s.identity.ECDH(cPub)
	if err1 != nil || err2 != nil {
		return
	}
	sNonce := make([]byte, 16)
	rand.Read(sNonce)
	c2s, s2c, err := deriveKeys(ss1, ss2, cNonce, sNonce)
	if err != nil {
		return
	}
	var id uint32
	for id == 0 || s.sessions[id] != nil {
		var b [4]byte
		rand.Read(b[:])
		id = binary.LittleEndian.Uint32(b[:])
	}
	now := time.Now()
	sess := &Session{addr: addr, realAddr: real, ch: newChannel(), created: now, active: now, lastRecv: now, bucketAt: now,
		link: SecureLink{id: id, send: newGCM(s2c), recv: newGCM(c2s)}}
	s.sessions[id] = sess

	w := &W{}
	w.U8(PktHelloAck)
	w.Bytes([]byte("MRS1"))
	w.U16(ProtoVersion)
	w.Bytes(s.pub)
	w.Bytes(eph.PublicKey().Bytes())
	w.Bytes(sNonce)
	w.U32(id)
	s.bytesOut += uint64(len(w.b))
	s.conn.WriteToUDP(w.b, addr)
}

func (s *Server) addrOf(sess *Session) string {
	if sess.realAddr != "" {
		return sess.realAddr
	}
	return sess.addr.String() + " via tunnel"
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

func (s *Server) handleMsg(sess *Session, m []byte) {
	r := &R{b: m, p: 1}
	p := sess.player
	switch m[0] {
	case CLogin:
		mode := r.U8()
		user := strings.TrimSpace(r.Str())
		secret := r.Str()
		build := r.U32()
		ver := r.Str()
		if r.Bad || p != nil {
			return
		}
		s.handleLogin(sess, mode, user, secret, build, ver)
	case CState:
		if p == nil || len(m) != 1+StateSize {
			return
		}
		x := math.Float32frombits(binary.LittleEndian.Uint32(m[1:]))
		y := math.Float32frombits(binary.LittleEndian.Uint32(m[5:]))
		z := math.Float32frombits(binary.LittleEndian.Uint32(m[9:]))
		if !finite(x) || !finite(y) || !finite(z) || math.Abs(float64(x)) > 2e8 || math.Abs(float64(y)) > 2e8 || math.Abs(float64(z)) > 2e8 {
			return
		}
		copy(p.state[:], m[1:])
		p.hasPos = true
		p.dirty = true
	case CLook:
		if p == nil {
			return
		}
		veh, paint, jacket, helmet, look := r.Str(), r.U32(), r.U32(), r.U32(), r.Str()
		if r.Bad || len(veh) > 64 || len(look) > 160 {
			return
		}
		p.vehicle, p.paint, p.jacket, p.helmet, p.look = veh, paint, jacket, helmet, look
		s.broadcastReliable(s.playerMsg(p), nil)
	case CChat:
		if p == nil {
			return
		}
		text := strings.TrimSpace(r.Str())
		if r.Bad || text == "" {
			return
		}
		if len([]rune(text)) > 256 {
			text = string([]rune(text)[:256])
		}
		text = strings.Map(func(c rune) rune {
			if c < 32 {
				return -1
			}
			return c
		}, text)
		now := time.Now()
		keep := p.chatLog[:0]
		for _, t := range p.chatLog {
			if now.Sub(t) < 5*time.Second {
				keep = append(keep, t)
			}
		}
		p.chatLog = keep
		if len(p.chatLog) >= 6 {
			s.tell(p, ChatError, "", "You're sending messages too fast.")
			return
		}
		p.chatLog = append(p.chatLog, now)
		if strings.HasPrefix(text, "/") {
			Info("%s issued server command: %s", p.name, text)
			s.runCommand(p, text[1:])
			return
		}
		Info("<%s> %s", p.name, text)
		s.broadcastChat(ChatPlayer, p.name, text)
	case CVoice:
		if p == nil || !s.cfg.Bool("voice-chat") || len(m) < 4 || len(m) > 1100 {
			return
		}
		now := time.Now()
		if now.Sub(p.voiceAt) > time.Second {
			p.voiceAt, p.voiceN = now, 0
		}
		if p.voiceN++; p.voiceN > 80 {
			return
		}
		w := &W{}
		w.U8(SVoice)
		w.U16(p.id)
		w.Bytes(m[1:])
		for _, o := range s.players {
			if o != p {
				s.sendPlain(o.sess, o.sess.ch.Immediate(w.b))
			}
		}
	case CPing:
		t := r.U32()
		if r.Bad {
			return
		}
		w := &W{}
		w.U8(SPong)
		w.U32(t)
		s.sendPlain(sess, sess.ch.Immediate(w.b))
	case CFileReq:
		s.handleFileReq(sess, r)
	case CBye:
		s.drop(sess, "left the game")
	case CLogout:
		tok := r.Str()
		if p != nil && !r.Bad && tok != "" {
			p.acc.RemoveToken(tok)
			s.store.SaveAccounts()
		}
	}
}

func finite(f float32) bool { return !math.IsNaN(float64(f)) && !math.IsInf(float64(f), 0) }

func (s *Server) handleFileReq(sess *Session, r *R) {
	u := s.update
	if u == nil {
		return
	}
	n := int(r.U16())
	if r.Bad || n > 256 {
		return
	}
	now := time.Now()
	sess.active = now
	rate := float64(s.cfg.Int("update-rate-kbps")) * 1024
	if rate <= 0 {
		rate = 3000 * 1024
	}
	sess.bucket += now.Sub(sess.bucketAt).Seconds() * rate
	sess.bucketAt = now
	if sess.bucket > rate/4 {
		sess.bucket = rate / 4
	}
	if s.chunkBuf == nil {
		s.chunkBuf = make([]byte, ChunkSize)
	}
	for i := 0; i < n; i++ {
		idx := r.U32()
		if r.Bad {
			return
		}
		if sess.bucket < ChunkSize {
			return // over the rate: the game asks again and slows down
		}
		data := u.Chunk(idx, s.chunkBuf)
		if data == nil {
			continue
		}
		w := &W{b: make([]byte, 0, 8+len(data))}
		w.U8(SFileChunk)
		w.U32(idx)
		w.Bytes(data)
		s.sendPlain(sess, sess.ch.Immediate(w.b))
		sess.bucket -= float64(len(data))
		sess.chunksSent++
	}
}

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

func (s *Server) loginResult(sess *Session, code uint8, msg string) {
	w := &W{}
	w.U8(SLoginResult)
	w.U8(code)
	w.Str(msg)
	if code == LoginNeedUpdate && s.update != nil {
		u := s.update
		w.Str(u.Version)
		w.U32(u.Build)
		w.U64(uint64(u.Size))
		w.Bytes(u.Sha[:])
		w.U32(ChunkSize)
	}
	sess.ch.Reliable(w.b)
}

func (s *Server) handleLogin(sess *Session, mode uint8, user, secret string, build uint32, ver string) {
	if sess.loginBusy {
		return
	}
	now := time.Now()
	sess.active = now
	keep := sess.loginTimes[:0]
	for _, t := range sess.loginTimes {
		if now.Sub(t) < 30*time.Second {
			keep = append(keep, t)
		}
	}
	sess.loginTimes = append(keep, now)
	if len(sess.loginTimes) > 6 {
		s.loginResult(sess, LoginSlowDown, "Too many tries. Wait half a minute, then try again.")
		return
	}
	sess.build, sess.version = build, ver
	if u := s.update; u != nil && build < u.Build {
		Info("%s (%s) has game build %d; offering update %s (build %d)", user, s.addrOf(sess), build, u.Version, u.Build)
		s.loginResult(sess, LoginNeedUpdate, fmt.Sprintf("This server has game version %s. Your game will update now.", u.Version))
		return
	}
	if !validName.MatchString(user) {
		s.loginResult(sess, LoginBadName, "Names are 3 to 16 letters, numbers or _ (no spaces).")
		return
	}
	k := key(user)
	acc := s.store.Accounts[k]
	// Lock an account for 10 minutes after 8 wrong passwords (stops guessing).
	fl := s.fails[k][:0]
	for _, t := range s.fails[k] {
		if now.Sub(t) < 10*time.Minute {
			fl = append(fl, t)
		}
	}
	s.fails[k] = fl
	if len(fl) >= 8 && mode != 2 {
		s.loginResult(sess, LoginSlowDown, "Too many wrong passwords for this account. Wait 10 minutes.")
		return
	}
	switch mode {
	case 1: // register
		if !s.cfg.Bool("allow-registration") {
			s.loginResult(sess, LoginRegClosed, "This server isn't taking new accounts. Ask the owner to make one for you.")
			return
		}
		if acc != nil || strings.EqualFold(user, "server") || strings.EqualFold(user, "console") {
			s.loginResult(sess, LoginTaken, "That name is taken. Pick another, or log in if it's yours.")
			return
		}
		if l := len([]rune(secret)); l < 6 || l > 64 {
			s.loginResult(sess, LoginBadPassword, "Passwords need 6 to 64 characters.")
			return
		}
		sess.loginBusy = true
		go func() {
			salt, hash := newPasswordRecord(secret)
			s.post(func() {
				sess.loginBusy = false
				if s.sessions[sess.link.id] != sess {
					return
				}
				if s.store.Accounts[k] != nil {
					s.loginResult(sess, LoginTaken, "That name was just taken. Pick another.")
					return
				}
				a := &Account{Name: user, Salt: salt, Hash: hash, Iter: pbkdfIterations, Created: time.Now()}
				s.store.Accounts[k] = a
				s.store.SaveAccounts()
				Info("New account: %s (%s)", user, s.addrOf(sess))
				s.finishLogin(sess, a, true)
			})
		}()
	case 0: // password
		if acc == nil {
			s.loginResult(sess, LoginNoAccount, "No account with that name. Choose Create account to make one.")
			return
		}
		sess.loginBusy = true
		snapshot := *acc
		go func() {
			ok := checkPassword(snapshot, secret)
			s.post(func() {
				sess.loginBusy = false
				if s.sessions[sess.link.id] != sess {
					return
				}
				a := s.store.Accounts[k]
				if !ok || a == nil {
					s.fails[k] = append(s.fails[k], time.Now())
					Warn("Wrong password for %s from %s", user, s.addrOf(sess))
					s.loginResult(sess, LoginBadPassword, "Wrong name or password.")
					return
				}
				delete(s.fails, k)
				s.finishLogin(sess, a, true)
			})
		}()
	case 2: // remembered login
		if acc == nil || !acc.CheckToken(secret, now) {
			s.loginResult(sess, LoginBadToken, "Your saved login has expired. Log in again.")
			return
		}
		s.finishLogin(sess, acc, false)
	}
}

func (s *Server) onlineCount() int { return len(s.players) }

func (s *Server) finishLogin(sess *Session, a *Account, issueToken bool) {
	if b := s.store.BanOf(a.Name); b != nil {
		Info("%s tried to join but is banned (%s)", a.Name, b.Reason)
		msg := "You are banned from this server."
		if b.Reason != "" {
			msg += " Reason: " + b.Reason
		}
		s.loginResult(sess, LoginBanned, msg)
		return
	}
	op := s.store.IsOp(a.Name)
	if s.cfg.Bool("white-list") && !op && !hasName(s.store.Whitelist, a.Name) {
		s.loginResult(sess, LoginNotWhitelist, "You are not white-listed on this server.")
		return
	}
	for _, o := range s.players {
		if strings.EqualFold(o.name, a.Name) {
			s.kick(o, "You logged in from another computer.")
		}
	}
	if s.onlineCount() >= s.cfg.Int("max-players") && !op {
		s.loginResult(sess, LoginFull, fmt.Sprintf("The server is full (%d riders). Try again soon.", s.cfg.Int("max-players")))
		return
	}
	for s.nextID++; s.nextID == 0 || s.players[s.nextID] != nil; s.nextID++ {
	}
	p := &Player{id: s.nextID, sess: sess, acc: a, name: a.Name, op: op, joined: time.Now()}
	token := ""
	if issueToken {
		plain, hashed := newToken()
		days := s.cfg.Int("login-timeout-days")
		if days <= 0 {
			days = 60
		}
		a.AddToken(hashed, time.Duration(days)*24*time.Hour)
		token = plain
	}
	a.LastLogin = time.Now()
	s.store.SaveAccounts()
	sess.player = p
	s.players[p.id] = p

	w := &W{}
	w.U8(SLoginResult)
	w.U8(LoginOK)
	w.Str("Welcome, " + a.Name)
	w.U16(p.id)
	w.Str(a.Name)
	w.Str(token)
	w.U8(b2u(op))
	w.Str(s.cfg.Str("motd"))
	w.Str(s.cfg.Str("server-name"))
	w.U16(uint16(s.cfg.Int("max-players")))
	sess.ch.Reliable(w.b)
	for _, o := range s.players {
		if o != p {
			sess.ch.Reliable(s.playerMsg(o))
		}
	}
	if s.weather >= 0 || s.hours >= 0 {
		sess.ch.Reliable(s.envMsg())
	}
	s.broadcastReliable(s.playerMsg(p), p)
	Info("%s joined the game (%s, game %s)", p.name, s.addrOf(sess), sess.version)
	s.broadcastChat(ChatSystem, "", p.name+" joined the game")
}

func b2u(b bool) uint8 {
	if b {
		return 1
	}
	return 0
}

func (s *Server) playerMsg(p *Player) []byte {
	w := &W{}
	w.U8(SPlayer)
	w.U16(p.id)
	w.Str(p.name)
	w.U8(b2u(p.op))
	w.Str(p.vehicle)
	w.U32(p.paint)
	w.U32(p.jacket)
	w.U32(p.helmet)
	w.Str(p.look)
	return w.b
}

func (s *Server) envMsg() []byte {
	w := &W{}
	w.U8(SEnv)
	w.U8(uint8(int8(s.weather)))
	w.F32(s.hours)
	return w.b
}

// ---------------------------------------------------------------------------
// Sending helpers
// ---------------------------------------------------------------------------

func (s *Server) broadcastReliable(msg []byte, except *Player) {
	for _, o := range s.players {
		if o != except {
			o.sess.ch.Reliable(msg)
		}
	}
}

func chatMsg(kind uint8, from, text string) []byte {
	w := &W{}
	w.U8(SChat)
	w.U8(kind)
	w.Str(from)
	w.Str(text)
	return w.b
}

func (s *Server) broadcastChat(kind uint8, from, text string) {
	s.broadcastReliable(chatMsg(kind, from, text), nil)
}

func (s *Server) tell(p *Player, kind uint8, from, text string) {
	if p == nil {
		Info("%s", text)
		return
	}
	p.sess.ch.Reliable(chatMsg(kind, from, text))
}

func (s *Server) removePlayer(p *Player, why string) {
	if s.players[p.id] != p {
		return
	}
	delete(s.players, p.id)
	p.sess.player = nil
	w := &W{}
	w.U8(SPlayerLeave)
	w.U16(p.id)
	s.broadcastReliable(w.b, nil)
	Info("%s left the game (%s)", p.name, why)
	s.broadcastChat(ChatSystem, "", p.name+" left the game")
}

func (s *Server) drop(sess *Session, why string) {
	if sess.player != nil {
		s.removePlayer(sess.player, why)
	}
	delete(s.sessions, sess.link.id)
}

func (s *Server) kick(p *Player, reason string) {
	sess := p.sess
	w := &W{}
	w.U8(SKick)
	w.Str(reason)
	sess.ch.Reliable(w.b)
	s.removePlayer(p, "kicked: "+reason)
	sess.closing = true
	sess.closeAt = time.Now().Add(2 * time.Second)
	s.flush(sess, time.Now())
}

func (s *Server) flush(sess *Session, now time.Time) {
	for _, pk := range sess.ch.Flush(now, true) {
		s.sendPlain(sess, pk)
	}
}

// ---------------------------------------------------------------------------
// Tick (30 Hz)
// ---------------------------------------------------------------------------

func (s *Server) tick() {
	now := time.Now()
	for _, sess := range s.sessions {
		switch {
		case sess.closing && (now.After(sess.closeAt) || len(sess.ch.unacked) == 0):
			delete(s.sessions, sess.link.id)
		case now.Sub(sess.lastRecv) > 20*time.Second:
			s.drop(sess, "timed out")
		case sess.player == nil && !sess.closing && now.Sub(sess.active) > 2*time.Minute:
			delete(s.sessions, sess.link.id) // connected but not logging in or downloading
		case len(sess.ch.unacked) > 4000:
			s.drop(sess, "not responding")
		}
	}

	// Ride states: everyone who moved since the last tick, to everyone else.
	var dirty []*Player
	for _, p := range s.players {
		if p.dirty {
			dirty = append(dirty, p)
			p.dirty = false
		}
	}
	if len(dirty) > 0 {
		for _, to := range s.players {
			var w *W
			n := 0
			flushMsg := func() {
				if w != nil && n > 0 {
					w.b[1] = uint8(n)
					to.sess.ch.Unreliable(w.b)
				}
				w, n = nil, 0
			}
			for _, p := range dirty {
				if p == to {
					continue
				}
				if w == nil {
					w = &W{b: make([]byte, 2, 2+30*(2+StateSize))}
					w.b[0] = SStates
				}
				w.U16(p.id)
				w.Bytes(p.state[:])
				if n++; n == 30 {
					flushMsg()
				}
			}
			flushMsg()
		}
	}

	if now.Sub(s.lastPings) > time.Second && len(s.players) > 0 {
		s.lastPings = now
		w := &W{}
		w.U8(SPings)
		w.U8(uint8(min(len(s.players), 255)))
		i := 0
		for _, p := range s.players {
			if i++; i > 255 {
				break
			}
			w.U16(p.id)
			w.U16(uint16(min(p.sess.ch.srtt, 9999)))
		}
		for _, p := range s.players {
			p.sess.ch.Unreliable(w.b)
		}
	}

	for _, sess := range s.sessions {
		s.flush(sess, now)
	}
}
