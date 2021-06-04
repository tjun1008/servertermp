
#include "iocp_server.h"

priority_queue <timer_event> timer_queue;
mutex timer_lock;

array <SESSION, MAX_USER + 1> Clients;
SECTOR_INFO g_ObjectListSector[WORLD_WIDTH][WORLD_HEIGHT];			//섹터마다의 object관리

SOCKET listenSocket;
HANDLE h_iocp;

bool g_Map[WORLD_WIDTH][WORLD_HEIGHT];

void init_map()
{
	char data;
	FILE* fp = fopen("Map_Data.txt", "rb");

	int count = 0;
	int i = 0, j = 0;


	while (fscanf(fp, "%c", &data) != EOF) {
		//printf("%c", data);

		switch (data)
		{
		case '0':
			if (j < WORLD_WIDTH)
			{
				g_Map[i][j] = true;
				++j;
			}
			else
			{
				j = 0;
				++i;
				g_Map[i][j] = true;
				++j;

				//printf("%d\n", i);
			}
			break;
		case '1':
			if (j < WORLD_WIDTH)
			{
				g_Map[i][j] = false;
				++j;
			}
			else
			{
				j = 0;
				++i;
				g_Map[i][j] = false;
				++j;

				//printf("%d\n", i);

			}
			break;
		default:
			break;
		}
	}
}

void display_error(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << L" 에러 " << lpMsgBuf << endl;
	LocalFree(lpMsgBuf);
}

void add_event(int id, OP_TYPE ev, int delay_ms)
{
	using namespace chrono;
	timer_event event{ id, ev, system_clock::now() + microseconds(delay_ms),0 };
	timer_lock.lock();
	timer_queue.push(event);
	timer_lock.unlock();
}

void activate_npc(int id)
{

	auto old_status = STATE_SLEEP;
	if (true == atomic_compare_exchange_strong(&Clients[id].m_state, &old_status, STATE_ACTIVE))
	{
		add_event(id, OP_RANDOM_MOVE, 1000);
		//cout << "activate_npc\t";
	}
}


bool is_npc(int id)
{
	return id >= NPC_ID_START;
}

unordered_set<int> get_nearVl(int id)
{
	unordered_set<int> viewlist;
	SESSION& pl = Clients[id];

	int viewX_Min = max(pl.m_x - VIEW_RADIUS, 0);
	int viewX_Max = min(pl.m_x + VIEW_RADIUS, WORLD_WIDTH);
	int viewY_Min = max(pl.m_y - VIEW_RADIUS, 0);
	int viewY_Max = min(pl.m_y + VIEW_RADIUS, WORLD_HEIGHT);

	for (int i = viewX_Min; i < viewX_Max; ++i)
	{
		for (int j = viewY_Min; j < viewY_Max; ++j)
		{
			for (auto& cl : g_ObjectListSector[i][j].m_client)
			{

				if (cl == &pl)continue;
				viewlist.emplace(cl->m_id);
			}
		}
	}
	return viewlist;
}


bool can_see(int id_a, int id_b)
{

	return VIEW_RADIUS * VIEW_RADIUS >= (Clients[id_a].m_x - Clients[id_b].m_x)
		* (Clients[id_a].m_x - Clients[id_b].m_x)
		+ (Clients[id_a].m_y - Clients[id_b].m_y)
		* (Clients[id_a].m_y - Clients[id_b].m_y);
}

void send_packet(int p_id, void* buf)
{
	EX_OVER* s_over = new EX_OVER;

	unsigned char packet_size = reinterpret_cast<unsigned char*>(buf)[0];
	s_over->m_op = OP_SEND;
	memset(&s_over->m_over, 0, sizeof(s_over->m_over));
	memcpy(s_over->m_netbuf, buf, packet_size);
	s_over->m_wsabuf[0].buf = reinterpret_cast<char*>(s_over->m_netbuf);
	s_over->m_wsabuf[0].len = packet_size;

	WSASend(Clients[p_id].m_s, s_over->m_wsabuf, 1, 0, 0, &s_over->m_over, 0);
}

