
#include "iocp_server.h"

priority_queue <timer_event> timer_queue;
mutex timer_lock;

array <SESSION, MAX_USER + 1> Clients;
SECTOR_INFO g_ObjectListSector[WORLD_WIDTH][WORLD_HEIGHT];			//섹터마다의 object관리

SOCKET listenSocket;
HANDLE h_iocp;

short start_x;
short start_y;

bool Map[WORLD_WIDTH][WORLD_HEIGHT];

//Astar* g_Astar;

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
				Map[i][j] = true;
				++j;
			}
			else
			{
				j = 0;
				++i;
				Map[i][j] = true;
				++j;

				//printf("%d\n", i);
			}
			break;
		case '1':
			if (j < WORLD_WIDTH)
			{
				Map[i][j] = false;
				++j;
			}
			else
			{
				j = 0;
				++i;
				Map[i][j] = false;
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

void db_error_display(SQLHANDLE hHandle, SQLSMALLINT hType, RETCODE RetCode)
{
	SQLSMALLINT iRec = 0;
	SQLINTEGER iError;
	WCHAR wszMessage[1000];
	WCHAR wszState[SQL_SQLSTATE_SIZE + 1];
	if (RetCode == SQL_INVALID_HANDLE) {
		fwprintf(stderr, L"Invalid handle!\n");
		return;
	}
	while (SQLGetDiagRec(hType, hHandle, ++iRec, wszState, &iError, wszMessage,
		(SQLSMALLINT)(sizeof(wszMessage) / sizeof(WCHAR)), (SQLSMALLINT*)NULL) == SQL_SUCCESS) {
		// Hide data truncated..
		if (wcsncmp(wszState, L"01004", 5)) {
			fwprintf(stderr, L"[%5.5s] %s (%d)\n", wszState, wszMessage, iError);
		}
	}
}

void add_event(int id, OP_TYPE ev, int delay_ms)
{
	using namespace chrono;
	timer_event event{ id, ev, system_clock::now() + milliseconds(delay_ms),0 };
	timer_lock.lock();
	timer_queue.emplace(event);
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

bool attack_range(int id_a, int id_b)
{
	if (abs(Clients[id_a].m_x - Clients[id_b].m_x) > 1) return false;
	if (abs(Clients[id_a].m_y - Clients[id_b].m_y) > 1) return false;

	return true;
}

bool can_see(int id_a, int id_b)
{

	return VIEW_RADIUS * VIEW_RADIUS >= (Clients[id_a].m_x - Clients[id_b].m_x)
		* (Clients[id_a].m_x - Clients[id_b].m_x)
		+ (Clients[id_a].m_y - Clients[id_b].m_y)
		* (Clients[id_a].m_y - Clients[id_b].m_y);
}

bool find_db(int id)
{
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	SQLWCHAR szUser_Name[MAX_ID_LEN];
	SQLINTEGER dUser_id, dUser_x, dUser_y, dUser_LEVEL, dUser_EXP, dUser_HP;

	SQLWCHAR query[1024];
	wsprintf(query, L"EXECUTE find_userdata %d ", Clients[id].login_id);
	bool no_data = true;

	setlocale(LC_ALL, "korean");
	std::wcout.imbue(std::locale("korean"));

	SQLLEN cbName = 0, cbID = 0, cbX, cbY = 0, cbHP, cbEXP, cbLEVEL; //초기화

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	// Set the ODBC version environment attribute  
	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"GSDB", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

					//printf("DB 연결 성공 \n");

				
					retcode = SQLExecDirect(hstmt, (SQLWCHAR*)query, SQL_NTS);
				
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {

						//printf("Select OK \n");

						// Bind columns 1, 2, and 3  
						retcode = SQLBindCol(hstmt, 1, SQL_C_LONG, &dUser_id, 100, &cbID);
						retcode = SQLBindCol(hstmt, 2, SQL_C_WCHAR, szUser_Name, MAX_ID_LEN, &cbName);
						retcode = SQLBindCol(hstmt, 3, SQL_C_LONG, &dUser_LEVEL, 100, &cbLEVEL);	
						retcode = SQLBindCol(hstmt, 4, SQL_C_LONG, &dUser_HP, 100, &cbHP);
						retcode = SQLBindCol(hstmt, 5, SQL_C_LONG, &dUser_EXP, 100, &cbEXP);
						retcode = SQLBindCol(hstmt, 6, SQL_C_LONG, &dUser_x, 100, &cbX);
						retcode = SQLBindCol(hstmt, 7, SQL_C_LONG, &dUser_y, 100, &cbY);

						// Fetch and print each row of data. On an error, display a message and exit.  
						for (int i = 0; ; i++) {
							retcode = SQLFetch(hstmt);
							if (retcode == SQL_ERROR || retcode == SQL_SUCCESS_WITH_INFO)
							{
								//show_error();
								db_error_display(hstmt, SQL_HANDLE_STMT, retcode);
							}
							if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO)
							{
								//wprintf(L"ID :%d  NICK : \'%s\' \t(%d,%d)\t LEV : %d\t EXP : %d\t HP : %d\n", dUser_id, szUser_Name, dUser_x, dUser_y, dUser_LEVEL, dUser_EXP, dUser_HP);
								char* temp;
								int strSize = WideCharToMultiByte(CP_ACP, 0, szUser_Name, -1, NULL, 0, NULL, NULL);
								temp = new char[11];
								WideCharToMultiByte(CP_ACP, 0, szUser_Name, -1, temp, strSize, 0, 0);

								no_data = false;
								Clients[id].m_x = dUser_x;
								Clients[id].m_y = dUser_y;
								Clients[id].hp = dUser_HP;
								Clients[id].level = dUser_LEVEL;
								Clients[id].exp = dUser_EXP;


								memcpy_s(Clients[id].m_name, 10, temp, 10);
								//strcpy_s(Clients[id].m_name, temp);
								//cout << "'" << Clients[id].m_name << "'" << endl;
								//cout << Clients[id].m_id << endl;

							}
							else
								break;
						}
					}

					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt);
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}
				else
					db_error_display(hdbc, SQL_HANDLE_DBC, retcode);

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);

			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}
	if (no_data) {
		
		return false;
	}
	else {
		
		return true;
	}

	//wprintf(L"database end \n");
}

