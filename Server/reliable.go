package main

import (
	"encoding/binary"
	"time"
)

// A small reliable, ordered message layer on top of the encrypted UDP link (same rules as the game's client).

type relMsg struct {
	seq      uint32
	data     []byte
	lastSent time.Time
	sends    int
}

type Channel struct {
	nextSeq  uint32 // next reliable seq to assign
	unacked  []*relMsg
	recvNext uint32 // next reliable seq expected from the peer
	recvBuf  map[uint32][]byte
	needAck  bool
	unrel    [][]byte
	srtt     float64 // ms
	lastSend time.Time
}

func newChannel() *Channel {
	return &Channel{nextSeq: 1, recvNext: 1, recvBuf: map[uint32][]byte{}, srtt: 150}
}

func (c *Channel) Reliable(msg []byte)   { c.unacked = append(c.unacked, &relMsg{seq: c.nextSeq, data: msg}); c.nextSeq++ }
func (c *Channel) Unreliable(msg []byte) { c.unrel = append(c.unrel, msg) }

func (c *Channel) rto() time.Duration {
	ms := c.srtt*1.6 + 60
	if ms < 120 {
		ms = 120
	}
	if ms > 2000 {
		ms = 2000
	}
	return time.Duration(ms) * time.Millisecond
}

// Receive handles one decrypted payload and returns the messages to act on, in order.
func (c *Channel) Receive(p []byte, now time.Time) ([][]byte, bool) {
	if len(p) < 4 {
		return nil, false
	}
	ack := binary.LittleEndian.Uint32(p)
	n := 0
	for _, m := range c.unacked {
		if int32(m.seq-ack) < 0 { // acked
			if m.sends == 1 {
				rtt := float64(now.Sub(m.lastSent).Milliseconds())
				c.srtt += (rtt - c.srtt) * 0.125
			}
			continue
		}
		c.unacked[n] = m
		n++
	}
	for i := n; i < len(c.unacked); i++ {
		c.unacked[i] = nil
	}
	c.unacked = c.unacked[:n]

	var out [][]byte
	r := &R{b: p, p: 4}
	for r.p < len(r.b) {
		kind := r.U8()
		var seq uint32
		if kind == 1 {
			seq = r.U32()
		}
		l := int(r.U16())
		data := r.Raw(l)
		if r.Bad || l == 0 {
			return out, false
		}
		if kind == 0 {
			out = append(out, data)
			continue
		}
		c.needAck = true
		d := int32(seq - c.recvNext)
		if d < 0 {
			continue // duplicate
		}
		if d > 4096 {
			return out, false
		}
		c.recvBuf[seq] = append([]byte{}, data...)
		for {
			m, ok := c.recvBuf[c.recvNext]
			if !ok {
				break
			}
			delete(c.recvBuf, c.recvNext)
			out = append(out, m)
			c.recvNext++
		}
	}
	return out, true
}

// Flush packs due reliable messages and queued unreliable ones into packets (plaintext, before Seal).
func (c *Channel) Flush(now time.Time, keepalive bool) [][]byte {
	var pkts [][]byte
	cur := make([]byte, 4, MaxPacket)
	binary.LittleEndian.PutUint32(cur, c.recvNext)
	items := 0
	add := func(item []byte) {
		if len(cur)+len(item) > MaxPacket && items > 0 {
			pkts = append(pkts, cur)
			cur = make([]byte, 4, MaxPacket)
			binary.LittleEndian.PutUint32(cur, c.recvNext)
			items = 0
		}
		cur = append(cur, item...)
		items++
	}
	rto := c.rto()
	budget := 64 // reliable messages per flush, so a backlog cannot flood the link
	for _, m := range c.unacked {
		if budget == 0 {
			break
		}
		if m.sends > 0 && now.Sub(m.lastSent) < rto {
			continue
		}
		item := make([]byte, 0, 7+len(m.data))
		item = append(item, 1)
		item = binary.LittleEndian.AppendUint32(item, m.seq)
		item = binary.LittleEndian.AppendUint16(item, uint16(len(m.data)))
		item = append(item, m.data...)
		add(item)
		m.lastSent = now
		m.sends++
		budget--
	}
	for _, u := range c.unrel {
		item := make([]byte, 0, 3+len(u))
		item = append(item, 0)
		item = binary.LittleEndian.AppendUint16(item, uint16(len(u)))
		item = append(item, u...)
		add(item)
	}
	c.unrel = c.unrel[:0]
	if items > 0 || c.needAck || (keepalive && now.Sub(c.lastSend) > time.Second) {
		pkts = append(pkts, cur)
	}
	if len(pkts) > 0 {
		c.needAck = false
		c.lastSend = now
	}
	return pkts
}

// One unreliable message right now (voice, file chunks), outside the tick.
func (c *Channel) Immediate(msg []byte) []byte {
	p := make([]byte, 4, 7+len(msg))
	binary.LittleEndian.PutUint32(p, c.recvNext)
	p = append(p, 0)
	p = binary.LittleEndian.AppendUint16(p, uint16(len(msg)))
	return append(p, msg...)
}