void send_login_info(int p_id)
{
	sc_packet_login_ok packet;
	packet.id = p_id;
	packet.HP = Clients[p_id].hp;
	packet.LEVEL = Clients[p_id].level;
	packet.EXP = Clients[p_id].exp;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_OK;
	packet.x = Clients[p_id].m_x;
	packet.y = Clients[p_id].m_y;
	send_packet(p_id, &packet);
}

void send_move_packet(int c_id, int p_id)
{
	
	sc_packet_position packet;
	packet.id = p_id;
	packet.size = sizeof(packet);
	packet.type = SC_POSITION;
	packet.x = Clients[p_id].m_x;
	packet.y = Clients[p_id].m_y;
	packet.move_time = Clients[p_id].last_move_time;
	send_packet(c_id, &packet);
}

void send_pc_login(int c_id, int p_id)
{
	sc_packet_add_object packet;
	packet.id = p_id;
	packet.size = sizeof(packet);
	packet.type = SC_ADD_OBJECT;
	packet.x = Clients[p_id].m_x;
	packet.y = Clients[p_id].m_y;
	packet.EXP = Clients[p_id].exp;
	packet.HP = Clients[p_id].hp;
	packet.LEVEL = Clients[p_id].level;
	strcpy_s(packet.name, Clients[p_id].m_name);
	if (true == is_npc(p_id))
	{
		packet.obj_class = ORC; 
	}
	else
		packet.obj_class = PLAYER;


	send_packet(c_id, &packet);
}

void send_chat_packet(int c_id, int p_id, char* mess)
{
	sc_packet_chat p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_CHAT;
	strcpy_s(p.message, mess);
	send_packet(c_id, &p);
}

void send_pc_logout(int c_id, int p_id)
{
	sc_packet_remove_object packet;
	packet.id = p_id;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;


	send_packet(c_id, &packet);
}

void player_move(int p_id, char dir)
{
	SESSION& u = Clients[p_id];
	short x = u.m_x;
	short y = u.m_y;
	switch (dir) {
	case 0: if (y > 0) y--; break;
	case 1: if (x < (WORLD_WIDTH - 1)) x++; break;
	case 2: if (y < (WORLD_HEIGHT - 1)) y++; break;
	case 3: if (x > 0) x--; break;
	}

	if (g_Map[y][x]) {

		g_ObjectListSector[x][y].m_sl.lock();

		g_ObjectListSector[u.m_x][u.m_y].m_client.erase(&u);
		g_ObjectListSector[x][y].m_client.emplace(&u);

		g_ObjectListSector[x][y].m_sl.unlock();

		u.m_x = x;
		u.m_y = y;

		unordered_set<int> old_vl;
		Clients[p_id].m_vl.lock();
		old_vl = Clients[p_id].m_viewlist;
		Clients[p_id].m_vl.unlock();

		unordered_set <int> new_vl;

		for (auto& vl_id : get_nearVl(p_id)) //sector
		{
			if (STATE_SLEEP == Clients[vl_id].m_state)
			{
				activate_npc(Clients[vl_id].m_id);		//sleep npc -> active npc
			}

			if (STATE_INGAME != Clients[vl_id].m_state) continue;
			if (vl_id == p_id) continue;

			//if (true == is_npc(vl_id))continue;
			//if (can_see(p_id, vl_id))
			new_vl.insert(vl_id);

		}


		send_move_packet(p_id, p_id);

		for (auto& pl : new_vl) {

			if (0 == old_vl.count(pl))
			{
				// 1. 새로 시야에 들어오는 플레이어 처리
				Clients[p_id].m_vl.lock();
				Clients[p_id].m_viewlist.insert(pl);
				Clients[p_id].m_vl.unlock();
				send_pc_login(p_id, pl);

				if (true == is_npc(pl))continue;

				Clients[pl].m_vl.lock();

				if (0 == Clients[pl].m_viewlist.count(p_id))
				{
					Clients[pl].m_viewlist.insert(p_id);
					Clients[pl].m_vl.unlock();
					send_pc_login(pl, p_id); //이름 바꾸기
				}
				else
				{
					Clients[pl].m_vl.unlock();
					send_move_packet(pl, p_id);
				}

			}
			else
			{
				//2. 처음부터 끝까지 시야에 존재하는 플레이어 처리
				//players[p_id].m_vl.unlock();

				if (true == is_npc(pl))continue;

				Clients[pl].m_vl.lock();

				if (0 != Clients[pl].m_viewlist.count(p_id)) {

					Clients[pl].m_vl.unlock();

					send_move_packet(pl, p_id);
				}
				else
				{
					Clients[pl].m_viewlist.insert(p_id);
					Clients[pl].m_vl.unlock();
					send_pc_login(pl, p_id); //이름 바꾸기

				}

			}

		}

		//3. 시야에서 벗어나는 플레이어 처리

		for (auto pl : old_vl)
		{
			if (0 == new_vl.count(pl)) {
				send_pc_logout(p_id, pl);
				Clients[p_id].m_vl.lock();
				Clients[p_id].m_viewlist.erase(pl);
				Clients[p_id].m_vl.unlock();

				send_pc_logout(p_id, pl);

				if (true == is_npc(pl))continue;

				if (0 != Clients[pl].m_viewlist.count(p_id))
				{
					Clients[pl].m_vl.lock();
					Clients[pl].m_viewlist.erase(p_id);
					Clients[pl].m_vl.unlock();
					send_pc_logout(pl, p_id);
				}
			}

		}
	}

}

