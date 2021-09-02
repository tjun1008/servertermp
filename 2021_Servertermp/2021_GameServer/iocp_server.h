#include <iostream>
#include <thread>
#include <vector>
using namespace std;
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <mutex>
#include <unordered_set> //셋보다 더 성능이 좋음
#include <array>
#include <string>
#include <set>
#include <queue>
#include <atomic>
#include <atlstr.h>
#include <random>

#include <windows.h>  
#include <sqlext.h>  //db

#include "protocol.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "MSWSock.lib")
#pragma comment (lib, "lua54.lib")

constexpr int NUM_THREADS = 4;

enum OP_TYPE { OP_RECV, OP_SEND, OP_ACCEPT, OP_RANDOM_MOVE, OP_PLAYER_HP_RECOVER, OP_PLAYER_MOVE, OP_NPC_ATTACK, OP_NPC_RESPAWN };
enum S_STATE { STATE_FREE, STATE_CONNECTED, STATE_INGAME, STATE_ACTIVE, STATE_SLEEP };
enum OBJ_TYPE { PLAYER, ORC, DRAGON };
enum NPC_TYPE { PEACE, AGRO };
// Peace: : 때리기 전에는 가만히
//Agro : 근처에 11x11 영역에 접근하면 쫓아 오기

enum NPC_MOVE { FIX, ROAMING };

struct EX_OVER {
	WSAOVERLAPPED	m_over;
	WSABUF			m_wsabuf[1];
	unsigned char	m_netbuf[MAX_BUFFER];
	OP_TYPE			m_op;
	SOCKET m_csocket;
	int m_target_id;
};

struct SESSION
{
	int				m_id;
	int login_id;

	// contents
	char	m_name[MAX_ID_LEN];
	short	m_x, m_y;
	int level;
	int hp;
	int exp;

	NPC_TYPE npcType;
	NPC_MOVE npcMove;

	EX_OVER			m_recv_over;
	unsigned char	m_prev_recv;
	SOCKET   m_s;

	atomic<S_STATE>	m_state; // 0. free 1. connected 2. ingame
	mutex m_lock;

	int last_move_time;
	unordered_set <int> m_viewlist;
	mutex m_vl;

	SESSION* target = nullptr;
	mutex m_tl; //target lock

	unsigned m_attack_time;

	lua_State* L;
	mutex m_sl; //lua lock
};

struct SECTOR_INFO {
	unordered_set<SESSION*> m_client;											//섹터에 있는 클라이언트들
	mutex m_sl;
};

struct timer_event {
	int object_id;
	OP_TYPE event_type;
	chrono::system_clock::time_point exec_time;
	int target_id;

	constexpr bool operator < (const timer_event& l) const
	{
		return exec_time > l.exec_time;
	}
};