package main

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Commands work the same from the server console (no slash) and from in-game chat (/command).
// p == nil means the console, which may do everything.

type cmdInfo struct {
	usage string
	help  string
	op    bool
}

var commandHelp = map[string]cmdInfo{
	"help":      {"help", "list commands", false},
	"list":      {"list", "who is online", false},
	"msg":       {"msg <player> <message>", "private message (also tell, w; r replies)", false},
	"me":        {"me <action>", "describe what you're doing", false},
	"tp":        {"tp <player> | tp <player> <to player> | tp <lat> <lon>", "teleport", false},
	"say":       {"say <message>", "announce to everyone", true},
	"kick":      {"kick <player> [reason]", "remove a player", true},
	"ban":       {"ban <player> [reason]", "ban an account", true},
	"pardon":    {"pardon <player>", "unban", true},
	"banlist":   {"banlist", "show bans", true},
	"op":        {"op <player>", "make an operator (admin)", true},
	"deop":      {"deop <player>", "remove operator", true},
	"whitelist": {"whitelist on|off|add|remove|list [player]", "only listed accounts may join", true},
	"weather":   {"weather clear|cloudy|overcast|rain|storm|fog", "set everyone's weather", true},
	"time":      {"time set <0-24> | time day|noon|night|midnight", "set everyone's time of day", true},
	"motd":      {"motd [message]", "show or set the join message", true},
	"update":    {"update [status|off]", "offer the newest installer in updates/ to players", true},
	"accounts":  {"accounts list | delete <name> | password <name> <new> | logout <name>", "manage accounts (console only)", true},
	"status":    {"status", "server health", true},
	"reload":    {"reload", "re-read server.properties and the lists", true},
	"save":      {"save", "write everything to disk", true},
	"stop":      {"stop", "shut the server down", true},
	"version":   {"version", "server and game versions", false},
}

const serverVersion = "1.0.0"

func (s *Server) reply(p *Player, text string) {
	if p == nil {
		Info("%s", text)
	} else {
		s.tell(p, ChatSystem, "", text)
	}
}
func (s *Server) fail(p *Player, text string) {
	if p == nil {
		Warn("%s", text)
	} else {
		s.tell(p, ChatError, "", text)
	}
}

func senderName(p *Player) string {
	if p == nil {
		return "Server"
	}
	return p.name
}

func (s *Server) findPlayer(name string) *Player {
	var prefix *Player
	n := 0
	for _, p := range s.players {
		if strings.EqualFold(p.name, name) {
			return p
		}
		if strings.HasPrefix(strings.ToLower(p.name), strings.ToLower(name)) {
			prefix = p
			n++
		}
	}
	if n == 1 {
		return prefix // "tp jo" finds "JohnSmith" if nobody else matches
	}
	return nil
}

func (s *Server) accountName(name string) (string, bool) {
	if a := s.store.Accounts[key(name)]; a != nil {
		return a.Name, true
	}
	return name, false
}