void do_random_move_npc(int id)
{

	unordered_set<int> old_vl = get_nearVl(id);

	int x = Clients[id].m_x;
	int y = Clients[id].m_y;

	switch (rand() % 4) {
	case 0: if (x > 0) x--; break;
	case 1: if (x < (WORLD_WIDTH - 1))x++; break;
	case 2:  if (y > 0)y--; break;
	case 3: if (y < (WORLD_HEIGHT - 1)) y++; break;
	}

	//g_ObjectListSector[x][y].m_sl.lock();

	//g_ObjectListSector[players[id].m_x][players[id].m_y].m_client.erase(&players[id]);
	//g_ObjectListSector[x][y].m_client.emplace(&u);

	//g_ObjectListSector[x][y].m_sl.unlock();

	if (g_Map[y][x]) {

		Clients[id].m_x = x;
		Clients[id].m_y = y;

		unordered_set <int> new_vl = get_nearVl(id);



		for (auto& pl : new_vl) {
			if (true == is_npc(pl)) continue;
			if (STATE_INGAME != Clients[pl].m_state) continue;
			Clients[pl].m_vl.lock();
			if (0 != Clients[pl].m_viewlist.count(id))
			{
				Clients[pl].m_vl.unlock();
				send_move_packet(pl, id);
				//cout << "npc 이동\n";
			}
			else
			{
				Clients[pl].m_vl.unlock();

				Clients[pl].m_vl.lock();
				Clients[pl].m_viewlist.insert(id);
				Clients[pl].m_vl.unlock();
				send_pc_login(pl, id);
			}

		}



		for (auto pl : old_vl) {
			if (0 != new_vl.count(pl)) continue;

			Clients[pl].m_vl.lock();
			if (0 != Clients[pl].m_viewlist.count(id))
			{
				Clients[pl].m_vl.unlock();
				Clients[pl].m_vl.lock();
				Clients[pl].m_viewlist.erase(id);
				Clients[pl].m_vl.unlock();
				send_pc_logout(pl, id);
			}
			else {
				Clients[pl].m_vl.unlock();
			}
		}

	}


}


void do_accept(SOCKET s_socket, EX_OVER* a_over)
{
	SOCKET  c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	memset(&a_over->m_over, 0, sizeof(a_over->m_over));
	DWORD num_byte;
	int addr_size = sizeof(SOCKADDR_IN) + 16;
	a_over->m_csocket = c_socket;
	BOOL ret = AcceptEx(s_socket, c_socket, a_over->m_netbuf, 0, addr_size, addr_size, &num_byte, &a_over->m_over);
	if (FALSE == ret) {
		int err = WSAGetLastError();
		if (WSA_IO_PENDING != err) {
			display_error("AcceptEx : ", err);
			exit(-1);
		}
	}
}

