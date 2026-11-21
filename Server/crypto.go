package main

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/hkdf"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"os"
	"strings"
)

// The server's long-term identity key (P-256). Created on first start; keep server-identity.key private and
// keep it when you move or reinstall the server, or players see a "server key changed" warning.
func loadOrCreateIdentity(path string) (*ecdh.PrivateKey, bool, error) {
	if b, err := os.ReadFile(path); err == nil {
		raw, err := hex.DecodeString(strings.TrimSpace(string(b)))
		if err != nil {
			return nil, false, errors.New("server-identity.key is damaged: " + err.Error())
		}
		k, err := ecdh.P256().NewPrivateKey(raw)
		return k, false, err
	}
	k, err := ecdh.P256().GenerateKey(rand.Reader)
	if err != nil {
		return nil, false, err
	}
	if err := os.WriteFile(path, []byte(hex.EncodeToString(k.Bytes())+"\n"), 0600); err != nil {
		return nil, false, err
	}
	return k, true, nil
}

func fingerprint(pub []byte) string {
	h := sha256.Sum256(pub)
	s := hex.EncodeToString(h[:8])
	return s[0:4] + "-" + s[4:8] + "-" + s[8:12] + "-" + s[12:16]
}

func deriveKeys(ss1, ss2, cNonce, sNonce []byte) (c2s, s2c []byte, err error) {
	ikm := append(append([]byte{}, ss1...), ss2...)
	salt := append(append([]byte{}, cNonce...), sNonce...)
	out, err := hkdf.Key(sha256.New, ikm, salt, "MR-session-v1", 64)
	if err != nil {
		return nil, nil, err
	}
	return out[:32], out[32:], nil
}

func newGCM(key []byte) cipher.AEAD {
	b, err := aes.NewCipher(key)
	if err != nil {
		panic(err)
	}
	g, err := cipher.NewGCM(b)
	if err != nil {
		panic(err)
	}
	return g
}

type SecureLink struct {
	id       uint32
	send     cipher.AEAD // s2c
	recv     cipher.AEAD // c2s
	sendCtr  uint64
	recvMax  uint64
	recvMask uint64
}

func (l *SecureLink) Seal(plain []byte) []byte {
	l.sendCtr++
	out := make([]byte, 13, 13+len(plain)+16)
	out[0] = PktData
	binary.LittleEndian.PutUint32(out[1:], l.id)
	binary.LittleEndian.PutUint64(out[5:], l.sendCtr)
	var nonce [12]byte
	binary.LittleEndian.PutUint32(nonce[0:], 1)
	binary.LittleEndian.PutUint64(nonce[4:], l.sendCtr)
	return l.send.Seal(out, nonce[:], plain, out[:13])
}

// Open decrypts a DATA packet, rejecting replays (64-packet window).
func (l *SecureLink) Open(pkt []byte) ([]byte, bool) {
	if len(pkt) < 13+16 {
		return nil, false
	}
	ctr := binary.LittleEndian.Uint64(pkt[5:])
	if ctr == 0 {
		return nil, false
	}
	if ctr <= l.recvMax {
		d := l.recvMax - ctr
		if d >= 64 || l.recvMask&(1<<d) != 0 {
			return nil, false
		}
	}
	var nonce [12]byte
	binary.LittleEndian.PutUint32(nonce[0:], 0)
	binary.LittleEndian.PutUint64(nonce[4:], ctr)
	plain, err := l.recv.Open(nil, nonce[:], pkt[13:], pkt[:13])
	if err != nil {
		return nil, false
	}
	if ctr > l.recvMax {
		shift := ctr - l.recvMax
		if shift >= 64 {
			l.recvMask = 1
		} else {
			l.recvMask = l.recvMask<<shift | 1
		}
		l.recvMax = ctr
	} else {
		l.recvMask |= 1 << (l.recvMax - ctr)
	}
	return plain, true
}
