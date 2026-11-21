package main

import (
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"os"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Accounts (accounts.json), ops.json, banned-players.json, whitelist.json - plain JSON next to the server.
// Passwords are never stored: only a salted PBKDF2-SHA256 hash. Remembered logins are random tokens, stored hashed.

const pbkdfIterations = 120000

type Token struct {
	Hash    string    `json:"hash"`
	Expires time.Time `json:"expires"`
}

type Account struct {
	Name      string    `json:"name"`
	Salt      string    `json:"salt"`
	Hash      string    `json:"hash"`
	Iter      int       `json:"iterations"`
	Created   time.Time `json:"created"`
	LastLogin time.Time `json:"lastLogin"`
	Tokens    []Token   `json:"tokens,omitempty"`
}

type Ban struct {
	Name    string    `json:"name"`
	Reason  string    `json:"reason"`
	By      string    `json:"source"`
	Created time.Time `json:"created"`
}

type Store struct {
	dir       string
	Accounts  map[string]*Account // key: lower-case name
	Ops       []string
	Bans      []Ban
	Whitelist []string
}

var validName = regexp.MustCompile(`^[A-Za-z0-9_]{3,16}$`)

func key(name string) string { return strings.ToLower(name) }

func loadJSON(path string, v any) error {
	b, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	return json.Unmarshal(b, v)
}

func saveJSON(path string, v any) error {
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, append(b, '\n'), 0600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func loadStore(dir string) (*Store, error) {
	s := &Store{dir: dir, Accounts: map[string]*Account{}}
	var accs []*Account
	if err := loadJSON(dir+"/accounts.json", &accs); err != nil {
		return nil, err
	}
	for _, a := range accs {
		s.Accounts[key(a.Name)] = a
	}
	if err := loadJSON(dir+"/ops.json", &s.Ops); err != nil {
		return nil, err
	}
	if err := loadJSON(dir+"/banned-players.json", &s.Bans); err != nil {
		return nil, err
	}
	if err := loadJSON(dir+"/whitelist.json", &s.Whitelist); err != nil {
		return nil, err
	}
	return s, nil
}

func (s *Store) SaveAccounts() {
	accs := make([]*Account, 0, len(s.Accounts))
	for _, a := range s.Accounts {
		accs = append(accs, a)
	}
	sort.Slice(accs, func(i, j int) bool { return key(accs[i].Name) < key(accs[j].Name) })
	if err := saveJSON(s.dir+"/accounts.json", accs); err != nil {
		Error("Could not save accounts.json: %v", err)
	}
}
func (s *Store) SaveLists() {
	for _, f := range []struct {
		n string
		v any
	}{{"ops.json", s.Ops}, {"banned-players.json", s.Bans}, {"whitelist.json", s.Whitelist}} {
		v := f.v
		if v == nil {
			v = []string{}
		}
		if err := saveJSON(s.dir+"/"+f.n, v); err != nil {
			Error("Could not save %s: %v", f.n, err)
		}
	}
}

func hasName(list []string, name string) bool {
	for _, n := range list {
		if strings.EqualFold(n, name) {
			return true
		}
	}
	return false
}
func removeName(list []string, name string) ([]string, bool) {
	out := list[:0]
	found := false
	for _, n := range list {
		if strings.EqualFold(n, name) {
			found = true
			continue
		}
		out = append(out, n)
	}
	return out, found
}

func (s *Store) IsOp(name string) bool { return hasName(s.Ops, name) }
func (s *Store) BanOf(name string) *Ban {
	for i := range s.Bans {
		if strings.EqualFold(s.Bans[i].Name, name) {
			return &s.Bans[i]
		}
	}
	return nil
}

// ---- password + token hashing (called off the main loop: PBKDF2 is deliberately slow) ----

func hashPassword(pw string, salt []byte, iter int) []byte {
	h, _ := pbkdf2.Key(sha256.New, pw, salt, iter, 32)
	return h
}

func newPasswordRecord(pw string) (salt, hash string) {
	sb := make([]byte, 16)
	rand.Read(sb)
	return base64.StdEncoding.EncodeToString(sb), base64.StdEncoding.EncodeToString(hashPassword(pw, sb, pbkdfIterations))
}

func checkPassword(a Account, pw string) bool {
	salt, err1 := base64.StdEncoding.DecodeString(a.Salt)
	want, err2 := base64.StdEncoding.DecodeString(a.Hash)
	if err1 != nil || err2 != nil || a.Iter <= 0 {
		return false
	}
	return subtle.ConstantTimeCompare(hashPassword(pw, salt, a.Iter), want) == 1
}

func newToken() (plain, hashed string) {
	b := make([]byte, 32)
	rand.Read(b)
	plain = base64.RawURLEncoding.EncodeToString(b)
	return plain, tokenHash(plain)
}

func tokenHash(t string) string { h := sha256.Sum256([]byte("mr-token:" + t)); return hex.EncodeToString(h[:]) }

func (a *Account) CheckToken(t string, now time.Time) bool {
	h := tokenHash(t)
	ok := false
	keep := a.Tokens[:0]
	for _, tk := range a.Tokens {
		if now.After(tk.Expires) {
			continue
		}
		if subtle.ConstantTimeCompare([]byte(tk.Hash), []byte(h)) == 1 {
			ok = true
		}
		keep = append(keep, tk)
	}
	a.Tokens = keep
	return ok
}

func (a *Account) AddToken(hashed string, life time.Duration) {
	a.Tokens = append(a.Tokens, Token{Hash: hashed, Expires: time.Now().Add(life)})
	if len(a.Tokens) > 5 { // 5 remembered PCs per account
		a.Tokens = a.Tokens[len(a.Tokens)-5:]
	}
}

func (a *Account) RemoveToken(t string) {
	h := tokenHash(t)
	keep := a.Tokens[:0]
	for _, tk := range a.Tokens {
		if tk.Hash != h {
			keep = append(keep, tk)
		}
	}
	a.Tokens = keep
}