void process_packet(int p_id, unsigned char* packet)
{
	cs_packet_login* p = reinterpret_cast<cs_packet_login*>(packet);
	switch (p->type) {
	case CS_LOGIN:
	{
		lock_guard<mutex> guard2{ Clients[p_id].m_lock };
		//players[p_id].m_lock.lock();
		strcpy_s(Clients[p_id].m_name, p->player_id);
		//Clients[p_id].m_x = rand() % WORLD_WIDTH;
		//Clients[p_id].m_y = rand() % WORLD_HEIGHT;

		int mx;
		int my;
		while (true)
		{
			mx = rand() % WORLD_WIDTH;
			my = rand() % WORLD_HEIGHT;

			if (g_Map[my][mx])
				break;
		}

		Clients[p_id].m_x = mx;
		Clients[p_id].m_y = my;
		Clients[p_id].exp = 0;
		Clients[p_id].hp = 100;
		Clients[p_id].level = 1;

		send_login_info(p_id);
		Clients[p_id].m_state = STATE_INGAME;


		g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_sl.lock();

		g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_client.emplace(&Clients[p_id]);

		g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_sl.unlock();



		for (auto& i : get_nearVl(p_id)) {

			if (STATE_SLEEP == Clients[i].m_state)
			{
				activate_npc(i);


			}

			if (STATE_INGAME == Clients[i].m_state || STATE_ACTIVE == Clients[i].m_state) {

				Clients[p_id].m_vl.lock();
				Clients[p_id].m_viewlist.insert(i);
				Clients[p_id].m_vl.unlock();

				send_pc_login(p_id, i);


				if (false == is_npc(i))
				{
					Clients[i].m_vl.lock();
					Clients[i].m_viewlist.insert(p_id);
					Clients[i].m_vl.unlock();

					send_pc_login(i, p_id);


				}

			}


		}

	}

	break;

	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		Clients[p_id].last_move_time = move_packet->move_time;
		player_move(p_id, move_packet->direction);
		break;
	}
	case CS_CHAT: {
		cs_packet_chat* chat_packet = reinterpret_cast<cs_packet_chat*>(packet);
		//char test[MAX_STR_LEN];
		//memcpy(test, chat_packet->message, sizeof(chat_packet->message)); //chat_packet->message에서 오류남
		//cout << test << endl;
		for (auto& pl : get_nearVl(p_id))
		{
			if (!is_npc(pl))
				send_chat_packet(pl, p_id, chat_packet->message); //상대방에게
		}
		send_chat_packet(p_id, p_id, chat_packet->message); //나에게
	}
				break;
	default:
		cout << "Unknown Packet Type [" << p->type << "] Error\n";
		exit(-1);
	}
}

void do_recv(int p_id)
{
	SESSION& pl = Clients[p_id];
	EX_OVER& r_over = pl.m_recv_over;
	// r_over.m_op = OP_RECV;
	memset(&r_over.m_over, 0, sizeof(r_over.m_over));
	r_over.m_wsabuf[0].buf = reinterpret_cast<CHAR*>(r_over.m_netbuf) + pl.m_prev_recv;
	r_over.m_wsabuf[0].len = MAX_BUFFER - pl.m_prev_recv;
	DWORD r_flag = 0;
	WSARecv(pl.m_s, r_over.m_wsabuf, 1, 0, &r_flag, &r_over.m_over, 0);
}

int get_new_player_id()
{

	for (int i = 0; i < MAX_USER; ++i) {
		//players[i].m_lock.lock();
		lock_guard<mutex> guard0{ Clients[i].m_lock };
		if (STATE_FREE == Clients[i].m_state)
		{
			Clients[i].m_state = STATE_CONNECTED;

			//players[i].m_lock.unlock();
			return i;
		}
		//players[i].m_lock.unlock();
	}


	return -1;

}