void InsertData(int keyid, char* name, int level, int hp, int exp, int x, int y)
{
	SQLHENV henv;		
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0; 
	SQLRETURN retcode;  
	SQLWCHAR query[1024];

	CString str = name;

	wsprintf(query, L"INSERT INTO char_table (char_id, char_name, char_level,char_hp, char_exp, char_x, char_y ) VALUES (%d, \'%s\', %d, %d, %d, %d, %d)",
		keyid, str, level, hp, exp, x, y);


	setlocale(LC_ALL, "korean"); // 오류코드 한글로 변환
	//std::wcout.imbue(std::locale("korean"));

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0); 

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0); 

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"GSDB", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);
				//retcode = SQLConnect(hdbc, (SQLWCHAR*)L"jys_gameserver", SQL_NTS, (SQLWCHAR*)NULL, SQL_NTS, NULL, SQL_NTS);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt); 

					retcode = SQLExecDirect(hstmt, (SQLWCHAR*)query, SQL_NTS); 
					

					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						//printf("DataBase insert success user_x = %d, user_y = %d, user_EXP = %d, user_HP = %d, user_LEVEL = %d WHERE user_id = %d \n",
						//	x, y, exp, hp, level, keyid);

					}
					else
						db_error_display(hdbc, SQL_HANDLE_DBC, retcode);


					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt); // 핸들캔슬
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}

}

void UpdateData(int keyid, int level, int hp, int exp, int x, int y)
{
	SQLHENV henv;		
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0; 
	SQLRETURN retcode;  
	SQLWCHAR query[1024];
	wsprintf(query, L"UPDATE char_table SET char_x = %d, char_y = %d, char_exp = %d, char_hp = %d, char_level = %d WHERE char_id = %d", x, y, exp, hp, level, keyid);


	//cout << "update data\n";
	setlocale(LC_ALL, "korean"); // 오류코드 한글로 변환
	//std::wcout.imbue(std::locale("korean"));

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0); // ODBC로 연결

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0); // 5초간 연결 5초넘어가면 타임아웃

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"GSDB", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);
				//retcode = SQLConnect(hdbc, (SQLWCHAR*)L"jys_gameserver", SQL_NTS, (SQLWCHAR*)NULL, SQL_NTS, NULL, SQL_NTS);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt); // SQL명령어 전달할 한들

					retcode = SQLExecDirect(hstmt, (SQLWCHAR*)query, SQL_NTS); // 쿼리문
			

					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						//printf("DataBase update success user_x = %d, user_y = %d, user_EXP = %d, user_HP = %d, user_LEVEL = %d WHERE user_id = %d \n",
						//	x, y, exp, hp, level, keyid);
					}


					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt); // 핸들캔슬
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}
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
	strcpy_s(packet.name, Clients[p_id].m_name);
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
		packet.monster_type = Clients[p_id].npcType;
		packet.monster_move = Clients[p_id].npcMove;
	}
	else
		packet.obj_class = PLAYER;


	send_packet(c_id, &packet);
}

