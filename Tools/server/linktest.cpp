// Protocol test bot for the dedicated server: the game's own link code (Net/MRServerLink.cpp) compiled outside Unreal.
//   linktest <host> <port> <name> <password> register|login|token <seconds> [chat text]
// Logs in, sends ride states at 30 Hz and a little fake voice, prints what it receives, and if the server
// offers a game update it downloads it and checks the SHA-256 (the same way the game does).
#define MR_LINK_STANDALONE
#include "../../Source/ManchesterRide/Net/MRServerLink.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <set>

using namespace MRNet;

static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv)
{
	if (argc < 7) { printf("usage: linktest host port name password register|login|token seconds [chat]\n"); return 2; }
	const std::string Host = argv[1], Name = argv[3], Pw = argv[4], Mode = argv[5];
	const double Secs = atof(argv[6]);
	const std::string Chat = argc > 7 ? argv[7] : "";
	FServerLink L;
	L.Start(Host, uint16_t(atoi(argv[2])), Now());
	bool bSentLogin = false, bIn = false;
	double T0 = Now(), LastState = 0, LastVoice = 0;
	int States = 0, Voices = 0;
	uint16_t VoiceSeq = 0;
	// update download
	bool bDownloading = false; uint64_t Size = 0; uint32_t Chunk = 1024, NumChunks = 0, Have = 0; uint8_t Sha[32];
	std::vector<uint8_t> File; std::vector<uint8_t> Got; std::map<uint32_t, double> Outstanding; uint32_t NextReq = 0; double Window = 64;
	std::string Token;

	while (Now() - T0 < Secs)
	{
		const double T = Now();
		std::vector<std::vector<uint8_t>> Msgs;
		L.Poll(T, Msgs);
		if (L.GetState() == FServerLink::EState::Failed) { printf("FAILED: %s\n", L.GetError().c_str()); return 1; }
		if (L.GetState() == FServerLink::EState::Connected && !bSentLogin)
		{
			printf("connected, server key %s, rtt %.0f\n", L.GetServerFingerprint().c_str(), L.GetRttMs());
			FWriter W; W.U8(CLogin); W.U8(Mode == "register" ? 1 : Mode == "token" ? 2 : 0); W.Str(Name); W.Str(Pw); W.U32(1); W.Str("linktest");
			L.SendReliable(W.B);
			bSentLogin = true;
		}
		for (auto& M : Msgs)
		{
			FReader R(M.data(), M.size(), 1);
			switch (M[0])
			{
			case SLoginResult:
			{
				const uint8_t Code = R.U8(); const std::string Msg = R.Str();
				printf("login result %d: %s\n", Code, Msg.c_str());
				if (Code == LoginOK)
				{
					const uint16_t Id = R.U16(); const std::string N = R.Str(); Token = R.Str(); const uint8_t Op = R.U8();
					const std::string Motd = R.Str(), SName = R.Str(); const uint16_t Max = R.U16();
					printf("  id %d name %s op %d token %s motd '%s' server '%s' max %d\n", Id, N.c_str(), Op, Token.empty() ? "(kept)" : "(new)", Motd.c_str(), SName.c_str(), Max);
					if (!Token.empty()) { FILE* F = fopen((Name + ".token").c_str(), "wb"); fputs(Token.c_str(), F); fclose(F); }
					bIn = true;
					FWriter W; W.U8(CLook); W.Str("bmx_street"); W.U32(0xff0000ff); W.U32(0xff00ff00); W.U32(0xffffffff); W.Str("abc");
					L.SendReliable(W.B);
					if (!Chat.empty()) { FWriter C; C.U8(CChat); C.Str(Chat); L.SendReliable(C.B); }
				}
				else if (Code == LoginNeedUpdate)
				{
					const std::string Ver = R.Str(); const uint32_t Build = R.U32(); Size = R.U64(); memcpy(Sha, R.Raw(32), 32); Chunk = R.U32();
					NumChunks = uint32_t((Size + Chunk - 1) / Chunk);
					printf("  update %s build %u size %llu chunks %u\n", Ver.c_str(), Build, (unsigned long long)Size, NumChunks);
					File.assign(Size, 0); Got.assign(NumChunks, 0); bDownloading = true; T0 = Now();
				}
				break;
			}
			case SPlayer: { const uint16_t Id = R.U16(); const std::string N = R.Str(); const uint8_t Op = R.U8(); const std::string V = R.Str(); printf("player %d %s op %d vehicle %s\n", Id, N.c_str(), Op, V.c_str()); break; }
			case SPlayerLeave: printf("player %d left\n", R.U16()); break;
			case SStates: States += R.U8(); break;
			case SChat: { const uint8_t K = R.U8(); const std::string F = R.Str(), Tx = R.Str(); printf("chat[%d] %s: %s\n", K, F.c_str(), Tx.c_str()); break; }
			case SKick: printf("KICKED: %s\n", R.Str().c_str()); return 3;
			case SVoice: ++Voices; break;
			case STeleport: { const uint8_t Md = R.U8(); const double A = R.F64(), B = R.F64(), C = R.F64(); printf("teleport mode %d %.4f %.4f %.1f\n", Md, A, B, C); break; }
			case SEnv: { const int8_t Wt = int8_t(R.U8()); const float H = R.F32(); printf("env weather %d hours %.2f\n", Wt, H); break; }
			case SUpdate: { const std::string V = R.Str(); printf("update notice %s build %u\n", V.c_str(), R.U32()); break; }
			case SFileChunk:
			{
				const uint32_t Idx = R.U32();
				const size_t Len = R.Left();
				const uint8_t* D = R.Raw(Len);
				if (bDownloading && Idx < NumChunks && !Got[Idx] && D) { memcpy(&File[size_t(Idx) * Chunk], D, Len); Got[Idx] = 1; ++Have; Outstanding.erase(Idx); Window = std::min(1024.0, Window + 1.0 / 8); }
				break;
			}
			default: break;
			}
		}
		if (bDownloading)
		{
			const double Timeout = std::max(0.4, L.GetRttMs() * 4 / 1000.0);
			std::vector<uint32_t> Req;
			for (auto It = Outstanding.begin(); It != Outstanding.end();)
			{
				if (T - It->second > Timeout) { Req.push_back(It->first); It->second = T; Window = std::max(16.0, Window * 0.85); }
				++It;
			}
			while (Outstanding.size() < size_t(Window) && NextReq < NumChunks) { if (!Got[NextReq]) { Outstanding[NextReq] = T; Req.push_back(NextReq); } ++NextReq; }
			for (size_t i = 0; i < Req.size(); i += 200)
			{
				FWriter W; W.U8(CFileReq); const size_t N = std::min<size_t>(200, Req.size() - i); W.U16(uint16_t(N));
				for (size_t j = 0; j < N; ++j) W.U32(Req[i + j]);
				L.SendNow(W.B);
			}
			if (Have == NumChunks)
			{
				uint8_t H[32]; FSha256::Hash(File.data(), File.size(), H);
				printf("downloaded %llu bytes in %.1fs (%.1f MB/s), sha256 %s\n", (unsigned long long)Size, Now() - T0, Size / 1e6 / (Now() - T0), memcmp(H, Sha, 32) == 0 ? "MATCHES" : "WRONG");
				return memcmp(H, Sha, 32) == 0 ? 0 : 4;
			}
		}
		if (bIn && T - LastState > 1.0 / 30)
		{
			LastState = T;
			FWriter W; W.U8(CState); W.F32(1000.f * float(T - T0)); W.F32(-2000.f); W.F32(500.f);
			for (int i = 0; i < StateSize - 12 - 4; ++i) W.U8(0);
			W.F32(float(T - T0));
			L.SendUnreliable(W.B);
		}
		if (bIn && T - LastVoice > 0.04) { LastVoice = T; FWriter W; W.U8(CVoice); W.U16(VoiceSeq++); for (int i = 0; i < 60; ++i) W.U8(uint8_t(i)); L.SendNow(W.B); }
		L.Flush(T);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	printf("done: got %d ride states, %d voice packets, rtt %.1f ms\n", States, Voices, L.GetRttMs());
	L.Close(true);
	return 0;
}
