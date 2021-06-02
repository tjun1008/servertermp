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

#include "protocol.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "MSWSock.lib")

constexpr int NUM_THREADS = 4;

enum OP_TYPE { OP_RECV, OP_SEND, OP_ACCEPT, OP_RANDOM_MOVE, OP_PLAYER_HP_RECOVER};
enum S_STATE { STATE_FREE, STATE_CONNECTED, STATE_INGAME, STATE_ACTIVE, STATE_SLEEP };
enum OBJ_TYPE {PLAYER, ORC, DRAGON};
struct EX_OVER {
	WSAOVERLAPPED	m_over;
	WSABUF			m_wsabuf[1];
	unsigned char	m_netbuf[MAX_BUFFER];
	OP_TYPE			m_op;
	SOCKET m_csocket;
};

struct SESSION
{
	int				m_id;

	// contents
	char	m_name[MAX_ID_LEN];
	short	m_x, m_y;
	int level;
	int hp;
	int exp;

	char npcType; //0-peace / 1-argo
	char npcMoveType; //0-고정 / 1-로밍

	EX_OVER			m_recv_over;
	unsigned char	m_prev_recv;
	SOCKET   m_s;

	atomic<S_STATE>	m_state; // 0. free 1. connected 2. ingame
	mutex m_lock;
	
	int last_move_time;
	unordered_set <int> m_viewlist;
	mutex m_vl;
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