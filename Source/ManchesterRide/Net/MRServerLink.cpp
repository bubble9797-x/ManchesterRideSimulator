#include "Net/MRServerLink.h"

#if defined(MR_LINK_STANDALONE)
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <windows.h>
	#include <bcrypt.h>
#else
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <bcrypt.h>
#endif

#include <algorithm>

#ifndef NT_SUCCESS
#define NT_SUCCESS(S) (((NTSTATUS)(S)) >= 0)
#endif
// Unreal builds against an older Windows SDK level that hides this name; Windows 10 and later support it.
#ifndef BCRYPT_KDF_RAW_SECRET
#define BCRYPT_KDF_RAW_SECRET L"TRUNCATE"
#endif

namespace MRNet
{
	namespace
	{
		constexpr int MaxPacket = 1200;
		constexpr int HelloSize = 256;
		constexpr uint8_t PktHello = 0x01, PktHelloAck = 0x02, PktReject = 0x03, PktData = 0x10;

		void Random(void* P, size_t N) { BCryptGenRandom(nullptr, (PUCHAR)P, (ULONG)N, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }

		void Put32(uint8_t* P, uint32_t V) { P[0] = uint8_t(V); P[1] = uint8_t(V >> 8); P[2] = uint8_t(V >> 16); P[3] = uint8_t(V >> 24); }
		void Put64(uint8_t* P, uint64_t V) { Put32(P, uint32_t(V)); Put32(P + 4, uint32_t(V >> 32)); }
		uint32_t Get32(const uint8_t* P) { return uint32_t(P[0]) | (uint32_t(P[1]) << 8) | (uint32_t(P[2]) << 16) | (uint32_t(P[3]) << 24); }
		uint64_t Get64(const uint8_t* P) { return uint64_t(Get32(P)) | (uint64_t(Get32(P + 4)) << 32); }

		void Hmac(const uint8_t* Key, size_t KeyLen, const uint8_t* A, size_t ALen, const uint8_t* B, size_t BLen, uint8_t Out[32])
		{
			BCRYPT_ALG_HANDLE Alg = nullptr;
			BCRYPT_HASH_HANDLE H = nullptr;
			BCryptOpenAlgorithmProvider(&Alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
			BCryptCreateHash(Alg, &H, nullptr, 0, (PUCHAR)Key, (ULONG)KeyLen, 0);
			if (ALen) BCryptHashData(H, (PUCHAR)A, (ULONG)ALen, 0);
			if (BLen) BCryptHashData(H, (PUCHAR)B, (ULONG)BLen, 0);
			BCryptFinishHash(H, Out, 32, 0);
			BCryptDestroyHash(H);
			BCryptCloseAlgorithmProvider(Alg, 0);
		}

		// HKDF-SHA256 (RFC 5869), 64 bytes of output.
		void Hkdf64(const std::vector<uint8_t>& Ikm, const std::vector<uint8_t>& Salt, const char* Info, uint8_t Out[64])
		{
			uint8_t Prk[32];
			Hmac(Salt.data(), Salt.size(), Ikm.data(), Ikm.size(), nullptr, 0, Prk);
			const size_t InfoLen = strlen(Info);
			std::vector<uint8_t> T1In(Info, Info + InfoLen);
			T1In.push_back(1);
			Hmac(Prk, 32, T1In.data(), T1In.size(), nullptr, 0, Out);
			std::vector<uint8_t> T2In(Out, Out + 32);
			T2In.insert(T2In.end(), Info, Info + InfoLen);
			T2In.push_back(2);
			Hmac(Prk, 32, T2In.data(), T2In.size(), nullptr, 0, Out + 32);
		}

		BCRYPT_KEY_HANDLE MakeAesKey(BCRYPT_ALG_HANDLE Alg, const uint8_t* Key)
		{
			BCRYPT_KEY_HANDLE K = nullptr;
			BCryptGenerateSymmetricKey(Alg, &K, nullptr, 0, (PUCHAR)Key, 32, 0);
			return K;
		}

		// 65-byte uncompressed point (04 X Y) <-> CNG public blob.
		bool ImportPub(BCRYPT_ALG_HANDLE Alg, const uint8_t* P65, BCRYPT_KEY_HANDLE& Out)
		{
			if (P65[0] != 4) return false;
			uint8_t Blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
			BCRYPT_ECCKEY_BLOB* Hdr = (BCRYPT_ECCKEY_BLOB*)Blob;
			Hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
			Hdr->cbKey = 32;
			memcpy(Blob + sizeof(BCRYPT_ECCKEY_BLOB), P65 + 1, 64);
			return NT_SUCCESS(BCryptImportKeyPair(Alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &Out, Blob, sizeof(Blob), 0));
		}

		bool Agree(BCRYPT_KEY_HANDLE Mine, BCRYPT_KEY_HANDLE Theirs, uint8_t Out[32])
		{
			BCRYPT_SECRET_HANDLE S = nullptr;
			if (!NT_SUCCESS(BCryptSecretAgreement(Mine, Theirs, &S, 0))) return false;
			ULONG Got = 0;
			const bool bOk = NT_SUCCESS(BCryptDeriveKey(S, BCRYPT_KDF_RAW_SECRET, nullptr, Out, 32, &Got, 0)) && Got == 32;
			BCryptDestroySecret(S);
			std::reverse(Out, Out + 32); // CNG returns the raw secret little-endian; everyone else uses big-endian X
			return bOk;
		}
	}

	// ------------------------------------------------------------------
	// SHA-256
	// ------------------------------------------------------------------

	FSha256::FSha256()
	{
		BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE*)&Alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		BCryptCreateHash((BCRYPT_ALG_HANDLE)Alg, (BCRYPT_HASH_HANDLE*)&H, nullptr, 0, nullptr, 0, 0);
	}
	FSha256::~FSha256()
	{
		if (H) BCryptDestroyHash((BCRYPT_HASH_HANDLE)H);
		if (Alg) BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)Alg, 0);
	}
	void FSha256::Update(const void* Data, size_t Len)
	{
		while (Len > 0)
		{
			const ULONG Part = (ULONG)std::min<size_t>(Len, 1u << 30);
			BCryptHashData((BCRYPT_HASH_HANDLE)H, (PUCHAR)Data, Part, 0);
			Data = (const uint8_t*)Data + Part;
			Len -= Part;
		}
	}
	void FSha256::End(uint8_t Out[32]) { BCryptFinishHash((BCRYPT_HASH_HANDLE)H, Out, 32, 0); }
	void FSha256::Hash(const void* Data, size_t Len, uint8_t Out[32]) { FSha256 S; S.Update(Data, Len); S.End(Out); }

	// ------------------------------------------------------------------
	// Link
	// ------------------------------------------------------------------

	FServerLink::FServerLink()
	{
		WSADATA W;
		WSAStartup(MAKEWORD(2, 2), &W);
		BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE*)&EcdhAlg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
		BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE*)&AesAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
		BCryptSetProperty((BCRYPT_ALG_HANDLE)AesAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
	}

	FServerLink::~FServerLink()
	{
		Close(false);
		if (AesAlg) BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)AesAlg, 0);
		if (EcdhAlg) BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)EcdhAlg, 0);
		WSACleanup();
	}

	void FServerLink::Close(bool bSayBye)
	{
		if (bSayBye && State == EState::Connected)
		{
			const std::vector<uint8_t> Bye{ CBye };
			for (int i = 0; i < 3; ++i) SendNow(Bye); // unreliable: say it a few times
		}
		if (Sock != ~uintptr_t(0)) { closesocket((SOCKET)Sock); Sock = ~uintptr_t(0); }
		if (SendKey) { BCryptDestroyKey((BCRYPT_KEY_HANDLE)SendKey); SendKey = nullptr; }
		if (RecvKey) { BCryptDestroyKey((BCRYPT_KEY_HANDLE)RecvKey); RecvKey = nullptr; }
		if (EcdhKey) { BCryptDestroyKey((BCRYPT_KEY_HANDLE)EcdhKey); EcdhKey = nullptr; }
		if (State != EState::Failed) State = EState::Idle;
		Unacked.clear(); RecvBuf.clear(); Unrel.clear();
		NextSeq = RecvNext = 1; SendCtr = RecvMax = RecvMask = 0; bNeedAck = false;
	}

	void FServerLink::Fail(const std::string& Why)
	{
		Error = Why;
		Close(false);
		State = EState::Failed;
	}

	bool FServerLink::Start(const std::string& Host, uint16_t Port, double Now)
	{
		Close(false);
		State = EState::Idle;
		Error.clear();
		ServerKey.clear();

		addrinfo Hints = {};
		Hints.ai_family = AF_INET;
		Hints.ai_socktype = SOCK_DGRAM;
		addrinfo* Res = nullptr;
		if (getaddrinfo(Host.c_str(), nullptr, &Hints, &Res) != 0 || !Res)
		{
			Fail("Could not find the server address " + Host + ".");
			return false;
		}
		sockaddr_in A = *(sockaddr_in*)Res->ai_addr;
		freeaddrinfo(Res);
		A.sin_port = htons(Port);
		memcpy(PeerAddr, &A, sizeof(A));

		SOCKET S = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (S == INVALID_SOCKET) { Fail("Could not open a network socket."); return false; }
		u_long NonBlocking = 1;
		ioctlsocket(S, FIONBIO, &NonBlocking);
		int Buf = 4 << 20;
		setsockopt(S, SOL_SOCKET, SO_RCVBUF, (const char*)&Buf, sizeof(Buf));
		setsockopt(S, SOL_SOCKET, SO_SNDBUF, (const char*)&Buf, sizeof(Buf));
		// A UDP "port unreachable" must not break later reads on Windows.
		BOOL NoReset = FALSE;
		DWORD Ret = 0;
		WSAIoctl(S, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &NoReset, sizeof(NoReset), nullptr, 0, &Ret, nullptr, nullptr);
		Sock = (uintptr_t)S;

		// Fresh ephemeral key for this connection.
		if (!NT_SUCCESS(BCryptGenerateKeyPair((BCRYPT_ALG_HANDLE)EcdhAlg, (BCRYPT_KEY_HANDLE*)&EcdhKey, 256, 0))
			|| !NT_SUCCESS(BCryptFinalizeKeyPair((BCRYPT_KEY_HANDLE)EcdhKey, 0)))
		{
			Fail("Could not create an encryption key.");
			return false;
		}
		uint8_t Blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
		ULONG Got = 0;
		BCryptExportKey((BCRYPT_KEY_HANDLE)EcdhKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, Blob, sizeof(Blob), &Got, 0);
		EphPub[0] = 4;
		memcpy(EphPub + 1, Blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
		Random(ClientNonce, sizeof(ClientNonce));

		State = EState::Handshaking;
		StartedAt = LastRecv = Now;
		HelloTries = 0;
		SendHello(Now);
		return true;
	}

	void FServerLink::SendHello(double Now)
	{
		uint8_t P[HelloSize] = {};
		P[0] = PktHello;
		memcpy(P + 1, "MRS1", 4);
		P[5] = uint8_t(ProtoVersion); P[6] = uint8_t(ProtoVersion >> 8);
		memcpy(P + 7, EphPub, 65);
		memcpy(P + 72, ClientNonce, 16);
		SendRaw(P, HelloSize);
		HelloSentAt = Now;
		++HelloTries;
	}

	void FServerLink::SendRaw(const uint8_t* D, int N)
	{
		if (Sock == ~uintptr_t(0)) return;
		sendto((SOCKET)Sock, (const char*)D, N, 0, (const sockaddr*)PeerAddr, sizeof(sockaddr_in));
	}

	std::string FServerLink::GetServerFingerprint() const
	{
		if (ServerKey.size() != 65) return std::string();
		uint8_t H[32];
		FSha256::Hash(ServerKey.data(), ServerKey.size(), H);
		static const char* Hex = "0123456789abcdef";
		std::string S;
		for (int i = 0; i < 8; ++i)
		{
			if (i && i % 2 == 0) S += '-';
			S += Hex[H[i] >> 4]; S += Hex[H[i] & 15];
		}
		return S;
	}

	void FServerLink::HandleHelloAck(const uint8_t* D, int N)
	{
		if (State != EState::Handshaking || N < 1 + 4 + 2 + 65 + 65 + 16 + 4 || memcmp(D + 1, "MRS1", 4) != 0) return;
		const uint8_t* StaticPub = D + 7;
		const uint8_t* EphSrv = D + 72;
		const uint8_t* SrvNonce = D + 137;
		const uint32_t Sid = Get32(D + 153);

		BCRYPT_KEY_HANDLE KStatic = nullptr, KEph = nullptr;
		uint8_t Ss1[32], Ss2[32];
		const bool bOk = ImportPub((BCRYPT_ALG_HANDLE)EcdhAlg, StaticPub, KStatic) && ImportPub((BCRYPT_ALG_HANDLE)EcdhAlg, EphSrv, KEph)
			&& Agree((BCRYPT_KEY_HANDLE)EcdhKey, KEph, Ss1) && Agree((BCRYPT_KEY_HANDLE)EcdhKey, KStatic, Ss2);
		if (KStatic) BCryptDestroyKey(KStatic);
		if (KEph) BCryptDestroyKey(KEph);
		if (!bOk) { Fail("The server sent a bad encryption key."); return; }

		std::vector<uint8_t> Ikm(Ss1, Ss1 + 32);
		Ikm.insert(Ikm.end(), Ss2, Ss2 + 32);
		std::vector<uint8_t> Salt(ClientNonce, ClientNonce + 16);
		Salt.insert(Salt.end(), SrvNonce, SrvNonce + 16);
		uint8_t Keys[64];
		Hkdf64(Ikm, Salt, "MR-session-v1", Keys);
		SendKey = MakeAesKey((BCRYPT_ALG_HANDLE)AesAlg, Keys);
		RecvKey = MakeAesKey((BCRYPT_ALG_HANDLE)AesAlg, Keys + 32);
		SecureZeroMemory(Keys, sizeof(Keys));
		ServerKey.assign(StaticPub, StaticPub + 65);
		SessionId = Sid;
		State = EState::Connected;
		bNeedAck = true; // first packet tells the server we're here
	}

	bool FServerLink::Open(const uint8_t* D, int N, std::vector<uint8_t>& OutPlain)
	{
		if (N < 13 + 16 || Get32(D + 1) != SessionId) return false;
		const uint64_t Ctr = Get64(D + 5);
		if (Ctr == 0) return false;
		if (Ctr <= RecvMax)
		{
			const uint64_t Dist = RecvMax - Ctr;
			if (Dist >= 64 || (RecvMask & (1ull << Dist))) return false;
		}
		uint8_t Nonce[12];
		Put32(Nonce, 1);
		Put64(Nonce + 4, Ctr);
		const int CipherLen = N - 13 - 16;
		BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO Info;
		BCRYPT_INIT_AUTH_MODE_INFO(Info);
		Info.pbNonce = Nonce; Info.cbNonce = 12;
		Info.pbAuthData = (PUCHAR)D; Info.cbAuthData = 13;
		Info.pbTag = (PUCHAR)(D + N - 16); Info.cbTag = 16;
		OutPlain.resize(CipherLen > 0 ? CipherLen : 1);
		ULONG Got = 0;
		if (!NT_SUCCESS(BCryptDecrypt((BCRYPT_KEY_HANDLE)RecvKey, (PUCHAR)(D + 13), CipherLen, &Info, nullptr, 0, OutPlain.data(), CipherLen, &Got, 0))) return false;
		OutPlain.resize(Got);
		if (Ctr > RecvMax)
		{
			const uint64_t Shift = Ctr - RecvMax;
			RecvMask = Shift >= 64 ? 1 : ((RecvMask << Shift) | 1);
			RecvMax = Ctr;
		}
		else RecvMask |= 1ull << (RecvMax - Ctr);
		return true;
	}

	void FServerLink::SealAndSend(const std::vector<uint8_t>& Plain)
	{
		if (State != EState::Connected || !SendKey) return;
		++SendCtr;
		std::vector<uint8_t> Out(13 + Plain.size() + 16);
		Out[0] = PktData;
		Put32(&Out[1], SessionId);
		Put64(&Out[5], SendCtr);
		uint8_t Nonce[12];
		Put32(Nonce, 0);
		Put64(Nonce + 4, SendCtr);
		BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO Info;
		BCRYPT_INIT_AUTH_MODE_INFO(Info);
		Info.pbNonce = Nonce; Info.cbNonce = 12;
		Info.pbAuthData = Out.data(); Info.cbAuthData = 13;
		Info.pbTag = Out.data() + 13 + Plain.size(); Info.cbTag = 16;
		ULONG Got = 0;
		BCryptEncrypt((BCRYPT_KEY_HANDLE)SendKey, (PUCHAR)Plain.data(), (ULONG)Plain.size(), &Info, nullptr, 0, Out.data() + 13, (ULONG)Plain.size(), &Got, 0);
		SendRaw(Out.data(), int(Out.size()));
	}

	void FServerLink::Poll(double Now, std::vector<std::vector<uint8_t>>& Out)
	{
		if (Sock == ~uintptr_t(0)) return;
		uint8_t Buf[2048];
		for (int Guard = 0; Guard < 20000; ++Guard)
		{
			sockaddr_in From;
			int FromLen = sizeof(From);
			const int N = recvfrom((SOCKET)Sock, (char*)Buf, sizeof(Buf), 0, (sockaddr*)&From, &FromLen);
			if (N <= 0) break;
			const sockaddr_in* Peer = (const sockaddr_in*)PeerAddr;
			if (From.sin_addr.s_addr != Peer->sin_addr.s_addr || From.sin_port != Peer->sin_port) continue; // not our server
			BytesIn += N;
			switch (Buf[0])
			{
			case PktHelloAck: LastRecv = Now; HandleHelloAck(Buf, N); break;
			case PktReject:
				if (State == EState::Handshaking)
				{
					FReader R(Buf, N, 2);
					const std::string Msg = R.Str();
					Fail(Msg.empty() ? "The server refused the connection." : Msg);
					return;
				}
				break;
			case PktData: HandleData(Buf, N, Now, Out); break;
			default: break;
			}
			if (State == EState::Failed || State == EState::Idle) return;
		}

		if (State == EState::Handshaking && Now - HelloSentAt > 0.6)
		{
			if (HelloTries >= 12) Fail("The server isn't answering. It may be offline, or the Playit tunnel may be down.");
			else SendHello(Now);
		}
		else if (State == EState::Connected && Now - LastRecv > 15.0)
		{
			Fail("Lost connection to the server (no reply for 15 seconds).");
		}
	}

	void FServerLink::HandleData(const uint8_t* D, int N, double Now, std::vector<std::vector<uint8_t>>& Out)
	{
		if (State != EState::Connected) return;
		std::vector<uint8_t> P;
		if (!Open(D, N, P) || P.size() < 4) return;
		LastRecv = Now;
		const uint32_t Ack = Get32(P.data());
		while (!Unacked.empty() && int32_t(Unacked.front().Seq - Ack) < 0)
		{
			if (Unacked.front().Sends == 1) Srtt += ((Now - Unacked.front().LastSent) * 1000.0 - Srtt) * 0.125;
			Unacked.pop_front();
		}
		FReader R(P.data(), P.size(), 4);
		while (R.Left() > 0)
		{
			const uint8_t Kind = R.U8();
			uint32_t Seq = 0;
			if (Kind == 1) Seq = R.U32();
			const uint16_t Len = R.U16();
			const uint8_t* Data = R.Raw(Len);
			if (!Data || Len == 0) return;
			if (Kind == 0) { Out.emplace_back(Data, Data + Len); continue; }
			bNeedAck = true;
			const int32_t Dist = int32_t(Seq - RecvNext);
			if (Dist < 0 || Dist > 4096) continue;
			RecvBuf[Seq].assign(Data, Data + Len);
			for (auto It = RecvBuf.find(RecvNext); It != RecvBuf.end(); It = RecvBuf.find(RecvNext))
			{
				Out.push_back(std::move(It->second));
				RecvBuf.erase(It);
				++RecvNext;
			}
		}
	}

	void FServerLink::SendReliable(std::vector<uint8_t> Msg)
	{
		FRel R;
		R.Seq = NextSeq++;
		R.Data = std::move(Msg);
		Unacked.push_back(std::move(R));
	}

	void FServerLink::SendUnreliable(std::vector<uint8_t> Msg) { Unrel.push_back(std::move(Msg)); }

	void FServerLink::SendNow(const std::vector<uint8_t>& Msg)
	{
		std::vector<uint8_t> P(4);
		Put32(P.data(), RecvNext);
		P.push_back(0);
		P.push_back(uint8_t(Msg.size()));
		P.push_back(uint8_t(Msg.size() >> 8));
		P.insert(P.end(), Msg.begin(), Msg.end());
		SealAndSend(P);
	}

	void FServerLink::Flush(double Now)
	{
		if (State != EState::Connected) return;
		const double Rto = std::clamp(Srtt * 1.6 + 60.0, 120.0, 2000.0) / 1000.0;
		std::vector<uint8_t> Cur(4);
		Put32(Cur.data(), RecvNext);
		int Items = 0;
		auto Emit = [&]() { SealAndSend(Cur); Cur.assign(4, 0); Put32(Cur.data(), RecvNext); Items = 0; };
		auto Add = [&](const std::vector<uint8_t>& Item)
		{
			if (Cur.size() + Item.size() > size_t(MaxPacket) && Items > 0) Emit();
			Cur.insert(Cur.end(), Item.begin(), Item.end());
			++Items;
		};
		bool bSent = false;
		int Budget = 64;
		for (FRel& M : Unacked)
		{
			if (Budget == 0) break;
			if (M.Sends > 0 && Now - M.LastSent < Rto) continue;
			std::vector<uint8_t> Item{ 1 };
			Item.resize(7);
			Put32(&Item[1], M.Seq);
			Item[5] = uint8_t(M.Data.size()); Item[6] = uint8_t(M.Data.size() >> 8);
			Item.insert(Item.end(), M.Data.begin(), M.Data.end());
			Add(Item);
			M.LastSent = Now;
			++M.Sends;
			--Budget;
		}
		for (const std::vector<uint8_t>& U : Unrel)
		{
			std::vector<uint8_t> Item{ 0, uint8_t(U.size()), uint8_t(U.size() >> 8) };
			Item.insert(Item.end(), U.begin(), U.end());
			Add(Item);
		}
		Unrel.clear();
		if (Items > 0 || bNeedAck || Now - LastSend > 1.0)
		{
			Emit();
			bSent = true;
		}
		if (bSent) { bNeedAck = false; LastSend = Now; }
	}
}

#if !defined(MR_LINK_STANDALONE)
	#include "Windows/HideWindowsPlatformTypes.h"
#endif