func (s *Server) runCommand(p *Player, line string) {
	line = strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(line), "/"))
	if line == "" {
		return
	}
	args := strings.Fields(line)
	cmd := strings.ToLower(args[0])
	args = args[1:]
	// rest(n): the text after the command and its first n words, spacing kept ("say hello  there").
	rest := func(from int) string {
		t := line
		for i := 0; i <= from; i++ {
			t = strings.TrimLeft(t, " \t")
			j := strings.IndexAny(t, " \t")
			if j < 0 {
				return ""
			}
			t = t[j:]
		}
		return strings.TrimSpace(t)
	}
	switch cmd {
	case "tell", "w", "whisper":
		cmd = "msg"
	case "save-all":
		cmd = "save"
	case "?":
		cmd = "help"
	case "unban":
		cmd = "pardon"
	}
	info, known := commandHelp[cmd]
	if cmd != "r" && !known {
		s.fail(p, "Unknown command \""+cmd+"\". Type "+slash(p)+"help")
		return
	}
	isOp := p == nil || p.op
	if known && info.op && !isOp {
		s.fail(p, "You need to be an operator to use "+slash(p)+cmd+".")
		return
	}
	usage := func() { s.fail(p, "Usage: "+slash(p)+info.usage) }

	switch cmd {
	case "help":
		names := make([]string, 0, len(commandHelp))
		for k, v := range commandHelp {
			if (!v.op || isOp) && !(k == "accounts" && p != nil) {
				names = append(names, k)
			}
		}
		sort.Strings(names)
		for _, k := range names {
			s.reply(p, fmt.Sprintf("%s%s - %s", slash(p), commandHelp[k].usage, commandHelp[k].help))
		}

	case "list":
		names := make([]string, 0, len(s.players))
		for _, o := range s.players {
			n := o.name
			if o.op {
				n += " (op)"
			}
			names = append(names, n)
		}
		sort.Strings(names)
		s.reply(p, fmt.Sprintf("There are %d of a max of %d riders online: %s", len(names), s.cfg.Int("max-players"), strings.Join(names, ", ")))

	case "msg", "r":
		var to *Player
		var text string
		if cmd == "r" {
			if p == nil || p.replyTo == "" {
				s.fail(p, "Nobody to reply to.")
				return
			}
			to, text = s.findPlayer(p.replyTo), rest(0)
		} else {
			if len(args) < 2 {
				usage()
				return
			}
			to, text = s.findPlayer(args[0]), rest(1)
		}
		if to == nil {
			s.fail(p, "That player is not online.")
			return
		}
		s.tell(to, ChatPrivate, senderName(p)+" -> you", text)
		to.replyTo = senderName(p)
		if p != nil {
			s.tell(p, ChatPrivate, "you -> "+to.name, text)
		} else {
			Info("[you -> %s] %s", to.name, text)
		}

	case "me":
		if len(args) == 0 {
			usage()
			return
		}
		Info("* %s %s", senderName(p), rest(0))
		s.broadcastChat(ChatAction, senderName(p), rest(0))

	case "say":
		if len(args) == 0 {
			usage()
			return
		}
		Info("[%s] %s", senderName(p), rest(0))
		s.broadcastChat(ChatServer, senderName(p), rest(0))

	case "tp":
		s.cmdTeleport(p, args, isOp, usage)

	case "kick":
		if len(args) < 1 {
			usage()
			return
		}
		t := s.findPlayer(args[0])
		if t == nil {
			s.fail(p, "That player is not online.")
			return
		}
		reason := rest(1)
		if reason == "" {
			reason = "Kicked by an operator."
		}
		s.kick(t, reason)
		s.reply(p, "Kicked "+t.name+": "+reason)

	case "ban":
		if len(args) < 1 {
			usage()
			return
		}
		name, exists := s.accountName(args[0])
		if !exists {
			if t := s.findPlayer(args[0]); t != nil {
				name, exists = t.name, true
			}
		}
		if !exists {
			s.fail(p, "No account called "+args[0]+".")
			return
		}
		if s.store.BanOf(name) != nil {
			s.fail(p, name+" is already banned.")
			return
		}
		reason := rest(1)
		s.store.Bans = append(s.store.Bans, Ban{Name: name, Reason: reason, By: senderName(p), Created: time.Now()})
		s.store.SaveLists()
		for _, o := range s.players {
			if strings.EqualFold(o.name, name) {
				msg := "You are banned from this server."
				if reason != "" {
					msg += " Reason: " + reason
				}
				s.kick(o, msg)
			}
		}
		s.reply(p, "Banned "+name+".")

	case "pardon":
		if len(args) < 1 {
			usage()
			return
		}
		n := 0
		keep := s.store.Bans[:0]
		for _, b := range s.store.Bans {
			if strings.EqualFold(b.Name, args[0]) {
				n++
				continue
			}
			keep = append(keep, b)
		}
		s.store.Bans = keep
		s.store.SaveLists()
		if n == 0 {
			s.fail(p, args[0]+" is not banned.")
		} else {
			s.reply(p, "Unbanned "+args[0]+".")
		}

	case "banlist":
		if len(s.store.Bans) == 0 {
			s.reply(p, "Nobody is banned.")
		}
		for _, b := range s.store.Bans {
			s.reply(p, fmt.Sprintf("%s - banned %s by %s: %s", b.Name, b.Created.Format("2 Jan 2006"), b.By, b.Reason))
		}

	case "op", "deop":
		if len(args) < 1 {
			usage()
			return
		}
		name, exists := s.accountName(args[0])
		if !exists {
			s.fail(p, "No account called "+args[0]+". They need to join once to make one.")
			return
		}
		if cmd == "op" {
			if s.store.IsOp(name) {
				s.fail(p, name+" is already an operator.")
				return
			}
			s.store.Ops = append(s.store.Ops, name)
		} else {
			var found bool
			s.store.Ops, found = removeName(s.store.Ops, name)
			if !found {
				s.fail(p, name+" is not an operator.")
				return
			}
		}
		s.store.SaveLists()
		for _, o := range s.players {
			if strings.EqualFold(o.name, name) {
				o.op = cmd == "op"
				s.broadcastReliable(s.playerMsg(o), nil)
				if o.op {
					s.tell(o, ChatSystem, "", "You are now a server operator. Type /help for commands.")
				} else {
					s.tell(o, ChatSystem, "", "You are no longer a server operator.")
				}
			}
		}
		if cmd == "op" {
			s.reply(p, "Made "+name+" a server operator.")
		} else {
			s.reply(p, name+" is no longer a server operator.")
		}

	case "whitelist":
		if len(args) < 1 {
			usage()
			return
		}
		switch strings.ToLower(args[0]) {
		case "on", "off":
			s.cfg.Set("white-list", strconv.FormatBool(strings.ToLower(args[0]) == "on"))
			s.cfg.Save()
			s.reply(p, "Whitelist is now "+strings.ToLower(args[0])+".")
		case "add", "remove":
			if len(args) < 2 {
				usage()
				return
			}
			name, _ := s.accountName(args[1])
			if strings.ToLower(args[0]) == "add" {
				if !hasName(s.store.Whitelist, name) {
					s.store.Whitelist = append(s.store.Whitelist, name)
				}
				s.reply(p, "Added "+name+" to the whitelist.")
			} else {
				s.store.Whitelist, _ = removeName(s.store.Whitelist, name)
				s.reply(p, "Removed "+name+" from the whitelist.")
			}
			s.store.SaveLists()
		case "list":
			state := "off"
			if s.cfg.Bool("white-list") {
				state = "on"
			}
			s.reply(p, fmt.Sprintf("Whitelist (%s): %s", state, strings.Join(s.store.Whitelist, ", ")))
		default:
			usage()
		}

	case "weather":
		if len(args) < 1 {
			usage()
			return
		}
		w := map[string]int{"clear": 0, "sun": 0, "cloudy": 1, "overcast": 2, "rain": 3, "drizzle": 3, "storm": 4, "thunder": 4, "heavyrain": 4, "fog": 5}
		v, ok := w[strings.ToLower(args[0])]
		if !ok {
			usage()
			return
		}
		s.weather = v
		s.broadcastReliable(s.envMsg(), nil)
		s.reply(p, "Weather set to "+strings.ToLower(args[0])+".")

	case "time":
		if len(args) < 1 {
			usage()
			return
		}
		named := map[string]float32{"day": 9, "morning": 7, "noon": 12, "evening": 18, "night": 22, "midnight": 0, "sunrise": 6}
		var h float32
		if v, ok := named[strings.ToLower(args[0])]; ok {
			h = v
		} else if strings.ToLower(args[0]) == "set" && len(args) >= 2 {
			if v, ok := named[strings.ToLower(args[1])]; ok {
				h = v
			} else {
				f, err := strconv.ParseFloat(strings.Replace(args[1], ":", ".", 1), 32)
				if err != nil || f < 0 || f > 24 {
					usage()
					return
				}
				if strings.Contains(args[1], ":") { // 18:30
					f = math.Floor(f) + (f-math.Floor(f))*100/60
				}
				h = float32(f)
			}
		} else {
			usage()
			return
		}
		s.hours = float32(math.Mod(float64(h), 24))
		s.broadcastReliable(s.envMsg(), nil)
		s.reply(p, fmt.Sprintf("Time set to %02d:%02d.", int(s.hours), int((s.hours-float32(int(s.hours)))*60)))

	case "motd":
		if len(args) == 0 {
			s.reply(p, "MOTD: "+s.cfg.Str("motd"))
			return
		}
		s.cfg.Set("motd", rest(0))
		s.cfg.Save()
		s.reply(p, "MOTD set.")

	case "update":
		s.cmdUpdate(p, args)

	case "accounts":
		if p != nil {
			s.fail(p, "Accounts can only be managed from the server console.")
			return
		}
		s.cmdAccounts(args, usage)

	case "status":
		up := time.Since(s.started).Round(time.Second)
		s.reply(p, fmt.Sprintf("Up %s. %d/%d riders online, %d connections, %d accounts.", up, len(s.players), s.cfg.Int("max-players"), len(s.sessions), len(s.store.Accounts)))
		s.reply(p, fmt.Sprintf("Traffic: %.1f MB in, %.1f MB out. UDP port %d.", float64(s.bytesIn)/1e6, float64(s.bytesOut)/1e6, s.port))
		if s.update != nil {
			s.reply(p, fmt.Sprintf("Game update offered: %s (build %d), %s, %.0f MB.", s.update.Version, s.update.Build, s.update.Name, float64(s.update.Size)/1048576))
		} else {
			s.reply(p, "No game update loaded (any game version can join).")
		}
		for _, o := range s.players {
			s.reply(p, fmt.Sprintf("  %s: ping %.0f ms, game %s, %s", o.name, o.sess.ch.srtt, o.sess.version, s.addrOf(o.sess)))
		}

	case "reload":
		if c, err := loadConfig(s.cfg.path); err == nil {
			s.cfg = c
		} else {
			s.fail(p, "server.properties: "+err.Error())
		}
		if st, err := loadStore(s.dir); err == nil {
			st.Accounts = s.store.Accounts // accounts stay as they are in memory (they change constantly)
			s.store = st
		} else {
			s.fail(p, "Lists: "+err.Error())
		}
		s.reply(p, "Reloaded server.properties, ops, bans and whitelist.")

	case "save":
		s.store.SaveAccounts()
		s.store.SaveLists()
		s.cfg.Save()
		s.reply(p, "Saved.")

	case "version":
		gv := "any"
		if s.update != nil {
			gv = fmt.Sprintf("%s (build %d)", s.update.Version, s.update.Build)
		}
		s.reply(p, fmt.Sprintf("Manchester Ride server %s, network protocol %d, game version offered: %s", serverVersion, ProtoVersion, gv))

	case "stop":
		s.reply(p, "Stopping the server...")
		s.shutdown()
	}
}