void init_server()
{
	//네트워크 초기화

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);


	// bind() listen()
	listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY); //IP주소를 자동으로 찾아서 대입
	bind(listenSocket, (struct sockaddr*)&serverAddr, sizeof(SOCKADDR_IN));
	listen(listenSocket, SOMAXCONN);

	//iocp 커널 객체 생성
	h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);

}

void init_npc()
{
	for (int i = NPC_ID_START; i < MAX_USER; ++i)
	{
		auto& npc = Clients[i];
		npc.m_id = i;
		npc.m_state = STATE_SLEEP;
		npc.last_move_time = 0;

		strcpy_s(npc.m_name, to_string(i).c_str()); //이름

		npc.m_x = rand() % WORLD_WIDTH;
		npc.m_y = rand() % WORLD_HEIGHT; //랜덤으로 뿌려줌
		npc.level = rand() % 5 + 1; //차후 수정
		npc.hp = 100;

		add_event(i, OP_RANDOM_MOVE, 1000);

		g_ObjectListSector[npc.m_x][npc.m_y].m_sl.lock();

		g_ObjectListSector[npc.m_x][npc.m_y].m_client.emplace(&npc);

		g_ObjectListSector[npc.m_x][npc.m_y].m_sl.unlock(); //섹터에 넣어줌
	}
}

void client_init()
{
	for (int i = 0; i < NPC_ID_START; ++i)
	{
		auto& pl = Clients[i];
		pl.m_id = i;
		pl.m_state = STATE_FREE;
		pl.last_move_time = 0;
	}
}

void disconnect(int p_id)
{

	g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_sl.lock();

	g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_client.erase(&Clients[p_id]);

	g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_sl.unlock();

	send_pc_logout(p_id, p_id);

	Clients[p_id].m_lock.lock();
	Clients[p_id].m_state = STATE_CONNECTED;

	//old_vl

	closesocket(Clients[p_id].m_s);

	for (int i = 0; i < NPC_ID_START; ++i) {
		if (true == is_npc(i))continue;
		SESSION& cl = Clients[i];
		if (p_id == cl.m_id) continue;
		//cl.m_cl.lock();
		if (STATE_INGAME == cl.m_state)
			send_pc_logout(cl.m_id, p_id);
		//cl.m_cl.unlock();
	}

	Clients[p_id].m_state = STATE_FREE;
	Clients[p_id].m_lock.unlock();


}

void do_timer()
{
	using namespace chrono;

	while (true) {

		this_thread::sleep_for(1000ms); //너무 빨리 움직여서 조정

		while (true) {
			if (false == timer_queue.empty()) {
				auto ev = timer_queue.top();
				if (ev.exec_time > system_clock::now()) break;
				timer_lock.lock();
				timer_queue.pop();
				timer_lock.unlock();

				switch (ev.event_type) {
				case OP_RANDOM_MOVE: {
					EX_OVER* ex_over = new EX_OVER;
					ex_over->m_op = OP_RANDOM_MOVE;
					PostQueuedCompletionStatus(h_iocp, 1, ev.object_id, &ex_over->m_over);
					//cout << "정리\n";
					//do_random_move_npc(ev.object_id);
					//add_event(ev.object_id, OP_RANDOM_MOVE, 1000); //worker thread로 넘김
				}

								   break;
				}
			}

		}

	}

}

