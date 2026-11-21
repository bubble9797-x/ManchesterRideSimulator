#pragma once

// Encrypted UDP link to the Manchester Ride dedicated server (Server/ in the project, written in Go).
// Plain C++ on Windows APIs (Winsock + CNG) so it can also be compiled outside Unreal for protocol tests
// (Tools/server/linktest.cpp). Wire format: see Server/protocol.go - both sides must match.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <map>

namespace MRNet
{
	enum : uint8_t
	{
		// client -> server
		CLogin = 0x01, CState = 0x02, CLook = 0x03, CChat = 0x04, CVoice = 0x05, CPing = 0x06, CFileReq = 0x07, CBye = 0x08, CLogout = 0x09,
		// server -> client
		SLoginResult = 0x81, SPlayer = 0x82, SPlayerLeave = 0x83, SStates = 0x84, SChat = 0x85, SKick = 0x86, SVoice = 0x87,
		SPong = 0x88, SFileChunk = 0x89, STeleport = 0x8A, SEnv = 0x8B, SUpdate = 0x8C, SPings = 0x8D,
	};
	enum : uint8_t
	{
		LoginOK = 0, LoginBadPassword = 1, LoginNeedUpdate = 2, LoginBanned = 3, LoginFull = 4, LoginNotWhitelist = 5, LoginBadName = 6,
		LoginSlowDown = 7, LoginTaken = 8, LoginNoAccount = 9, LoginRegClosed = 10, LoginBadToken = 11,
	};
	enum : uint8_t { ChatPlayer = 0, ChatSystem = 1, ChatPrivate = 2, ChatServer = 3, ChatError = 4, ChatAction = 5 };
	constexpr uint16_t ProtoVersion = 1;
	constexpr int StateSize = 35;

	struct FWriter
	{
		std::vector<uint8_t> B;
		void U8(uint8_t V) { B.push_back(V); }
		void U16(uint16_t V) { U8(uint8_t(V)); U8(uint8_t(V >> 8)); }
		void U32(uint32_t V) { U16(uint16_t(V)); U16(uint16_t(V >> 16)); }
		void U64(uint64_t V) { U32(uint32_t(V)); U32(uint32_t(V >> 32)); }
		void F32(float V) { uint32_t U; memcpy(&U, &V, 4); U32(U); }
		void F64(double V) { uint64_t U; memcpy(&U, &V, 8); U64(U); }
		void Bytes(const void* P, size_t N) { B.insert(B.end(), (const uint8_t*)P, (const uint8_t*)P + N); }
		void Str(const std::string& S) { const size_t N = S.size() > 4000 ? 4000 : S.size(); U16(uint16_t(N)); Bytes(S.data(), N); }
	};

	struct FReader
	{
		const uint8_t* P = nullptr; size_t N = 0, Pos = 0; bool bBad = false;
		FReader(const uint8_t* InP, size_t InN, size_t Start = 0) : P(InP), N(InN), Pos(Start) {}
		bool Need(size_t K) { if (bBad || Pos + K > N) { bBad = true; return false; } return true; }
		uint8_t U8() { return Need(1) ? P[Pos++] : 0; }
		uint16_t U16() { if (!Need(2)) return 0; uint16_t V = uint16_t(P[Pos] | (P[Pos + 1] << 8)); Pos += 2; return V; }
		uint32_t U32() { uint32_t Lo = U16(); uint32_t Hi = U16(); return Lo | (Hi << 16); }
		uint64_t U64() { uint64_t Lo = U32(); uint64_t Hi = U32(); return Lo | (Hi << 32); }
		float F32() { uint32_t U = U32(); float F; memcpy(&F, &U, 4); return F; }
		double F64() { uint64_t U = U64(); double D; memcpy(&D, &U, 8); return D; }
		const uint8_t* Raw(size_t K) { if (!Need(K)) return nullptr; const uint8_t* R = P + Pos; Pos += K; return R; }
		std::string Str() { const uint16_t L = U16(); const uint8_t* R = Raw(L); return R ? std::string((const char*)R, L) : std::string(); }
		size_t Left() const { return bBad ? 0 : N - Pos; }
	};

	/** SHA-256 (CNG). Streaming use: Begin, Update..., End. */
	class FSha256
	{
	public:
		FSha256();
		~FSha256();
		void Update(const void* Data, size_t Len);
		void End(uint8_t Out[32]);
		static void Hash(const void* Data, size_t Len, uint8_t Out[32]);
	private:
		void* Alg = nullptr; void* H = nullptr;
	};

	class FServerLink
	{
	public:
		enum class EState { Idle, Handshaking, Connected, Failed };

		FServerLink();
		~FServerLink();

		/** Opens the socket and starts the handshake. Host must be an IPv4 address or a name (resolved here, blocking). */
		bool Start(const std::string& Host, uint16_t Port, double Now);
		void Close(bool bSayBye);

		/** Receives, decrypts and orders everything waiting on the socket; returns complete messages. Call every frame. */
		void Poll(double Now, std::vector<std::vector<uint8_t>>& OutMessages);
		/** Sends queued messages, resends lost reliable ones, keeps the link alive. Call every frame after Poll. */
		void Flush(double Now);

		void SendReliable(std::vector<uint8_t> Msg);
		void SendUnreliable(std::vector<uint8_t> Msg);
		/** Sent at once in its own packet (voice, download requests). */
		void SendNow(const std::vector<uint8_t>& Msg);

		EState GetState() const { return State; }
		const std::string& GetError() const { return Error; }
		/** The server's long-term key (65 bytes, uncompressed P-256), known once connected. */
		const std::vector<uint8_t>& GetServerKey() const { return ServerKey; }
		std::string GetServerFingerprint() const;
		float GetRttMs() const { return float(Srtt); }
		uint64_t GetBytesIn() const { return BytesIn; }

	private:
		void Fail(const std::string& Why);
		void SendHello(double Now);
		void HandleHelloAck(const uint8_t* D, int N);
		void HandleData(const uint8_t* D, int N, double Now, std::vector<std::vector<uint8_t>>& Out);
		void SealAndSend(const std::vector<uint8_t>& Plain);
		void SendRaw(const uint8_t* D, int N);
		bool Open(const uint8_t* D, int N, std::vector<uint8_t>& OutPlain);

		EState State = EState::Idle;
		std::string Error;
		uintptr_t Sock = ~uintptr_t(0);
		uint8_t PeerAddr[16] = {}; // sockaddr_in
		double HelloSentAt = 0.0, StartedAt = 0.0, LastRecv = 0.0, LastSend = 0.0;
		int HelloTries = 0;

		// crypto
		void* EcdhAlg = nullptr; void* EcdhKey = nullptr; void* AesAlg = nullptr; void* SendKey = nullptr; void* RecvKey = nullptr;
		uint8_t ClientNonce[16] = {};
		uint8_t EphPub[65] = {};
		std::vector<uint8_t> ServerKey;
		uint32_t SessionId = 0;
		uint64_t SendCtr = 0, RecvMax = 0, RecvMask = 0;

		// reliable channel (same rules as Server/reliable.go)
		struct FRel { uint32_t Seq; std::vector<uint8_t> Data; double LastSent = 0.0; int Sends = 0; };
		std::deque<FRel> Unacked;
		uint32_t NextSeq = 1, RecvNext = 1;
		std::map<uint32_t, std::vector<uint8_t>> RecvBuf;
		std::vector<std::vector<uint8_t>> Unrel;
		bool bNeedAck = false;
		double Srtt = 150.0;
		uint64_t BytesIn = 0;
	};
}