func slash(p *Player) string {
	if p == nil {
		return ""
	}
	return "/"
}

func stateOf(p *Player) (x, y, z, yaw float64) {
	r := &R{b: p.state[:]}
	x, y, z = float64(r.F32()), float64(r.F32()), float64(r.F32())
	yaw = float64(r.U16()) * 360 / 65536
	return
}

func teleportMsg(mode uint8, a, b, c float64, yaw float32) []byte {
	w := &W{}
	w.U8(STeleport)
	w.U8(mode)
	w.F64(a)
	w.F64(b)
	w.F64(c)
	w.F32(yaw)
	return w.b
}

func (s *Server) cmdTeleport(p *Player, args []string, isOp bool, usage func()) {
	if !isOp && !s.cfg.Bool("allow-player-teleport") {
		s.fail(p, "Teleporting is switched off on this server.")
		return
	}
	isNum := func(a string) bool { _, err := strconv.ParseFloat(a, 64); return err == nil }
	who, rest := p, args
	// tp <player> <...> moves someone else: operators only (and the console, which has no rider)
	if len(args) == 3 || (len(args) == 2 && !isNum(args[0])) {
		if !isOp {
			s.fail(p, "Only operators can teleport other riders.")
			return
		}
		who = s.findPlayer(args[0])
		if who == nil {
			s.fail(p, args[0]+" is not online.")
			return
		}
		rest = args[1:]
	}
	if who == nil {
		s.fail(p, "The console has no rider: use tp <player> <to player> or tp <player> <lat> <lon>.")
		return
	}
	switch {
	case len(rest) == 1:
		to := s.findPlayer(rest[0])
		if to == nil || !to.hasPos {
			s.fail(p, rest[0]+" is not online (or hasn't spawned yet).")
			return
		}
		if to == who {
			s.fail(p, "That's you.")
			return
		}
		x, y, z, yaw := stateOf(to)
		// Land just behind them, facing the same way.
		rad := yaw * math.Pi / 180
		who.sess.ch.Reliable(teleportMsg(0, x-math.Cos(rad)*400, y-math.Sin(rad)*400, z+50, float32(yaw)))
		s.reply(p, fmt.Sprintf("Teleported %s to %s.", who.name, to.name))
		if who != p {
			s.tell(who, ChatSystem, "", "You were teleported to "+to.name+".")
		}
	case len(rest) == 2 && isNum(rest[0]) && isNum(rest[1]):
		lat, _ := strconv.ParseFloat(rest[0], 64)
		lon, _ := strconv.ParseFloat(rest[1], 64)
		if lat < 53.2 || lat > 53.8 || lon < -2.8 || lon > -1.8 {
			s.fail(p, "That's outside Greater Manchester (try e.g. tp 53.4808 -2.2426 for Piccadilly).")
			return
		}
		who.sess.ch.Reliable(teleportMsg(1, lat, lon, 0, -1))
		s.reply(p, fmt.Sprintf("Teleported %s to %.5f, %.5f.", who.name, lat, lon))
	default:
		usage()
	}
}