void send_chat(int c_id, int p_id, const char* mess)
{
	sc_packet_chat p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_CHAT;
	strcpy_s(p.message, mess);
	send_packet(c_id, &p);
}

void send_stat_change(int c_id, int p_id)
{
	sc_packet_stat_change packet;
	packet.id = p_id;
	packet.size = sizeof(packet);
	packet.type = SC_STAT_CHANGE;
	packet.EXP = Clients[p_id].exp;
	packet.HP = Clients[p_id].hp;
	packet.LEVEL = Clients[p_id].level;

	send_packet(c_id, &packet);
}

void send_login_fail_packet(int p_id)
{
	cout << "login fail\n";

	sc_packet_login_fail packet;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_FAIL;
	send_packet(p_id, &packet);
}

void send_pc_logout(int c_id, int p_id)
{
	sc_packet_remove_object packet;
	packet.id = p_id;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;


	send_packet(c_id, &packet);
}

void player_levelUp(int p_id)
{
	if (Clients[p_id].exp >= (int)(100 * pow(2, (Clients[p_id].level - 1))))
	{
		Clients[p_id].exp = 0;
		Clients[p_id].hp = 100;
		Clients[p_id].level += 1;
	}
}

void npc_dead(int p_id, int m_id)
{
	char mess[100];

	Clients[m_id].hp = 0;

	if (Clients[m_id].npcType == AGRO || Clients[m_id].npcMove == ROAMING)
	{
		Clients[p_id].exp += (Clients[m_id].level+1) * (Clients[m_id].level + 1) * 2 * 2;
		sprintf_s(mess, "%s add EXP (+%d).", Clients[p_id].m_name, (Clients[m_id].level + 1) * (Clients[m_id].level + 1) * 2 * 2);
	}
	else
	{
		Clients[p_id].exp += (Clients[m_id].level + 1) * (Clients[m_id].level + 1) * 2;
		sprintf_s(mess, "%s add EXP (+%d).", Clients[p_id].m_name, (Clients[m_id].level + 1) * (Clients[m_id].level + 1) * 2);
	}

	player_levelUp(p_id);

	Clients[m_id].m_state = STATE_CONNECTED;
	while (true)
	{
		int x = rand() % WORLD_WIDTH;
		int y = rand() % WORLD_HEIGHT;

		if (Map[y][x])
		{
			Clients[m_id].m_x = x;
			Clients[m_id].m_y = y;
			break;
		}
	}

	

	send_chat(p_id, p_id, mess);
	send_stat_change(p_id, m_id);
	send_pc_logout(p_id, m_id);

	for (auto vlPlayer : Clients[p_id].m_viewlist)
	{
		send_chat(m_id, p_id, mess);
		send_stat_change(vlPlayer, m_id);
		send_pc_logout(vlPlayer, m_id);
	}

	g_ObjectListSector[Clients[m_id].m_x][Clients[m_id].m_y].m_sl.lock();

	g_ObjectListSector[Clients[m_id].m_x][Clients[m_id].m_y].m_client.erase(&Clients[m_id]);

	g_ObjectListSector[Clients[m_id].m_x][Clients[m_id].m_y].m_sl.unlock();

	

	add_event(m_id, OP_NPC_RESPAWN, 30'000);
}

void do_attack_npc(int m_id, int p_id)
{

	SESSION& player = Clients[p_id];
	SESSION& monster = Clients[m_id];

	if (player.hp == 100)
	{
		add_event(p_id, OP_PLAYER_HP_RECOVER, 5000);
	}

	player.m_lock.lock();
	player.hp -= 5;

	//cout << player.hp << "\t";

	unordered_set<int> vl = Clients[p_id].m_viewlist;
	player.m_lock.unlock();

	char mess[100];
	sprintf_s(mess, "%s : attack -> %s (-%d).", monster.m_name, player.m_name, player.level * 2);


	send_chat(p_id, p_id, mess);

	for (auto vlPlayer : vl)
		send_chat(vlPlayer, p_id, mess);

	if (player.hp <= 0)
	{
		player.exp /= 2;
		player.hp = 100;

		player.m_x = start_x;
		player.m_y = start_y;

		send_stat_change(p_id, p_id);
		send_move_packet(p_id, p_id);
		for (auto vlPlayer : vl)
		{
			send_stat_change(vlPlayer, p_id);
			send_move_packet(vlPlayer, p_id);
		}



		char mess1[100];
		sprintf_s(mess1, "!!! RESPAWN !!!");
		send_chat(p_id, p_id, mess1);


	}
	else
	{
		send_stat_change(p_id, p_id);
		for (auto vlPlayer : vl)
			send_stat_change(vlPlayer, p_id);
	}

	//send_stat_change(p_id, p_id);



}

void player_attack(int m_id, int p_id)
{
	SESSION& player = Clients[p_id];
	SESSION& monster = Clients[m_id];

	player.m_vl.lock();
	unordered_set<int> vl = player.m_viewlist;
	player.m_vl.unlock();

	monster.hp -= (10 * (player.level + 1));

	//cout << "attack npc\n";

	if (monster.hp <= 0)
		npc_dead(p_id, m_id);

	char mess[100];
	sprintf_s(mess, "%s : attack -> %s (-%d).", player.m_name, monster.m_name, (player.level + 1) * 5);

	send_chat(p_id, p_id, mess);
	send_stat_change(p_id, m_id);

	for (auto vlPlayer : vl)
	{
		send_chat(m_id, p_id, mess);
		send_stat_change(vlPlayer, m_id);
	}
}

void do_random_move_npc(int id)
{

	unordered_set<int> old_vl = get_nearVl(id);

	int x = Clients[id].m_x;
	int y = Clients[id].m_y;


	if (Clients[id].npcMove == ROAMING) {
		//로밍몬스터만 움직이기
		switch (rand() % 4) {
		case 0: if (x > 0) x--; break;
		case 1: if (x < (WORLD_WIDTH - 1))x++; break;
		case 2:  if (y > 0)y--; break;
		case 3: if (y < (WORLD_HEIGHT - 1)) y++; break;
		}
		if (Map[y][x]) {

			Clients[id].m_x = x;
			Clients[id].m_y = y;
		}
	}

	//g_ObjectListSector[x][y].m_sl.lock();

	//g_ObjectListSector[players[id].m_x][players[id].m_y].m_client.erase(&players[id]);
	//g_ObjectListSector[x][y].m_client.emplace(&u);

	//g_ObjectListSector[x][y].m_sl.unlock();



	unordered_set <int> new_vl = get_nearVl(id);



	for (auto& pl : new_vl) {
		if (true == is_npc(pl)) continue;
		if (STATE_INGAME != Clients[pl].m_state) continue;
		Clients[pl].m_vl.lock();


		if (attack_range(id, pl))
			do_attack_npc(id, pl);



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

	if (Map[y][x]) {

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

			if (STATE_INGAME == Clients[vl_id].m_state || STATE_ACTIVE == Clients[vl_id].m_state)
			{
				if (true == is_npc(vl_id))
				{
					EX_OVER* ex_over = new EX_OVER;
					ex_over->m_op = OP_PLAYER_MOVE;
					ex_over->m_target_id = p_id;
					PostQueuedCompletionStatus(h_iocp, 1, vl_id, &ex_over->m_over);

				}
			}
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

				/*
				if (true == is_npc(pl) && Clients[pl].npcType == AGRO)
				{
					if (Clients[pl].target == nullptr)
					{
						Clients[pl].target = &Clients[p_id];
						//if (Clients[pl].npcMove != ROAMING)
							//do_random_move_npc(pl);

					}
					continue;
				}
				*/

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

				if (true == is_npc(pl))
				{
					//if (pl >= NPC_ID_START)
						//Clients[pl].target = nullptr;

					continue;
				}

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

		if (p->id != 9999)
		{
			bool use_id = false;

			for (int i = 0; i < NPC_ID_START; ++i)
			{
				if (Clients[i].login_id == p->id && Clients[i].m_state == STATE_INGAME)
				{
					use_id = true;
					break;
				}
			}
			if (use_id == true)
			{
				send_login_fail_packet(p_id);
				closesocket(Clients[p_id].m_s);
				//disconnect(p_id);
				break;

			}


			Clients[p_id].login_id = p->id;

			//cout << "login id: " << Clients[p_id].login_id << endl;
		}
		else
			Clients[p_id].login_id = p_id;

		if (!find_db(p_id))
		{
			int mx;
			int my;
			while (true)
			{
				mx = rand() % WORLD_WIDTH;
				my = rand() % WORLD_HEIGHT;

				if (Map[my][mx])
					break;
			}

			Clients[p_id].m_x = mx;
			Clients[p_id].m_y = my;

			start_x = mx;
			start_y = my;
			Clients[p_id].exp = 0;
			Clients[p_id].hp = 100;
			Clients[p_id].level = 1;



			InsertData(Clients[p_id].login_id, Clients[p_id].m_name, Clients[p_id].level, Clients[p_id].hp, Clients[p_id].exp, Clients[p_id].m_x, Clients[p_id].m_y);
		}
		else
		{
			start_x = Clients[p_id].m_x;
			start_y = Clients[p_id].m_y;
		}



		send_login_info(p_id);
		Clients[p_id].m_state = STATE_INGAME;


		g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_sl.lock();

		g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_client.emplace(&Clients[p_id]);

		g_ObjectListSector[Clients[p_id].m_x][Clients[p_id].m_y].m_sl.unlock();

		if (Clients[p_id].hp < 100)
			add_event(p_id, OP_PLAYER_HP_RECOVER, 5000);

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
				/*
				else
				{
					//플레이어가 npc 시야에 들어왔을때
					if (Clients[i].npcType == AGRO)
					{
						if (Clients[i].target == nullptr)
						{
							Clients[i].target = &Clients[p_id];
							//if (Clients[i].npcMove != ROAMING)
								//do_random_move_npc(i);

						}
					}
				}
				*/

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
				send_chat(pl, p_id, chat_packet->message); //상대방에게
		}
		send_chat(p_id, p_id, chat_packet->message); //나에게
	}
				break;
	case CS_ATTACK:
	{
		cs_packet_attack* chat_packet = reinterpret_cast<cs_packet_attack*>(packet);

		SESSION& player = Clients[p_id];

		player.m_vl.lock();
		unordered_set<int> vl = player.m_viewlist;
		player.m_vl.unlock();

		for (int vl_id : vl)
		{
			if (Clients[vl_id].m_id < NPC_ID_START)
				continue;

			if ((Clients[vl_id].m_x == player.m_x && Clients[vl_id].m_y == player.m_y + 1) ||
				(Clients[vl_id].m_x == player.m_x && Clients[vl_id].m_y == player.m_y - 1) ||
				(Clients[vl_id].m_x == player.m_x + 1 && Clients[vl_id].m_y == player.m_y) ||
				(Clients[vl_id].m_x == player.m_x - 1 && Clients[vl_id].m_y == player.m_y))
			{

				player_attack(vl_id, p_id);
			}
		}
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

int API_get_x(lua_State* L)
{
	int obj_id = lua_tonumber(L, -1);
	lua_pop(L, 2);
	int x = Clients[obj_id].m_x;
	lua_pushnumber(L, x);
	return 1;
}

int API_get_y(lua_State* L)
{
	int obj_id = lua_tonumber(L, -1);
	lua_pop(L, 2);
	int y = Clients[obj_id].m_y;
	lua_pushnumber(L, y);
	return 1;
}

int API_send_message(lua_State* L)
{

	int obj_id = lua_tonumber(L, -3);
	int p_id = lua_tonumber(L, -2);
	const char* mess = lua_tostring(L, -1);
	send_chat(p_id, obj_id, mess);
	lua_pop(L, 3);

	return 0;
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

		int mx;
		int my;
		while (true)
		{
			mx = rand() % WORLD_WIDTH;
			my = rand() % WORLD_HEIGHT;

			if (Map[my][mx])
				break;
		}

		npc.m_x = mx;
		npc.m_y = my;

		npc.level = rand() % 5 + 1;
		npc.hp = 100;

		//npc move
		if (0 == rand() % 2)
			npc.npcMove = FIX;
		else
			npc.npcMove = ROAMING;

		//npc move
		if (0 == rand() % 2)
			npc.npcType = PEACE;
		else
			npc.npcType = AGRO;

		if (ROAMING == npc.npcMove)
			add_event(i, OP_RANDOM_MOVE, 1000);

		
		if (npc.npcMove == ROAMING)
		{
			npc.L = luaL_newstate();
			luaL_openlibs(npc.L);
			luaL_loadfile(npc.L, "monster_ai.lua");
			int error = lua_pcall(npc.L, 0, 0, 0);
			if (error) cout << lua_tostring(npc.L, -1);

			lua_getglobal(npc.L, "set_o_id");
			lua_pushnumber(npc.L, i);
			error = lua_pcall(npc.L, 1, 0, 0);
			if (error) cout << lua_tostring(npc.L, -1);

			lua_register(npc.L, "API_send_message", API_send_message);
			lua_register(npc.L, "API_get_x", API_get_x);
			lua_register(npc.L, "API_get_y", API_get_y);
		}
		/*
		
		*/

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

	UpdateData(Clients[p_id].login_id, Clients[p_id].level, Clients[p_id].hp, Clients[p_id].exp, Clients[p_id].m_x, Clients[p_id].m_y);

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

	/*
	for (auto vl : Clients[p_id].m_viewlist)
	{
		Clients[vl].target = nullptr;
	}
	*/
	Clients[p_id].m_state = STATE_FREE;
	Clients[p_id].m_lock.unlock();


}

void do_timer()
{
	using namespace chrono;

	while (true) {

		this_thread::sleep_for(1ms); //너무 빨리 움직여서 조정

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
				case OP_PLAYER_HP_RECOVER:
				{
					EX_OVER* ex_over = new EX_OVER;
					ex_over->m_op = OP_PLAYER_HP_RECOVER;
					PostQueuedCompletionStatus(h_iocp, 1, ev.object_id, &ex_over->m_over);
				}
				break;
				case OP_NPC_RESPAWN:
				{
					EX_OVER* ex_over = new EX_OVER;
					ex_over->m_op = OP_NPC_RESPAWN;
					PostQueuedCompletionStatus(h_iocp, 1, ev.object_id, &ex_over->m_over);
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
			//cout << "연결종료\n";
			int err = WSAGetLastError();
			display_error("GQCS : ", err);
			disconnect(key);
			continue;
		}

		EX_OVER* ex_over = reinterpret_cast<EX_OVER*>(over);

		switch (ex_over->m_op) {
		case OP_RECV:
		{

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


			do_recv(p_id);
			do_accept(listenSocket, ex_over);
			//cout << "New CLient [" << p_id << "] connected.\n";
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
		case OP_PLAYER_MOVE:
		{
			if (Clients[key].npcMove == ROAMING)
			{
				Clients[key].m_sl.lock();
				lua_State* L = Clients[key].L;
				lua_getglobal(L, "event_player_move");
				lua_pushnumber(L, ex_over->m_target_id);
				int error = lua_pcall(L, 1, 0, 0);
				if (error) cout << lua_tostring(L, -1);
				Clients[key].m_sl.unlock();
			}
			

			delete ex_over;
		}
		break;
		case OP_PLAYER_HP_RECOVER:
		{
			Clients[key].hp += (Clients[key].hp / 10);

			if (Clients[key].hp >= 100)
				Clients[key].hp = 100;
			else if (Clients[key].hp < 100)
				add_event(key, OP_PLAYER_HP_RECOVER, 5000);

			send_stat_change(key, key);

			Clients[key].m_lock.lock();
			unordered_set<int> vl = Clients[key].m_viewlist;
			Clients[key].m_lock.unlock();

			for (auto vlplayer : vl)
			{
				send_stat_change(vlplayer, key);
			}

		}
		break;
		case OP_NPC_RESPAWN:
		{
			Clients[key].hp = 100;
			
			g_ObjectListSector[Clients[key].m_x][Clients[key].m_y].m_sl.lock();

			g_ObjectListSector[Clients[key].m_x][Clients[key].m_y].m_client.emplace(&Clients[key]);

			g_ObjectListSector[Clients[key].m_x][Clients[key].m_y].m_sl.unlock();

			add_event(key, OP_RANDOM_MOVE, 1000); 

			//cout << "respawn\n";
			Clients[key].m_state = STATE_SLEEP;

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
	//g_Astar = new Astar(Map);

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