void worker()
{
	while (true) {
		DWORD num_byte;
		ULONG_PTR i_key;
		WSAOVERLAPPED* over;
		BOOL ret = GetQueuedCompletionStatus(h_iocp, &num_byte, &i_key, &over, INFINITE);
		int key = static_cast<int> (i_key);
		if (FALSE == ret) {
			int err = WSAGetLastError();
			display_error("GQCS : ", err);
			disconnect(key);
			continue;
		}

		EX_OVER* ex_over = reinterpret_cast<EX_OVER*>(over);

		switch (ex_over->m_op) {
		case OP_RECV:
		{
			/*
			SESSION& cu = Clients[key];

			unsigned char* ps = ex_over->m_netbuf; //데이터의 포인터
			int remain_data = num_byte + cu.m_prev_recv;

			while (0 < remain_data)
			{
				int packet_size = ps[0];

				if (packet_size <= remain_data) //남은 데이터로 패킷하나를 완성할 수 있나?
				{

					process_packet(key, ps);
					remain_data -= packet_size;
					ps += packet_size;
				}
				else
				{
					memcpy(ex_over->m_netbuf, ps, remain_data);
					cu.m_prev_recv = remain_data;
					
				}
			}
				*/

			unsigned char* ps = ex_over->m_netbuf;
			int remain_data = num_byte + Clients[key].m_prev_recv;
			while (remain_data > 0) {
				int packet_size = ps[0];
				if (packet_size > remain_data) break;
				process_packet(key, ps);
				remain_data -= packet_size;
				ps += packet_size;
			}
			if (remain_data > 0)
				memcpy(ex_over->m_netbuf, ps, remain_data);
			Clients[key].m_prev_recv = remain_data;
			do_recv(key);

		//일단 패킷조립 이렇게 하고.. 만약 안되면 기존꺼쓰자ㅠ

		}
		break;
		case OP_SEND:
			if (num_byte != ex_over->m_wsabuf[0].len)
				disconnect(key);
			delete ex_over;
			break;
		case OP_ACCEPT:
		{
			SOCKET c_socket = ex_over->m_csocket;
			int p_id = get_new_player_id();
			if (-1 == p_id) {
				closesocket(Clients[p_id].m_s);
				do_accept(listenSocket, ex_over);
				continue;
			}

			Clients[p_id].m_prev_recv = 0;
			Clients[p_id].m_recv_over.m_op = OP_RECV;
			Clients[p_id].m_s = c_socket;
			Clients[p_id].m_viewlist.clear();

			CreateIoCompletionPort(reinterpret_cast<HANDLE>(Clients[p_id].m_s), h_iocp, p_id, 0);


			//g_ObjectListSector[players[p_id].m_x][players[p_id].m_y].m_sl.lock();

			//g_ObjectListSector[players[p_id].m_x][players[p_id].m_y].m_client.emplace(&players[p_id]);

			//g_ObjectListSector[players[p_id].m_x][players[p_id].m_y].m_sl.unlock();



			do_recv(p_id);
			do_accept(listenSocket, ex_over);
			cout << "New CLient [" << p_id << "] connected.\n";
		}
		break;
		case OP_RANDOM_MOVE:
		{

			//do_random_move_npc(key);
			//add_event(key, OP_RANDOM_MOVE, 1000);


			bool npc_alive = false;

			for (auto& i : get_nearVl(key)) {

				//if (i >= NPC_ID_START) continue;

				if (STATE_INGAME == Clients[i].m_state) {
					npc_alive = true;
					break;
				}
			}
			if (true == npc_alive)
			{
				do_random_move_npc(key);
				add_event(key, OP_RANDOM_MOVE, 1000);
			}
			else Clients[key].m_state = STATE_SLEEP;


			delete ex_over;

		}
		break;
	
		default: cout << "Unknown GQCS Error!\n";
			exit(-1);
		}
	}
}


int main()
{

	wcout.imbue(locale("korean"));

	init_map();
	//g_PathFinder = new PathFinder(map_data);

	cout << "npc init start\n";
		init_npc();
	cout << "npc init finish\n";

	init_server();

	client_init();

	// iocp 객체와 소켓 연결
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(listenSocket),
		h_iocp, 100000, 0); 

	EX_OVER a_over;
	a_over.m_op = OP_ACCEPT;
	do_accept(listenSocket, &a_over);

	vector <thread> worker_threads;
	for (int i = 0; i < NUM_THREADS; ++i)
		worker_threads.emplace_back(worker);

	thread timer_thread{ do_timer };
	timer_thread.join();

	for (auto& th : worker_threads) th.join();
	closesocket(listenSocket);
	WSACleanup();
}