func (s *Server) cmdUpdate(p *Player, args []string) {
	sub := ""
	if len(args) > 0 {
		sub = strings.ToLower(args[0])
	}
	switch sub {
	case "status":
		if s.update == nil {
			s.reply(p, "No update loaded. Copy ManchesterRide-Setup.exe into "+s.dir+"/updates and type update.")
		} else {
			s.reply(p, fmt.Sprintf("Offering %s (build %d): %s, %.0f MB, sha256 %x", s.update.Version, s.update.Build, s.update.Name, float64(s.update.Size)/1048576, s.update.Sha[:8]))
		}
	case "off":
		s.update.Close()
		s.update = nil
		s.reply(p, "Stopped offering game updates. Any game version can join.")
	case "", "load", "reload":
		if s.updBusy {
			s.fail(p, "Already loading an update, wait a moment.")
			return
		}
		path, err := findUpdateFile(s.dir + "/updates")
		if err != nil {
			s.fail(p, err.Error())
			return
		}
		s.updBusy = true
		s.reply(p, "Checking "+path+" ...")
		go func() {
			u, err := loadUpdate(path)
			s.post(func() { s.updBusy = false; s.applyUpdate(p, u, err) })
		}()
	default:
		s.fail(p, "Usage: "+slash(p)+commandHelp["update"].usage)
	}
}

