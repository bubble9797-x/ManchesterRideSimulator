package main

// Wire protocol shared with the game (Source/ManchesterRide/Net/MRServerProtocol.h). Keep both in step.
//
// Outer UDP packets:
//   HELLO     C->S  0x01 "MRS1" u16 proto  clientEphemeralPub[65] clientNonce[16]  (zero-padded to 256 bytes,
//                   so the reply is never larger than the request - no amplification)
//   HELLO_ACK S->C  0x02 "MRS1" u16 proto  serverStaticPub[65] serverEphemeralPub[65] serverNonce[16] u32 session
//   REJECT    S->C  0x03 u8 reason  str message                      (plaintext, before a session exists)
//   DATA      both  0x10 u32 session  u64 counter  AES-256-GCM(payload) (AAD = these 13 header bytes)
//
// Keys: HKDF-SHA256(ikm = ECDH(cEph,sEph) || ECDH(cEph,sStatic), salt = cNonce||sNonce, info "MR-session-v1") -> 64 bytes:
// client->server key, server->client key. Only the holder of the server's static key can read the session, and the
// game remembers that key the first time it connects (like SSH), so nobody can sit in the middle later on.
// GCM nonce = u32 direction (0 c->s, 1 s->c) || u64 counter.
//
// DATA payload: u32 ack (next reliable seq expected from the peer), then items until the end:
//   u8 kind (0 unreliable, 1 reliable)  [u32 seq if reliable]  u16 len  msg[len]   (msg[0] = message type)

const (
	ProtoVersion = 1
	MaxPacket    = 1200 // plaintext bytes per DATA packet

	PktHello    = 0x01
	PktHelloAck = 0x02
	PktReject   = 0x03
	PktData     = 0x10

	HelloSize = 256

	// client -> server
	CLogin    = 0x01 // u8 mode(0 password,1 register,2 token)  str user  str secret  u32 build  str version
	CState    = 0x02 // 35-byte ride state (unreliable)
	CLook     = 0x03 // str vehicle u32 paint u32 jacket u32 helmet str look
	CChat     = 0x04 // str text ('/' = command)
	CVoice    = 0x05 // u16 seq  opus... (unreliable)
	CPing     = 0x06 // u32 clientMs (unreliable)
	CFileReq  = 0x07 // u16 n  u32 chunk[n] (unreliable)
	CBye      = 0x08
	CLogout   = 0x09 // forget the remembered-login token

	// server -> client
	SLoginResult = 0x81
	SPlayer      = 0x82 // u16 id str name u8 op str vehicle u32 paint u32 jacket u32 helmet str look
	SPlayerLeave = 0x83 // u16 id
	SStates      = 0x84 // u8 n  n x (u16 id, state[35])  (unreliable)
	SChat        = 0x85 // u8 kind  str from  str text
	SKick        = 0x86 // str reason
	SVoice       = 0x87 // u16 from u16 seq opus... (unreliable)
	SPong        = 0x88 // u32 clientMs (unreliable)
	SFileChunk   = 0x89 // u32 chunk  data (unreliable)
	STeleport    = 0x8A // u8 mode(0 unreal cm, 1 lat/lon)  f64 a f64 b f64 c  f32 yaw
	SEnv         = 0x8B // i8 weather (-1 keep)  f32 hours (-1 keep)
	SUpdate      = 0x8C // str version u32 build  (a newer game is on the server)
	SPings       = 0x8D // u8 n  n x (u16 id, u16 ms)

	StateSize = 35
	ChunkSize = 1024

	// login result codes
	LoginOK           = 0
	LoginBadPassword  = 1
	LoginNeedUpdate   = 2
	LoginBanned       = 3
	LoginFull         = 4
	LoginNotWhitelist = 5
	LoginBadName      = 6
	LoginSlowDown     = 7
	LoginTaken        = 8
	LoginNoAccount    = 9
	LoginRegClosed    = 10
	LoginBadToken     = 11

	// chat kinds
	ChatPlayer  = 0
	ChatSystem  = 1 // joins, leaves, server notices (yellow)
	ChatPrivate = 2
	ChatServer  = 3 // [Server] say from the console
	ChatError   = 4 // command errors (red), only to you
	ChatAction  = 5 // /me
)
