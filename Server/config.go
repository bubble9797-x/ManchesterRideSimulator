package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// server.properties, Minecraft style: key=value, # comments. Missing keys are added with their defaults.

type Config struct {
	path  string
	vals  map[string]string
	order []string
}

var configDefaults = []struct{ key, val, help string }{
	{"server-name", "Manchester Ride Server", "Name shown in the game's multiplayer screen"},
	{"server-ip", "0.0.0.0", "Address to listen on (0.0.0.0 = all)"},
	{"server-port", "9010", "UDP port. Your Playit.gg UDP tunnel must point at this port on this machine"},
	{"max-players", "16", "Most riders online at once"},
	{"motd", "Welcome to the Manchester Ride server!", "Message of the day, shown when you join"},
	{"allow-registration", "true", "Let new players create accounts from the game"},
	{"white-list", "false", "Only accounts on whitelist.json can join"},
	{"allow-player-teleport", "true", "Let everyone use /tp to riders (ops can always)"},
	{"voice-chat", "true", "Relay voice chat"},
	{"update-rate-kbps", "3000", "Fastest a player downloads a game update from this server (KB/s per player)"},
	{"login-timeout-days", "60", "How long the game stays logged in without typing the password"},
}

func loadConfig(path string) (*Config, error) {
	c := &Config{path: path, vals: map[string]string{}}
	if f, err := os.Open(path); err == nil {
		sc := bufio.NewScanner(f)
		for sc.Scan() {
			line := strings.TrimSpace(sc.Text())
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			k, v, ok := strings.Cut(line, "=")
			if !ok {
				continue
			}
			k = strings.TrimSpace(k)
			if _, seen := c.vals[k]; !seen {
				c.order = append(c.order, k)
			}
			c.vals[k] = strings.TrimSpace(v)
		}
		f.Close()
	}
	changed := false
	for _, d := range configDefaults {
		if _, ok := c.vals[d.key]; !ok {
			c.vals[d.key] = d.val
			c.order = append(c.order, d.key)
			changed = true
		}
	}
	if changed {
		return c, c.Save()
	}
	return c, nil
}

func (c *Config) Save() error {
	var b strings.Builder
	b.WriteString("# Manchester Ride dedicated server settings. Type 'reload' in the console after editing.\n")
	help := map[string]string{}
	for _, d := range configDefaults {
		help[d.key] = d.help
	}
	for _, k := range c.order {
		if h := help[k]; h != "" {
			fmt.Fprintf(&b, "# %s\n", h)
		}
		fmt.Fprintf(&b, "%s=%s\n", k, c.vals[k])
	}
	tmp := c.path + ".tmp"
	if err := os.WriteFile(tmp, []byte(b.String()), 0644); err != nil {
		return err
	}
	return os.Rename(tmp, c.path)
}

func (c *Config) Str(k string) string { return c.vals[k] }
func (c *Config) Int(k string) int {
	v, err := strconv.Atoi(c.vals[k])
	if err != nil {
		for _, d := range configDefaults {
			if d.key == k {
				v, _ = strconv.Atoi(d.val)
			}
		}
	}
	return v
}
func (c *Config) Bool(k string) bool { return strings.EqualFold(c.vals[k], "true") }
func (c *Config) Set(k, v string) {
	if _, ok := c.vals[k]; !ok {
		c.order = append(c.order, k)
	}
	c.vals[k] = v
}