func (s *Server) applyUpdate(p *Player, u *Update, err error) {
	if err != nil {
		s.fail(p, "Can't use that update: "+err.Error())
		return
	}
	if s.update != nil && u.Build < s.update.Build {
		s.fail(p, fmt.Sprintf("%s is build %d, older than the build %d already offered. Not changed.", u.Name, u.Build, s.update.Build))
		u.Close()
		return
	}
	s.update.Close()
	s.update = u
	msg := fmt.Sprintf("Game update ready: version %s (build %d), %.0f MB. Players on older versions get it when they join.", u.Version, u.Build, float64(u.Size)/1048576)
	if p != nil {
		s.reply(p, msg)
	}
	Info("%s", msg)
	w := &W{}
	w.U8(SUpdate)
	w.Str(u.Version)
	w.U32(u.Build)
	for _, o := range s.players {
		if o.sess.build < u.Build {
			o.sess.ch.Reliable(w.b)
			s.tell(o, ChatSystem, "", "Game version "+u.Version+" is on the server. Leave and rejoin the server to install it.")
		}
	}
}

func (s *Server) cmdAccounts(args []string, usage func()) {
	if len(args) == 0 {
		usage()
		return
	}
	switch strings.ToLower(args[0]) {
	case "list":
		names := make([]string, 0, len(s.store.Accounts))
		for _, a := range s.store.Accounts {
			names = append(names, a.Name)
		}
		sort.Strings(names)
		Info("%d accounts: %s", len(names), strings.Join(names, ", "))
	case "delete":
		if len(args) < 2 {
			usage()
			return
		}
		k := key(args[1])
		if s.store.Accounts[k] == nil {
			Warn("No account called %s.", args[1])
			return
		}
		for _, o := range s.players {
			if key(o.name) == k {
				s.kick(o, "Your account was deleted.")
			}
		}
		delete(s.store.Accounts, k)
		s.store.SaveAccounts()
		Info("Deleted account %s.", args[1])
	case "password":
		if len(args) < 3 {
			usage()
			return
		}
		k := key(args[1])
		if s.store.Accounts[k] == nil {
			Warn("No account called %s.", args[1])
			return
		}
		pw := args[2]
		if l := len([]rune(pw)); l < 6 || l > 64 {
			Warn("Passwords need 6 to 64 characters.")
			return
		}
		go func() {
			salt, hash := newPasswordRecord(pw)
			s.post(func() {
				if a := s.store.Accounts[k]; a != nil {
					a.Salt, a.Hash, a.Iter, a.Tokens = salt, hash, pbkdfIterations, nil
					s.store.SaveAccounts()
					Info("Password changed for %s (saved logins on their PCs were signed out).", a.Name)
				}
			})
		}()
	case "logout":
		if len(args) < 2 {
			usage()
			return
		}
		if a := s.store.Accounts[key(args[1])]; a != nil {
			a.Tokens = nil
			s.store.SaveAccounts()
			Info("Signed %s out on every PC.", a.Name)
		} else {
			Warn("No account called %s.", args[1])
		}
	default:
		usage()
	}
}

func (s *Server) shutdown() {
	if s.stopping {
		return
	}
	s.stopping = true
	for _, p := range s.players {
		s.kick(p, "The server has closed.")
	}
	now := time.Now()
	for _, sess := range s.sessions {
		s.flush(sess, now)
	}
	s.store.SaveAccounts()
	s.store.SaveLists()
	Info("Server stopped.")
	go func() {
		time.Sleep(500 * time.Millisecond)
		os.Exit(0)
	}()
}
